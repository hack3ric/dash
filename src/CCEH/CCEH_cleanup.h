#pragma once

#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <shared_mutex>

#include "../../util/hash.h"
#include "../../util/pair.h"
#include "../../util/utils.h"
#include "../Hash.h"
#include "../allocator_new.h"

constexpr bool inplace = true;
constexpr bool use_epoch = false;
constexpr size_t log_capacity = 1024;

namespace cceh {

enum class InsertResult : int { Success, Duplicate, NeedSplit, Redirect };
enum class SplitInsertResult : int { Success, Full };

// TODO: use <K, V>; the std::is_pointer<T> path actually forces the usage of a
// specific custom type `string_key*`
template <class T>
struct Pair {
  T key;
  Value_t value;
};

template <class T>
struct Segment;

template <class T>
struct LogEntry {
  uint64_t lock = 0;
  Segment<T>* pending = nullptr;

  void LockLog() {
    uint64_t expected = 0;
    while (!CAS(&lock, &expected, 1)) {
      expected = 0;
    }
  }

  void UnlockLog() { lock = 0; }
};

constexpr size_t segment_bits = 8;
constexpr size_t segment_mask = (1 << segment_bits) - 1;
constexpr size_t segment_size = (1 << segment_bits) * 16 * 4;
constexpr size_t pairs_per_cacheline = kCacheLineSize / 16;
constexpr size_t num_cache_lines = 4;

inline bool VarEqual(const char* str1, const char* str2, size_t len1,
                     size_t len2) {
  if (len1 != len2) return false;
  return !memcmp(str1, str2, len1);
}

template <class T>
struct Segment {
  static const size_t num_slots = segment_size / sizeof(Pair<T>);

  Segment() : local_depth{0}, sema{0}, count{0}, seg_lock{0}, mutex() {
    memset(static_cast<void*>(slots_), 255, sizeof(Pair<T>) * num_slots);
  }

  Segment(size_t depth)
      : local_depth{depth}, sema{0}, count{0}, seg_lock{0}, mutex() {
    memset(static_cast<void*>(slots_), 255, sizeof(Pair<T>) * num_slots);
  }

  ~Segment() = default;

  InsertResult Insert(T, Value_t, size_t, size_t);
  SplitInsertResult InsertForSplit(T, Value_t, size_t);
  bool Put(T, Value_t, size_t);
  Segment<T>** Split(size_t, LogEntry<T>*);

  void ReleaseLock() { mutex.unlock(); }

  bool TryGetLock() { return mutex.try_lock(); }

  Pair<T> slots_[num_slots];
  size_t local_depth;
  int64_t sema = 0;
  size_t pattern = 0;
  size_t count = 0;
  std::shared_mutex mutex;
  uint64_t seg_lock;
};

template <class T>
struct SegArray {
  using SegPtr = Segment<T>*;
  size_t global_depth;

  SegPtr* entries() {
    return reinterpret_cast<SegPtr*>(reinterpret_cast<char*>(this) +
                                     sizeof(SegArray));
  }
};

template <class T>
struct Directory {
  static const size_t default_dir_size = 1024;
  Allocator::uptr<SegArray<T>> sa;
  Allocator::uptr<SegArray<T>> new_sa;
  size_t capacity;
  bool lock;
  int sema = 0;

  Directory(SegArray<T>* _sa) {
    capacity = default_dir_size;
    sa.reset(_sa);
    new_sa = nullptr;
    lock = false;
    sema = 0;
  }

  Directory(size_t size, SegArray<T>* _sa) {
    capacity = size;
    sa.reset(_sa);
    new_sa = nullptr;
    lock = false;
    sema = 0;
  }

  ~Directory() = default;

  void GetItemCount() {
    size_t count = 0;
    size_t seg_num = 0;
    SegArray<T>* seg_array = sa.get();
    Segment<T>** dir_entry = seg_array->entries();
    Segment<T>* cur_seg;
    auto global_depth = seg_array->global_depth;
    size_t depth_diff;
    for (size_t i = 0; i < capacity;) {
      cur_seg = dir_entry[i];
      depth_diff = global_depth - cur_seg->local_depth;

      for (size_t i = 0; i < Segment<T>::num_slots; ++i) {
        if constexpr (std::is_pointer_v<T>) {
          if ((cur_seg->slots_[i].key != (T)INVALID) &&
              ((h(cur_seg->slots_[i].key->key,
                  cur_seg->slots_[i].key->length) >>
                (64 - cur_seg->local_depth)) == cur_seg->pattern)) {
            ++count;
          }
        } else {
          if ((cur_seg->slots_[i].key != (T)INVALID) &&
              ((h(&cur_seg->slots_[i].key, sizeof(Key_t)) >>
                (64 - cur_seg->local_depth)) == cur_seg->pattern)) {
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

  bool Acquire() {
    bool unlocked = false;
    return CAS(&lock, &unlocked, true);
  }

  bool Release() {
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
  CCEH();
  CCEH(size_t);
  ~CCEH();
  int Insert(T key, Value_t value);
  int Insert(T key, Value_t value, bool);
  bool Delete(T);
  bool Delete(T, bool);
  bool Get(T, Value_t*);
  bool Get(T key, Value_t*, bool is_in_epoch);
  Value_t FindAnyway(T);
  void DirectoryDouble(size_t x, Segment<T>* old_seg, Segment<T>** new_seg);
  void DirectoryUpdate(size_t x, Segment<T>* old_seg, Segment<T>** new_seg);
  void TxSwap(void** entry, Segment<T>** new_seg);
  void GetNumber() { dir->GetItemCount(); }

 private:
  Allocator::uptr<Directory<T>> dir;
  std::array<LogEntry<T>, log_capacity> log;
  size_t seg_num;
  size_t restart;
};

template <class T>
InsertResult Segment<T>::Insert(T key, Value_t value, size_t loc,
                                size_t key_hash) {
  if (sema == -1) {
    return InsertResult::Redirect;
  }
  std::unique_lock<std::shared_mutex> lock(mutex);
  if ((key_hash >> (64 - local_depth)) != pattern || sema == -1) {
    return InsertResult::Redirect;
  }
  auto result = InsertResult::NeedSplit;
  T expected = (T)INVALID;

  /* uniqueness check */
  auto slot = loc;
  for (size_t i = 0; i < num_cache_lines * pairs_per_cacheline; ++i) {
    slot = (loc + i) % num_slots;
    if constexpr (std::is_pointer_v<T>) {
      if (slots_[slot].key != (T)INVALID &&
          (VarEqual(key->key, slots_[slot].key->key, key->length,
                    slots_[slot].key->length))) {
        return InsertResult::Duplicate;
      }
    } else {
      if (slots_[slot].key == key) {
        return InsertResult::Duplicate;
      }
    }
  }

  for (size_t i = 0; i < pairs_per_cacheline * num_cache_lines; ++i) {
    slot = (loc + i) % num_slots;
    if constexpr (std::is_pointer_v<T>) {
      if ((slots_[slot].key != (T)INVALID) &&
          ((h(slots_[slot].key->key, slots_[slot].key->length) >>
            (64 - local_depth)) != pattern)) {
        slots_[slot].key = (T)INVALID;
      }
      if (CAS(&slots_[slot].key, &expected, SENTINEL)) {
        slots_[slot].value = value;
        slots_[slot].key = key;
        result = InsertResult::Success;
        break;
      } else {
        expected = (T)INVALID;
      }
    } else {
      if ((h(&slots_[slot].key, sizeof(Key_t)) >> (64 - local_depth)) !=
          pattern) {
        slots_[slot].key = INVALID;
      }
      if (CAS(&slots_[slot].key, &expected, SENTINEL)) {
        slots_[slot].value = value;
        slots_[slot].key = key;
        result = InsertResult::Success;
        break;
      } else {
        expected = INVALID;
      }
    }
  }
  return result;
}

template <class T>
SplitInsertResult Segment<T>::InsertForSplit(T key, Value_t value, size_t loc) {
  for (size_t i = 0; i < pairs_per_cacheline * num_cache_lines; ++i) {
    auto slot = (loc + i) % num_slots;
    if (slots_[slot].key == (T)INVALID) {
      slots_[slot].key = key;
      slots_[slot].value = value;
      return SplitInsertResult::Success;
    }
  }
  return SplitInsertResult::Full;
}

template <class T>
Segment<T>** Segment<T>::Split(size_t key_hash, LogEntry<T>* log) {
  if (!TryGetLock()) {
    return nullptr;
  }
  sema = -1;

  size_t new_pattern = (pattern << 1) + 1;
  size_t old_pattern = pattern << 1;

  size_t log_pos = key_hash % log_capacity;
  log[log_pos].LockLog();

  log[log_pos].pending =
      Allocator::New<Segment<T>>(kCacheLineSize, 0, local_depth + 1);
  Segment<T>* split = log[log_pos].pending;

  for (size_t i = 0; i < num_slots; ++i) {
    size_t key_hash;
    if constexpr (std::is_pointer_v<T>) {
      if (slots_[i].key != (T)INVALID) {
        key_hash = h(slots_[i].key->key, slots_[i].key->length);
      }
    } else {
      key_hash = h(&slots_[i].key, sizeof(Key_t));
    }
    if ((slots_[i].key != (T)INVALID) &&
        (key_hash >> (64 - local_depth - 1) == new_pattern)) {
      split->InsertForSplit(slots_[i].key, slots_[i].value,
                            (key_hash & segment_mask) * pairs_per_cacheline);
      if constexpr (std::is_pointer_v<T>) {
        slots_[i].key = (T)INVALID;
      }
    }
  }

  return &log[log_pos].pending;
}

template <class T>
CCEH<T>::CCEH(size_t initial_capacity) {
  dir = Allocator::MakeUnique<Directory<T>>(kCacheLineSize, 0, initial_capacity,
                                            nullptr);

  auto new_sa = Allocator::New<SegArray<T>>(
      kCacheLineSize, sizeof(typename SegArray<T>::SegPtr) * initial_capacity);
  new_sa->global_depth = static_cast<size_t>(log2(initial_capacity));
  memset(new_sa->entries(), 0,
         initial_capacity * sizeof(typename SegArray<T>::SegPtr));
  dir->sa.reset(new_sa);

  auto dir_entry = dir->sa->entries();
  for (size_t i = 0; i < dir->capacity; ++i) {
    dir_entry[i] =
        Allocator::New<Segment<T>>(kCacheLineSize, 0, dir->sa->global_depth);
    dir_entry[i]->pattern = i;
  }
  /* clear the log area */
  for (size_t i = 0; i < log_capacity; ++i) {
    log[i].lock = 0;
    log[i].pending = nullptr;
  }

  seg_num = 0;
  restart = 0;
}

template <class T>
CCEH<T>::CCEH() {
  std::cout << "Reinitialize for CCEH" << std::endl;
}

template <class T>
CCEH<T>::~CCEH() = default;

template <class T>
void CCEH<T>::TxSwap(void** entry, Segment<T>** new_seg) {
  *entry = *new_seg;
  *new_seg = nullptr;
}

template <class T>
void CCEH<T>::DirectoryDouble(size_t x, Segment<T>* old_seg,
                              Segment<T>** new_seg) {
  auto* sa = dir->sa.get();
  Segment<T>** d = sa->entries();
  auto global_depth = sa->global_depth;

  size_t new_capacity = 2 * dir->capacity;
  auto new_seg_array = Allocator::New<SegArray<T>>(
      kCacheLineSize, sizeof(typename SegArray<T>::SegPtr) * new_capacity);
  new_seg_array->global_depth = static_cast<size_t>(log2(new_capacity));
  memset(new_seg_array->entries(), 0,
         new_capacity * sizeof(typename SegArray<T>::SegPtr));
  dir->new_sa.reset(new_seg_array);
  auto doubled_entries = dir->new_sa->entries();

  for (size_t i = 0; i < dir->capacity; ++i) {
    doubled_entries[2 * i] = d[i];
    doubled_entries[2 * i + 1] = d[i];
  }

  TxSwap(reinterpret_cast<void**>(&doubled_entries[2 * x + 1]), new_seg);

  dir->sa = std::move(dir->new_sa);
  dir->capacity *= 2;
}

template <class T>
void CCEH<T>::DirectoryUpdate(size_t x, Segment<T>* old_seg,
                              Segment<T>** new_seg) {
  Segment<T>** dir_entry = dir->sa->entries();
  auto global_depth = dir->sa->global_depth;
  auto depth_diff = global_depth - old_seg->local_depth;
  if (depth_diff == 1) {
    if (x % 2 == 0) {
      TxSwap(reinterpret_cast<void**>(&dir_entry[x + 1]), new_seg);
    } else {
      TxSwap(reinterpret_cast<void**>(&dir_entry[x]), new_seg);
    }
  } else {
    size_t chunk_size = pow(2, global_depth - (old_seg->local_depth));
    x = x - (x % chunk_size);
    size_t base = chunk_size / 2;
    TxSwap(reinterpret_cast<void**>(&dir_entry[x + base + base - 1]), new_seg);
    auto seg_ptr = dir_entry[x + base + base - 1];
    for (int i = base - 2; i >= 0; --i) {
      dir_entry[x + base + i] = seg_ptr;
    }
  }
}

template <class T>
int CCEH<T>::Insert(T key, Value_t value, bool is_in_epoch) {
  if (!is_in_epoch) {
    if constexpr (use_epoch) {
      auto epoch_guard = Allocator::AcquireEpochGuard();
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
    auto y = (key_hash & segment_mask) * pairs_per_cacheline;

    for (;;) {
      auto snapshot = dir->sa.get();
      if (snapshot != dir->sa.get()) {
        continue;
      }
      auto x = (key_hash >> (64 - snapshot->global_depth));
      auto dir_entry = snapshot->entries();
      Segment<T>* target = dir_entry[x];

      auto result = target->Insert(key, value, y, key_hash);

      if (result == InsertResult::Duplicate) return -1;

      if (result == InsertResult::NeedSplit) {
        auto s = target->Split(key_hash, log.data());
        if (s == nullptr) {
          continue;
        }

        auto split_seg = *s;
        split_seg->pattern =
            ((key_hash >> (64 - split_seg->local_depth + 1)) << 1) + 1;

        // Directory management
        {
          DirectoryGuard dir_guard(dir.get());
          auto sa = dir->sa.get();
          dir_entry = sa->entries();

          x = (key_hash >> (64 - sa->global_depth));
          target = dir_entry[x];
          if (target->local_depth < sa->global_depth) {
            DirectoryUpdate(x, target, s);
          } else {  // directory doubling
            DirectoryDouble(x, target, s);
          }
          target->pattern = (key_hash >> (64 - target->local_depth)) << 1;
          target->local_depth += 1;
          if constexpr (inplace) {
            target->sema = 0;
            target->ReleaseLock();
          }
        }  // DirectoryGuard released here
        size_t log_pos = key_hash % log_capacity;
        log[log_pos].UnlockLog();
        continue;
      } else if (result == InsertResult::Redirect) {
        break;  // restart outer loop with fresh key_hash
      }

      return 0;
    }
  }
}

template <class T>
bool CCEH<T>::Delete(T key, bool is_in_epoch) {
  if (!is_in_epoch) {
    if constexpr (use_epoch) {
      auto epoch_guard = Allocator::AcquireEpochGuard();
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
  auto y = (key_hash & segment_mask) * pairs_per_cacheline;

  for (;;) {
    auto snapshot = dir->sa.get();
    if (snapshot != dir->sa.get()) {
      continue;
    }
    auto x = (key_hash >> (64 - snapshot->global_depth));
    auto dir_entry = snapshot->entries();
    Segment<T>* segment = dir_entry[x];

    auto sema = segment->sema;
    if (sema == -1) {
      continue;
    }
    std::unique_lock<std::shared_mutex> lock(segment->mutex);

    if ((key_hash >> (64 - segment->local_depth)) != segment->pattern ||
        segment->sema == -1) {
      continue;
    }

    for (size_t i = 0; i < pairs_per_cacheline * num_cache_lines; ++i) {
      auto slot = (y + i) % Segment<T>::num_slots;
      if constexpr (std::is_pointer_v<T>) {
        if ((segment->slots_[slot].key != (T)INVALID) &&
            (VarEqual(key->key, segment->slots_[slot].key->key, key->length,
                      segment->slots_[slot].key->length))) {
          segment->slots_[slot].key = (T)INVALID;
          return true;
        }
      } else {
        if (segment->slots_[slot].key == key) {
          segment->slots_[slot].key = (T)INVALID;
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
    if constexpr (use_epoch) {
      auto epoch_guard = Allocator::AcquireEpochGuard();
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
  auto y = (key_hash & segment_mask) * pairs_per_cacheline;

  for (;;) {
    auto snapshot = dir->sa.get();
    if (snapshot != dir->sa.get()) {
      continue;
    }
    auto x = (key_hash >> (64 - snapshot->global_depth));
    auto dir_entry = snapshot->entries();
    Segment<T>* segment = dir_entry[x];

    std::shared_lock<std::shared_mutex> lock(segment->mutex, std::try_to_lock);
    if (!lock) {
      continue;
    }

    if ((key_hash >> (64 - segment->local_depth)) != segment->pattern ||
        segment->sema == -1) {
      continue;
    }

    for (size_t i = 0; i < pairs_per_cacheline * num_cache_lines; ++i) {
      auto slot = (y + i) % Segment<T>::num_slots;
      if constexpr (std::is_pointer_v<T>) {
        if ((segment->slots_[slot].key != (T)INVALID) &&
            (VarEqual(key->key, segment->slots_[slot].key->key, key->length,
                      segment->slots_[slot].key->length))) {
          auto value = segment->slots_[slot].value;
          *value_ = value;
          return true;
        }
      } else {
        if (segment->slots_[slot].key == key) {
          auto value = segment->slots_[slot].value;
          *value_ = value;
          return true;
        }
      }
    }

    return false;
  }
}
}  // namespace cceh
