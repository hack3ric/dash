#pragma once
/*
We do several optimization and correctness patches for CCEH, including:
(1) remove fence between storing value and storing key during insert() because
these two stores are in the same cacheline and will mot be reordered. (2) remove
bucket-level lock described in their original paper since frequent
lock/unlocking will severly degrade its performance (actually their original
open-sourced code also does not have bucket-level lock). (3) add epoch manager
in the application level (mini-benchmark) to gurantee correct memory
reclamation. (4) avoid the perssitent memory leak during the segment split by
storing the newly allocated segment in a small preallocated area (organized as a
hash table). (5) add uniqnuess check during the insert opeartion to avoid
inserting duplicate keys. (6) add support for variable-length key by storing the
pointer to the key object. (7) use persistent lock in PMDK library to aovid
deadlock caused by sudden system failure.
*/
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <shared_mutex>

#include "../../util/hash.h"
#include "../../util/pair.h"
#include "../../util/utils.h"
#include "../Hash.h"
#include "../allocator_new.h"

constexpr bool kInplace = true;
constexpr bool kUseEpoch = false;
constexpr size_t kLogNum = 1024;

namespace cceh {

enum class InsertResult : int { Success, Duplicate, NeedSplit, Redirect };
enum class SplitInsertResult : int { Success, Full };

template <class T>
struct Pair {
  T key;
  Value_t value;
};

template <class T>
struct Segment;

template <class T>
struct log_entry {
  uint64_t lock = 0;
  Segment<T>* temp = nullptr;

  void Lock_log() {
    uint64_t temp_val = 0;
    while (!CAS(&lock, &temp_val, 1)) {
      temp_val = 0;
    }
  }

  void Unlock_log() { lock = 0; }
};

// const size_t kCacheLineSize = 64;
constexpr size_t kSegmentBits = 8;
constexpr size_t kMask = (1 << kSegmentBits) - 1;
constexpr size_t kShift = kSegmentBits;
constexpr size_t kSegmentSize = (1 << kSegmentBits) * 16 * 4;
constexpr size_t kNumPairPerCacheLine = kCacheLineSize / 16;
constexpr size_t kNumCacheLine = 4;

// uint64_t clflushCount;

inline bool var_compare(const char* str1, const char* str2, size_t len1, size_t len2) {
  if (len1 != len2) return false;
  return !memcmp(str1, str2, len1);
}

template <class T>
struct Segment {
  static const size_t kNumSlot = kSegmentSize / sizeof(Pair<T>);

  Segment(void) : local_depth{0}, sema{0}, count{0}, seg_lock{0}, mutex() {
    memset(static_cast<void*>(slots_), 255, sizeof(Pair<T>) * kNumSlot);
  }

  Segment(size_t depth)
      : local_depth{depth}, sema{0}, count{0}, seg_lock{0}, mutex() {
    memset(static_cast<void*>(slots_), 255, sizeof(Pair<T>) * kNumSlot);
  }

  ~Segment() = default;

  InsertResult Insert(T, Value_t, size_t, size_t);
  SplitInsertResult Insert4split(T, Value_t, size_t);
  bool Put(T, Value_t, size_t);
  Segment<T>** Split(size_t, log_entry<T>*);

  void release_lock() { mutex.unlock(); }

  bool try_get_lock() { return mutex.try_lock(); }

  Pair<T> slots_[kNumSlot];
  size_t local_depth;
  int64_t sema = 0;
  size_t pattern = 0;
  size_t count = 0;
  std::shared_mutex mutex;
  uint64_t seg_lock;
};

template <class T>
struct Seg_array {
  using seg_p = Segment<T>*;
  size_t global_depth;

  seg_p* entries() {
    return reinterpret_cast<seg_p*>(reinterpret_cast<char*>(this) +
                                    sizeof(Seg_array));
  }
};

template <class T>
struct Directory {
  static const size_t kDefaultDirectorySize = 1024;
  std::unique_ptr<Seg_array<T>, Allocator::Deleter<Seg_array<T>>> sa;
  std::unique_ptr<Seg_array<T>, Allocator::Deleter<Seg_array<T>>> new_sa;
  size_t capacity;
  bool lock;
  int sema = 0;

  Directory(Seg_array<T>* _sa) {
    capacity = kDefaultDirectorySize;
    sa.reset(_sa);
    new_sa = nullptr;
    lock = false;
    sema = 0;
  }

  Directory(size_t size, Seg_array<T>* _sa) {
    capacity = size;
    sa.reset(_sa);
    new_sa = nullptr;
    lock = false;
    sema = 0;
  }

  ~Directory() = default;

  void get_item_num() {
    size_t count = 0;
    size_t seg_num = 0;
    Seg_array<T>* seg = sa.get();
    Segment<T>** dir_entry = seg->entries();
    Segment<T>* ss;
    auto global_depth = seg->global_depth;
    size_t depth_diff;
    for (size_t i = 0; i < capacity;) {
      ss = dir_entry[i];
      depth_diff = global_depth - ss->local_depth;

      for (size_t i = 0; i < Segment<T>::kNumSlot; ++i) {
        if constexpr (std::is_pointer_v<T>) {
          if ((ss->slots_[i].key !=   (T)INVALID) &&
              ((h(ss->slots_[i].key->key, ss->slots_[i].key->length) >>
                (64 - ss->local_depth)) == ss->pattern)) {
            ++count;
          }
        } else {
          if ((ss->slots_[i].key !=   (T)INVALID) &&
              ((h(&ss->slots_[i].key, sizeof(Key_t)) >> (64 - ss->local_depth)) ==
               ss->pattern)) {
            ++count;
          }
        }
      }

      seg_num++;
      i += pow(2, depth_diff);
    }
    std::cout << "#items: " << count << std::endl;
    std::cout << "load_factor: " << (double)count / (seg_num * 256 * 4)
              << std::endl;
  }

  bool Acquire(void) {
    bool unlocked = false;
    return CAS(&lock, &unlocked, true);
  }

  bool Release(void) {
    bool locked = true;
    return CAS(&lock, &locked, false);
  }

  void SanityCheck(void*);
};

template <class T>
struct DirectoryGuard {
  Directory<T>* dir_;
  DirectoryGuard(Directory<T>* d) : dir_(d) {
    while (!d->Acquire()) {
      _mm_pause();
    }
  }
  ~DirectoryGuard() {
    while (!dir_->Release()) {
      _mm_pause();
    }
  }
  DirectoryGuard(const DirectoryGuard&) = delete;
  DirectoryGuard& operator=(const DirectoryGuard&) = delete;
};

template <class T>
class CCEH : public Hash<T> {
 public:
  CCEH(void);
  CCEH(size_t);
  ~CCEH(void);
  int Insert(T key, Value_t value);
  int Insert(T key, Value_t value, bool);
  bool Delete(T);
  bool Delete(T, bool);
  bool Get(T, Value_t*);
  bool Get(T key, Value_t*, bool is_in_epoch);
  Value_t FindAnyway(T);
  [[nodiscard]] double Utilization(void);
  [[nodiscard]] size_t Capacity(void);
  void Directory_Doubling(size_t x, Segment<T>* s0, Segment<T>** s1);
  void Directory_Update(size_t x, Segment<T>* s0, Segment<T>** s1);
  void TX_Swap(void** entry, Segment<T>** new_seg);
  void getNumber() { dir->get_item_num(); }

 private:
  std::unique_ptr<Directory<T>, Allocator::Deleter<Directory<T>>> dir;
  std::array<log_entry<T>, kLogNum> log;
  size_t seg_num;
  size_t restart;
};
// #endif  // EXTENDIBLE_PTR_H_

template <class T>
InsertResult Segment<T>::Insert(T key, Value_t value, size_t loc,
                                size_t key_hash) {
  if (sema == -1) {
    return InsertResult::Redirect;
  }
  std::unique_lock<std::shared_mutex> lock(mutex);
  if ((key_hash >> (64 - local_depth)) != pattern ||
      sema == -1) {
    return InsertResult::Redirect;
  }
  auto ret = InsertResult::NeedSplit;
  T LOCK =   (T)INVALID;

  /*uniqueness check*/
  auto slot = loc;
  for (size_t i = 0; i < kNumCacheLine * kNumPairPerCacheLine; ++i) {
    slot = (loc + i) % kNumSlot;
    if constexpr (std::is_pointer_v<T>) {
      if (slots_[slot].key !=   (T)INVALID &&
          (var_compare(key->key, slots_[slot].key->key, key->length,
                       slots_[slot].key->length))) {
        return InsertResult::Duplicate;
      }
    } else {
      if (slots_[slot].key == key) {
        return InsertResult::Duplicate;
      }
    }
  }

  for (size_t i = 0; i < kNumPairPerCacheLine * kNumCacheLine; ++i) {
    slot = (loc + i) % kNumSlot;
    if constexpr (std::is_pointer_v<T>) {
      if ((slots_[slot].key !=   (T)INVALID) &&
          ((h(slots_[slot].key->key, slots_[slot].key->length) >>
            (64 - local_depth)) != pattern)) {
        slots_[slot].key =   (T)INVALID;
      }
      if (CAS(&slots_[slot].key, &LOCK, SENTINEL)) {
        slots_[slot].value = value;
        slots_[slot].key = key;
        ret = InsertResult::Success;
        break;
      } else {
        LOCK =   (T)INVALID;
      }
    } else {
      if ((h(&slots_[slot].key, sizeof(Key_t)) >>
           (64 - local_depth)) != pattern) {
        slots_[slot].key = INVALID;
      }
      if (CAS(&slots_[slot].key, &LOCK, SENTINEL)) {
        slots_[slot].value = value;
        slots_[slot].key = key;
        ret = InsertResult::Success;
        break;
      } else {
        LOCK = INVALID;
      }
    }
  }
  return ret;
}

template <class T>
SplitInsertResult Segment<T>::Insert4split(T key, Value_t value, size_t loc) {
  for (size_t i = 0; i < kNumPairPerCacheLine * kNumCacheLine; ++i) {
    auto slot = (loc + i) % kNumSlot;
    if (slots_[slot].key ==   (T)INVALID) {
      slots_[slot].key = key;
      slots_[slot].value = value;
      return SplitInsertResult::Success;
    }
  }
  return SplitInsertResult::Full;
}

template <class T>
Segment<T>** Segment<T>::Split(size_t key_hash, log_entry<T>* log) {
  if (!try_get_lock()) {
    return nullptr;
  }
  sema = -1;

  size_t new_pattern = (pattern << 1) + 1;
  size_t old_pattern = pattern << 1;

  size_t log_pos = key_hash % kLogNum;
  log[log_pos].Lock_log();

  log[log_pos].temp =
      Allocator::New<Segment<T>>(kCacheLineSize, 0, local_depth + 1);
  Segment<T>* split = log[log_pos].temp;

  for (size_t i = 0; i < kNumSlot; ++i) {
    size_t key_hash;
    if constexpr (std::is_pointer_v<T>) {
      if (slots_[i].key !=   (T)INVALID) {
        key_hash = h(slots_[i].key->key, slots_[i].key->length);
      }
    } else {
      key_hash = h(&slots_[i].key, sizeof(Key_t));
    }
    if ((slots_[i].key !=   (T)INVALID) &&
        (key_hash >> (64 - local_depth - 1) == new_pattern)) {
      split->Insert4split(slots_[i].key, slots_[i].value,
                          (key_hash & kMask) * kNumPairPerCacheLine);
      if constexpr (std::is_pointer_v<T>) {
        slots_[i].key =   (T)INVALID;
      }
    }
  }

  return &log[log_pos].temp;
}

template <class T>
CCEH<T>::CCEH(size_t initCap) {
  dir = Allocator::MakeUnique<Directory<T>>(kCacheLineSize, 0, initCap, nullptr);

  auto new_sa =
      Allocator::New<Seg_array<T>>(kCacheLineSize,
                                   sizeof(typename Seg_array<T>::seg_p) * initCap);
  new_sa->global_depth = static_cast<size_t>(log2(initCap));
  memset(new_sa->entries(), 0, initCap * sizeof(typename Seg_array<T>::seg_p));
  dir->sa.reset(new_sa);

  auto dir_entry = dir->sa->entries();
  for (size_t i = 0; i < dir->capacity; ++i) {
    dir_entry[i] = Allocator::New<Segment<T>>(kCacheLineSize, 0,
                                               dir->sa->global_depth);
    dir_entry[i]->pattern = i;
  }
  /*clear the log area*/
  for (size_t i = 0; i < kLogNum; ++i) {
    log[i].lock = 0;
    log[i].temp = nullptr;
  }

  seg_num = 0;
  restart = 0;
}

template <class T>
CCEH<T>::CCEH(void) {
  std::cout << "Reintialize Up for CCEH" << std::endl;
}

template <class T>
CCEH<T>::~CCEH() = default;

template <class T>
void CCEH<T>::TX_Swap(void** entry, Segment<T>** new_seg) {
  *entry = *new_seg;
  *new_seg = nullptr;
}

template <class T>
void CCEH<T>::Directory_Doubling(size_t x, Segment<T>* s0, Segment<T>** s1) {
  auto* sa = dir->sa.get();
  Segment<T>** d = sa->entries();
  auto global_depth = sa->global_depth;

  size_t new_capacity = 2 * dir->capacity;
  auto new_seg_array =
      Allocator::New<Seg_array<T>>(kCacheLineSize,
                                   sizeof(typename Seg_array<T>::seg_p) * new_capacity);
  new_seg_array->global_depth = static_cast<size_t>(log2(new_capacity));
  memset(new_seg_array->entries(), 0,
         new_capacity * sizeof(typename Seg_array<T>::seg_p));
  dir->new_sa.reset(new_seg_array);
  auto dd = dir->new_sa->entries();

  for (size_t i = 0; i < dir->capacity; ++i) {
    dd[2 * i] = d[i];
    dd[2 * i + 1] = d[i];
  }

  TX_Swap(reinterpret_cast<void**>(&dd[2 * x + 1]), s1);

  dir->sa = std::move(dir->new_sa);
  dir->capacity *= 2;
}

template <class T>
void CCEH<T>::Directory_Update(size_t x, Segment<T>* s0, Segment<T>** s1) {
  Segment<T>** dir_entry = dir->sa->entries();
  auto global_depth = dir->sa->global_depth;
    auto depth_diff = global_depth - s0->local_depth;
    if (depth_diff == 1) {
      if (x % 2 == 0) {
        TX_Swap(reinterpret_cast<void**>(&dir_entry[x + 1]), s1);
      } else {
        TX_Swap(reinterpret_cast<void**>(&dir_entry[x]), s1);
      }
    } else {
      size_t chunk_size = pow(2, global_depth - (s0->local_depth));
      x = x - (x % chunk_size);
      size_t base = chunk_size / 2;
    TX_Swap(reinterpret_cast<void**>(&dir_entry[x + base + base - 1]), s1);
    auto seg_ptr = dir_entry[x + base + base - 1];
    for (int i = base - 2; i >= 0; --i) {
      dir_entry[x + base + i] = seg_ptr;
    }
  }
}

template <class T>
int CCEH<T>::Insert(T key, Value_t value, bool is_in_epoch) {
  if (!is_in_epoch) {
    if constexpr (kUseEpoch) {
      auto epoch_guard = Allocator::AquireEpochGuard();
    }
    return Insert(key, value);
  }
  return Insert(key, value);
}

template <class T>
int CCEH<T>::Insert(T key, Value_t value) {
  for (;;) {
    size_t key_hash;
    if constexpr (std::is_pointer_v<T>) {
      key_hash = h(key->key, key->length);
    } else {
      key_hash = h(&key, sizeof(key));
    }
    auto y = (key_hash & kMask) * kNumPairPerCacheLine;

    for (;;) {
      auto old_sa = dir->sa.get();
      if (old_sa != dir->sa.get()) {
        continue;
      }
      auto x = (key_hash >> (64 - old_sa->global_depth));
      auto dir_entry = old_sa->entries();
      Segment<T>* target = dir_entry[x];

      auto ret = target->Insert(key, value, y, key_hash);

      if (ret == InsertResult::Duplicate) return -1;

      if (ret == InsertResult::NeedSplit) {
        auto s = target->Split(key_hash, log.data());
        if (s == nullptr) {
          continue;
        }

        auto ss = *s;
        ss->pattern =
            ((key_hash >> (64 - ss->local_depth + 1)) << 1) +
            1;

        // Directory management
        {
          DirectoryGuard dir_guard(dir.get());
          auto sa = dir->sa.get();
          dir_entry = sa->entries();

          x = (key_hash >> (64 - sa->global_depth));
          target = dir_entry[x];
          if (target->local_depth < sa->global_depth) {
            Directory_Update(x, target, s);
          } else {  // directory doubling
            Directory_Doubling(x, target, s);
          }
          target->pattern =
              (key_hash >> (64 - target->local_depth)) << 1;
          target->local_depth += 1;
          if constexpr (kInplace) {
            target->sema = 0;
            target->release_lock();
          }
        }  // DirectoryGuard released here
        size_t log_pos = key_hash % kLogNum;
        log[log_pos].Unlock_log();
        continue;
      } else if (ret == InsertResult::Redirect) {
        break;  // restart outer loop with fresh key_hash
      }

      return 0;
    }
  }
}

template <class T>
bool CCEH<T>::Delete(T key, bool is_in_epoch) {
  if (!is_in_epoch) {
    if constexpr (kUseEpoch) {
      auto epoch_guard = Allocator::AquireEpochGuard();
    }
    return Delete(key);
  }
  return Delete(key);
}

template <class T>
bool CCEH<T>::Delete(T key) {
  uint64_t key_hash;
  if constexpr (std::is_pointer_v<T>) {
    key_hash = h(key->key, key->length);
  } else {
    key_hash = h(&key, sizeof(key));
  }
  auto y = (key_hash & kMask) * kNumPairPerCacheLine;

  for (;;) {
    auto old_sa = dir->sa.get();
    if (old_sa != dir->sa.get()) {
      continue;
    }
    auto x = (key_hash >> (64 - old_sa->global_depth));
    auto dir_entry = old_sa->entries();
    Segment<T>* dir_ = dir_entry[x];

    auto sema = dir_->sema;
    if (sema == -1) {
      continue;
    }
    std::unique_lock<std::shared_mutex> lock(dir_->mutex);

    if ((key_hash >> (64 - dir_->local_depth)) !=
            dir_->pattern ||
        dir_->sema == -1) {
      continue;
    }

    for (size_t i = 0; i < kNumPairPerCacheLine * kNumCacheLine; ++i) {
      auto slot = (y + i) % Segment<T>::kNumSlot;
      if constexpr (std::is_pointer_v<T>) {
        if ((dir_->slots_[slot].key !=   (T)INVALID) &&
            (var_compare(key->key, dir_->slots_[slot].key->key, key->length,
                         dir_->slots_[slot].key->length))) {
          dir_->slots_[slot].key =   (T)INVALID;
          return true;
        }
      } else {
        if (dir_->slots_[slot].key == key) {
          dir_->slots_[slot].key =   (T)INVALID;
          return true;
        }
      }
    }
    return false;
  }
}

template <class T>
bool CCEH<T>::Get(T key, Value_t* value, bool is_in_epoch) {
  if (is_in_epoch) {
    if constexpr (kUseEpoch) {
      auto epoch_guard = Allocator::AquireEpochGuard();
    }
    return Get(key, value);
  }
  return Get(key, value);
}

template <class T>
bool CCEH<T>::Get(T key, Value_t* value_) {
  uint64_t key_hash;
  if constexpr (std::is_pointer_v<T>) {
    key_hash = h(key->key, key->length);
  } else {
    key_hash = h(&key, sizeof(key));
  }
  auto y = (key_hash & kMask) * kNumPairPerCacheLine;

  for (;;) {
    auto old_sa = dir->sa.get();
    if (old_sa != dir->sa.get()) {
      continue;
    }
    auto x = (key_hash >> (64 - old_sa->global_depth));
    auto dir_entry = old_sa->entries();
    Segment<T>* dir_ = dir_entry[x];

    std::shared_lock<std::shared_mutex> lock(dir_->mutex, std::try_to_lock);
    if (!lock) {
      continue;
    }

    if ((key_hash >> (64 - dir_->local_depth)) !=
            dir_->pattern ||
        dir_->sema == -1) {
      continue;
    }

    for (size_t i = 0; i < kNumPairPerCacheLine * kNumCacheLine; ++i) {
      auto slot = (y + i) % Segment<T>::kNumSlot;
      if constexpr (std::is_pointer_v<T>) {
        if ((dir_->slots_[slot].key !=   (T)INVALID) &&
            (var_compare(key->key, dir_->slots_[slot].key->key, key->length,
                         dir_->slots_[slot].key->length))) {
          auto value = dir_->slots_[slot].value;
          *value_ = value;
          return true;
        }
      } else {
        if (dir_->slots_[slot].key == key) {
          auto value = dir_->slots_[slot].value;
          *value_ = value;
          return true;
        }
      }
    }

    return false;
  }
}
}  // namespace cceh
