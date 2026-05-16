// Copyright (c) Simon Fraser University & The Chinese University of Hong Kong.
// All rights reserved. Licensed under the MIT license.
//
// Benchmark driver for evaluating the throughput and scalability of the Dash
// hash table (CCEH variant) under various workloads. Supports insert, positive
// search, negative search, delete, and mixed operations with both fixed-length
// (uint64_t) and variable-length (string_key*) key types. Threads optionally
// enrol in application-level epochs for safe memory reclamation.

#include <argparse/argparse.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../util/key_generator.hpp"
#include "../util/uniform.hpp"
#include "CCEH/CCEH_cleanup.h"
#include "Hash.h"
#include "allocator_new.h"

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

// Work partition assigned to a single thread.
struct ThreadPartition {
  int index = 0;
  uint64_t begin = 0;
  uint64_t end = 0;
  int key_length = 0;                // for variable-length keys
  void* workload = nullptr;          // points into the shared workload buffer
  uint64_t random_seed = 0;          // for the per-thread RNG (mixed workloads)
  std::chrono::steady_clock::time_point finish_time;
};

// Per-thread operation counter, cache-line padded to avoid false sharing.
struct alignas(64) OperationSample {
  std::atomic<uint64_t> count{0};
};

// ---------------------------------------------------------------------------
// Barrier – coordinates worker threads and the main thread
// ---------------------------------------------------------------------------
class Barrier {
 public:
  explicit Barrier(int n_threads)
      : ready_count_(n_threads),
        done_count_(n_threads) {}

  // Workers call this after finishing local setup; spins until SignalStart().
  void WorkerReady() {
    ready_count_.fetch_sub(1, std::memory_order_seq_cst);
    while (start_flag_.load(std::memory_order_seq_cst) == 1) {
      // spin
    }
  }

  // Main thread calls this after all workers have signalled readiness.
  void SignalStart() {
    while (ready_count_.load(std::memory_order_seq_cst) != 0) {
      // spin
    }
    start_ts_ = std::chrono::steady_clock::now();
    start_flag_.store(0, std::memory_order_seq_cst);
  }

  // A worker calls this when it finishes its work.
  void WorkerDone() {
    if (done_count_.fetch_sub(1, std::memory_order_seq_cst) == 1) {
      std::lock_guard<std::mutex> lk(mtx_);
      finished_ = true;
      cv_.notify_one();
    }
  }

  // Main thread calls this to sleep until all workers finish.
  void WaitAllDone() {
    std::unique_lock<std::mutex> lk(mtx_);
    cv_.wait(lk, [this] { return finished_; });
  }

  std::chrono::steady_clock::time_point start_time() const { return start_ts_; }

 private:
  std::atomic<int> start_flag_{1};
  std::atomic<int> ready_count_;
  std::atomic<int> done_count_;
  std::mutex mtx_;
  std::condition_variable cv_;
  bool finished_{false};
  std::chrono::steady_clock::time_point start_ts_;
};

// ---------------------------------------------------------------------------
// CPU affinity
// ---------------------------------------------------------------------------
inline void SetAffinity(uint32_t idx) {
  cpu_set_t cset;
  CPU_ZERO(&cset);
  if (idx < 24) {
    CPU_SET(idx, &cset);
  } else {
    CPU_SET(idx + 24, &cset);
  }
  sched_setaffinity(0, sizeof(cpu_set_t), &cset);
}

// ---------------------------------------------------------------------------
// Index initialisation
// ---------------------------------------------------------------------------
template <class T>
Hash<T>* InitializeIndex(int seg_count, const std::string& index_type) {
  std::cout << "Initialize " << index_type << std::endl;
  Hash<T>* eh = new cceh::CCEH<T>(seg_count);
  std::cout << "Initialization complete" << std::endl;
  return eh;
}

// ---------------------------------------------------------------------------
// Workload-generation helpers
// ---------------------------------------------------------------------------

// Generate `n` fixed-length (8-byte) keys into a pre-allocated buffer.
inline void GenerateFixedKeys(void* buf, uint64_t n, key_generator_t* gen) {
  auto* arr = static_cast<uint64_t*>(buf);
  for (uint64_t i = 0; i < n; ++i) {
    arr[i] = gen->next_uint64();
  }
}

// Generate `n` variable-length keys into a pre-allocated buffer.
inline void GenerateVarKeys(void* buf, uint64_t n, int length,
                            key_generator_t* gen) {
  auto* raw = static_cast<char*>(buf);
  const int word_cnt = (length / 8) + ((length % 8) != 0 ? 1 : 0);
  const int slot = static_cast<int>(sizeof(string_key)) + length;

  for (uint64_t i = 0; i < n; ++i) {
    auto* vk = reinterpret_cast<string_key*>(raw + i * slot);
    vk->length = length;
    const uint64_t rval = gen->next_uint64();
    for (int w = 0; w < word_cnt; ++w) {
      reinterpret_cast<uint64_t*>(vk->key)[w] = rval;
    }
  }
}

// Pre-load keys into the index.
template <class T>
void PreLoad(uint64_t count, Hash<T>* index, int var_len, void* workload) {
  if (count == 0) return;
  std::cout << "Start pre-loading workload" << std::endl;

  if constexpr (!std::is_pointer_v<T>) {
    auto* keys = static_cast<T*>(workload);
    for (uint64_t i = 0; i < count; ++i) {
      index->Insert(keys[i], DEFAULT);
    }
  } else {
    auto* raw = static_cast<char*>(workload);
    const int slot_size = static_cast<int>(sizeof(string_key)) + var_len;
    for (uint64_t i = 0; i < count; ++i) {
      auto* key = reinterpret_cast<T>(raw + i * slot_size);
      index->Insert(key, DEFAULT);
    }
  }

  std::cout << "Finished loading " << count << " keys" << std::endl;
}

// ---------------------------------------------------------------------------
// Benchmark operations
//
// Each operation family is a template parameterised on UseEpoch.
// `if constexpr (UseEpoch)` is resolved at compile time – zero runtime overhead.
// ---------------------------------------------------------------------------

template <class T, bool UseEpoch>
void OpsInsert(ThreadPartition& part, Hash<T>* index, uint64_t epoch_dur) {
  SetAffinity(part.index);
  const uint64_t begin = part.begin;
  const uint64_t end = part.end;
  auto* raw = static_cast<char*>(part.workload);

  if constexpr (!std::is_pointer_v<T>) {
    auto* keys = static_cast<T*>(part.workload);

    if constexpr (UseEpoch) {
      const uint64_t rounds = (end - begin) / epoch_dur;
      uint64_t i = 0;
      while (i < rounds) {
        auto guard = Allocator::AcquireEpochGuard();
        const uint64_t chunk_end = begin + (i + 1) * epoch_dur;
        for (uint64_t j = begin + i * epoch_dur; j < chunk_end; ++j) {
          index->Insert(keys[j], DEFAULT, true);
        }
        ++i;
      }
      {
        auto guard = Allocator::AcquireEpochGuard();
        for (i = begin + epoch_dur * rounds; i < end; ++i) {
          index->Insert(keys[i], DEFAULT, true);
        }
      }
    } else {
      for (uint64_t i = begin; i < end; ++i) {
        index->Insert(keys[i], DEFAULT);
      }
    }
  } else {
    const uint64_t slot_size =
        static_cast<uint64_t>(sizeof(string_key)) + part.key_length;

    if constexpr (UseEpoch) {
      const uint64_t rounds = (end - begin) / epoch_dur;
      uint64_t i = 0;
      while (i < rounds) {
        auto guard = Allocator::AcquireEpochGuard();
        const uint64_t chunk_end = begin + (i + 1) * epoch_dur;
        for (uint64_t j = begin + i * epoch_dur; j < chunk_end; ++j) {
          auto* key = reinterpret_cast<T>(raw + slot_size * j);
          index->Insert(key, DEFAULT, true);
        }
        ++i;
      }
      {
        auto guard = Allocator::AcquireEpochGuard();
        for (i = begin + epoch_dur * rounds; i < end; ++i) {
          auto* key = reinterpret_cast<T>(raw + slot_size * i);
          index->Insert(key, DEFAULT, true);
        }
      }
    } else {
      for (uint64_t i = begin; i < end; ++i) {
        auto* key = reinterpret_cast<T>(raw + slot_size * i);
        index->Insert(key, DEFAULT);
      }
    }
  }
}

template <class T, bool UseEpoch>
void OpsSearch(ThreadPartition& part, Hash<T>* index, uint64_t epoch_dur) {
  SetAffinity(part.index);
  const uint64_t begin = part.begin;
  const uint64_t end = part.end;
  auto* raw = static_cast<char*>(part.workload);
  Value_t value{};
  uint64_t missed = 0;

  if constexpr (!std::is_pointer_v<T>) {
    auto* keys = static_cast<T*>(part.workload);

    if constexpr (UseEpoch) {
      const uint64_t rounds = (end - begin) / epoch_dur;
      uint64_t i = 0;
      while (i < rounds) {
        auto guard = Allocator::AcquireEpochGuard();
        const uint64_t chunk_end = begin + (i + 1) * epoch_dur;
        for (uint64_t j = begin + i * epoch_dur; j < chunk_end; ++j) {
          if (!index->Get(keys[j], &value, true)) ++missed;
        }
        ++i;
      }
      {
        auto guard = Allocator::AcquireEpochGuard();
        for (i = begin + epoch_dur * rounds; i < end; ++i) {
          if (!index->Get(keys[i], &value, true)) ++missed;
        }
      }
    } else {
      for (uint64_t i = begin; i < end; ++i) {
        if (!index->Get(keys[i], &value)) ++missed;
      }
    }
  } else {
    const uint64_t slot_size =
        static_cast<uint64_t>(sizeof(string_key)) + part.key_length;

    if constexpr (UseEpoch) {
      const uint64_t rounds = (end - begin) / epoch_dur;
      uint64_t i = 0;
      while (i < rounds) {
        auto guard = Allocator::AcquireEpochGuard();
        const uint64_t chunk_end = begin + (i + 1) * epoch_dur;
        for (uint64_t j = begin + i * epoch_dur; j < chunk_end; ++j) {
          auto* key = reinterpret_cast<T>(raw + slot_size * j);
          if (!index->Get(key, &value, true)) ++missed;
        }
        ++i;
      }
      {
        auto guard = Allocator::AcquireEpochGuard();
        for (i = begin + epoch_dur * rounds; i < end; ++i) {
          auto* key = reinterpret_cast<T>(raw + slot_size * i);
          if (!index->Get(key, &value, true)) ++missed;
        }
      }
    } else {
      for (uint64_t i = begin; i < end; ++i) {
        auto* key = reinterpret_cast<T>(raw + slot_size * i);
        if (!index->Get(key, &value)) ++missed;
      }
    }
  }

  std::cout << "not_found = " << missed << std::endl;
}

template <class T, bool UseEpoch>
void OpsDelete(ThreadPartition& part, Hash<T>* index, uint64_t epoch_dur) {
  SetAffinity(part.index);
  const uint64_t begin = part.begin;
  const uint64_t end = part.end;
  auto* raw = static_cast<char*>(part.workload);
  uint64_t missed = 0;

  if constexpr (!std::is_pointer_v<T>) {
    auto* keys = static_cast<T*>(part.workload);

    if constexpr (UseEpoch) {
      const uint64_t rounds = (end - begin) / epoch_dur;
      uint64_t i = 0;
      while (i < rounds) {
        auto guard = Allocator::AcquireEpochGuard();
        const uint64_t chunk_end = begin + (i + 1) * epoch_dur;
        for (uint64_t j = begin + i * epoch_dur; j < chunk_end; ++j) {
          if (!index->Delete(keys[j], true)) ++missed;
        }
        ++i;
      }
      {
        auto guard = Allocator::AcquireEpochGuard();
        for (i = begin + epoch_dur * rounds; i < end; ++i) {
          if (!index->Delete(keys[i], true)) ++missed;
        }
      }
    } else {
      for (uint64_t i = begin; i < end; ++i) {
        if (!index->Delete(keys[i])) ++missed;
      }
    }
  } else {
    const uint64_t slot_size =
        static_cast<uint64_t>(sizeof(string_key)) + part.key_length;

    if constexpr (UseEpoch) {
      const uint64_t rounds = (end - begin) / epoch_dur;
      uint64_t i = 0;
      while (i < rounds) {
        auto guard = Allocator::AcquireEpochGuard();
        const uint64_t chunk_end = begin + (i + 1) * epoch_dur;
        for (uint64_t j = begin + i * epoch_dur; j < chunk_end; ++j) {
          auto* key = reinterpret_cast<T>(raw + slot_size * j);
          if (!index->Delete(key, true)) ++missed;
        }
        ++i;
      }
      {
        auto guard = Allocator::AcquireEpochGuard();
        for (i = begin + epoch_dur * rounds; i < end; ++i) {
          auto* key = reinterpret_cast<T>(raw + slot_size * i);
          if (!index->Delete(key, true)) ++missed;
        }
      }
    } else {
      for (uint64_t i = begin; i < end; ++i) {
        auto* key = reinterpret_cast<T>(raw + slot_size * i);
        if (!index->Delete(key)) ++missed;
      }
    }
  }

  std::cout << "not_found = " << missed << std::endl;
}

template <class T, bool UseEpoch>
void OpsMixed(ThreadPartition& part, Hash<T>* index, uint64_t epoch_dur,
              double ins_ratio, double read_ratio, double del_ratio) {
  SetAffinity(part.index);
  const uint64_t begin = part.begin;
  const uint64_t end = part.end;
  auto* raw = static_cast<char*>(part.workload);
  auto* keys = static_cast<T*>(part.workload);
  const uint64_t slot_size =
      static_cast<uint64_t>(sizeof(string_key)) + part.key_length;

  UniformRandom rng(part.random_seed);
  uint64_t missed = 0;
  Value_t value{};

  const uint32_t ins_threshold = static_cast<uint32_t>(ins_ratio * 100);
  const uint32_t read_threshold = static_cast<uint32_t>(read_ratio * 100) + ins_threshold;
  const uint32_t del_threshold = static_cast<uint32_t>(del_ratio * 100) + read_threshold;
  (void)del_threshold;

  auto dispatch = [&](uint64_t j) {
    T key;
    if constexpr (std::is_pointer_v<T>) {
      key = reinterpret_cast<T>(raw + slot_size * j);
    } else {
      key = keys[j];
    }

    const uint32_t r = rng.next_uint32() % 100;
    if (r < ins_threshold) {
      if constexpr (UseEpoch) {
        index->Insert(key, DEFAULT, true);
      } else {
        index->Insert(key, DEFAULT);
      }
    } else if (r < read_threshold) {
      if constexpr (UseEpoch) {
        if (!index->Get(key, &value, true)) ++missed;
      } else {
        if (!index->Get(key, &value)) ++missed;
      }
    } else {
      if constexpr (UseEpoch) {
        index->Delete(key, true);
      } else {
        index->Delete(key);
      }
    }
  };

  if constexpr (UseEpoch) {
    const uint64_t rounds = (end - begin) / epoch_dur;
    uint64_t i = 0;
    while (i < rounds) {
      auto guard = Allocator::AcquireEpochGuard();
      const uint64_t chunk_end = begin + (i + 1) * epoch_dur;
      for (uint64_t j = begin + i * epoch_dur; j < chunk_end; ++j) {
        dispatch(j);
      }
      ++i;
    }
    {
      auto guard = Allocator::AcquireEpochGuard();
      for (i = begin + epoch_dur * rounds; i < end; ++i) {
        dispatch(i);
      }
    }
  } else {
    for (uint64_t i = begin; i < end; ++i) {
      dispatch(i);
    }
  }

  std::cout << "not_found = " << missed << std::endl;
}

// Sampling variants – record per-thread operation counts.
// These mirror the original sampling functions which are defined but not
// currently exercised by Run(). They are preserved for completeness.

template <class T, bool UseEpoch>
void OpsSearchSampled(ThreadPartition& part, Hash<T>* index, uint64_t epoch_dur,
                      std::vector<OperationSample>& samples) {
  SetAffinity(part.index);
  auto& cnt = samples[part.index];
  cnt.count.store(0, std::memory_order_relaxed);

  const uint64_t begin = part.begin;
  const uint64_t end = part.end;
  auto* raw = static_cast<char*>(part.workload);
  Value_t value{};

  if constexpr (!std::is_pointer_v<T>) {
    auto* keys = static_cast<T*>(part.workload);
    if constexpr (UseEpoch) {
      const uint64_t rounds = (end - begin) / epoch_dur;
      uint64_t i = 0;
      while (i < rounds) {
        auto guard = Allocator::AcquireEpochGuard();
        const uint64_t chunk_end = begin + (i + 1) * epoch_dur;
        for (uint64_t j = begin + i * epoch_dur; j < chunk_end; ++j) {
          index->Get(keys[j], &value, true);
          cnt.count.fetch_add(1, std::memory_order_relaxed);
        }
        ++i;
      }
      {
        auto guard = Allocator::AcquireEpochGuard();
        for (i = begin + epoch_dur * rounds; i < end; ++i) {
          index->Get(keys[i], &value, true);
          cnt.count.fetch_add(1, std::memory_order_relaxed);
        }
      }
    } else {
      for (uint64_t i = begin; i < end; ++i) {
        index->Get(keys[i], &value);
        cnt.count.fetch_add(1, std::memory_order_relaxed);
      }
    }
  } else {
    const uint64_t slot_size =
        static_cast<uint64_t>(sizeof(string_key)) + part.key_length;
    if constexpr (UseEpoch) {
      const uint64_t rounds = (end - begin) / epoch_dur;
      uint64_t i = 0;
      while (i < rounds) {
        auto guard = Allocator::AcquireEpochGuard();
        const uint64_t chunk_end = begin + (i + 1) * epoch_dur;
        for (uint64_t j = begin + i * epoch_dur; j < chunk_end; ++j) {
          auto* key = reinterpret_cast<T>(raw + slot_size * j);
          index->Get(key, &value, true);
          cnt.count.fetch_add(1, std::memory_order_relaxed);
        }
        ++i;
      }
      {
        auto guard = Allocator::AcquireEpochGuard();
        for (i = begin + epoch_dur * rounds; i < end; ++i) {
          auto* key = reinterpret_cast<T>(raw + slot_size * i);
          index->Get(key, &value, true);
          cnt.count.fetch_add(1, std::memory_order_relaxed);
        }
      }
    } else {
      for (uint64_t i = begin; i < end; ++i) {
        auto* key = reinterpret_cast<T>(raw + slot_size * i);
        index->Get(key, &value);
        cnt.count.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
}

template <class T, bool UseEpoch>
void OpsInsertSampled(ThreadPartition& part, Hash<T>* index, uint64_t epoch_dur,
                      std::vector<OperationSample>& samples) {
  SetAffinity(part.index);
  auto& cnt = samples[part.index];
  cnt.count.store(0, std::memory_order_relaxed);

  const uint64_t begin = part.begin;
  const uint64_t end = part.end;
  auto* raw = static_cast<char*>(part.workload);

  if constexpr (!std::is_pointer_v<T>) {
    auto* keys = static_cast<T*>(part.workload);
    if constexpr (UseEpoch) {
      const uint64_t rounds = (end - begin) / epoch_dur;
      uint64_t i = 0;
      while (i < rounds) {
        auto guard = Allocator::AcquireEpochGuard();
        const uint64_t chunk_end = begin + (i + 1) * epoch_dur;
        for (uint64_t j = begin + i * epoch_dur; j < chunk_end; ++j) {
          index->Insert(keys[j], DEFAULT, true);
          cnt.count.fetch_add(1, std::memory_order_relaxed);
        }
        ++i;
      }
      {
        auto guard = Allocator::AcquireEpochGuard();
        for (i = begin + epoch_dur * rounds; i < end; ++i) {
          index->Insert(keys[i], DEFAULT, true);
          cnt.count.fetch_add(1, std::memory_order_relaxed);
        }
      }
    } else {
      for (uint64_t i = begin; i < end; ++i) {
        index->Insert(keys[i], DEFAULT);
        cnt.count.fetch_add(1, std::memory_order_relaxed);
      }
    }
  } else {
    const uint64_t slot_size =
        static_cast<uint64_t>(sizeof(string_key)) + part.key_length;
    if constexpr (UseEpoch) {
      const uint64_t rounds = (end - begin) / epoch_dur;
      uint64_t i = 0;
      while (i < rounds) {
        auto guard = Allocator::AcquireEpochGuard();
        const uint64_t chunk_end = begin + (i + 1) * epoch_dur;
        for (uint64_t j = begin + i * epoch_dur; j < chunk_end; ++j) {
          auto* key = reinterpret_cast<T>(raw + slot_size * j);
          index->Insert(key, DEFAULT, true);
          cnt.count.fetch_add(1, std::memory_order_relaxed);
        }
        ++i;
      }
      {
        auto guard = Allocator::AcquireEpochGuard();
        for (i = begin + epoch_dur * rounds; i < end; ++i) {
          auto* key = reinterpret_cast<T>(raw + slot_size * i);
          index->Insert(key, DEFAULT, true);
          cnt.count.fetch_add(1, std::memory_order_relaxed);
        }
      }
    } else {
      for (uint64_t i = begin; i < end; ++i) {
        auto* key = reinterpret_cast<T>(raw + slot_size * i);
        index->Insert(key, DEFAULT);
        cnt.count.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Benchmark driver (templated on the operation callable)
// ---------------------------------------------------------------------------

template <class T, typename Fn>
void GeneralBench(std::vector<ThreadPartition>& partitions, Hash<T>* index,
                  int n_threads, uint64_t total_ops, const std::string& label,
                  Fn&& op_fn) {
  std::string full_label = label + std::to_string(n_threads);
  std::cout << full_label << " Begin" << std::endl;

  Barrier barrier(n_threads);

  std::vector<std::thread> threads;
  threads.reserve(n_threads);
  for (int i = 0; i < n_threads; ++i) {
    threads.emplace_back([i, &partitions, index, &barrier, &op_fn]() {
      barrier.WorkerReady();
      op_fn(partitions[i], index);
      partitions[i].finish_time = std::chrono::steady_clock::now();
      barrier.WorkerDone();
    });
  }

  barrier.SignalStart();
  barrier.WaitAllDone();

  for (auto& t : threads) {
    t.join();
  }

  // Throughput statistics (mirrors the original output).
  const auto t0 = barrier.start_time();

  double fastest = std::chrono::duration<double>(
      partitions[0].finish_time - t0).count();
  double slowest = fastest;
  double duration_sum = fastest;

  for (int i = 1; i < n_threads; ++i) {
    double d = std::chrono::duration<double>(
        partitions[i].finish_time - t0).count();
    duration_sum += d;
    if (fastest > d) fastest = d;
    if (slowest < d) slowest = d;
  }

  const double avg = duration_sum / n_threads;
  printf("%d threads, Time = %f s, throughput = %f ops/s, fastest = %f, slowest = %f\n",
         n_threads,
         avg,
         total_ops / avg,
         total_ops / fastest,
         total_ops / slowest);

  std::cout << full_label << " End" << std::endl;
}

// ---------------------------------------------------------------------------
// Workload construction
// ---------------------------------------------------------------------------

std::vector<char> GenerateUniformWorkload(uint64_t total_count, int var_len,
                                          key_generator_t* gen,
                                          bool is_variable) {
  if (!is_variable) {
    std::vector<char> buf(total_count * sizeof(uint64_t));
    GenerateFixedKeys(buf.data(), total_count, gen);
    return buf;
  }

  const int slot = static_cast<int>(sizeof(string_key)) + var_len;
  std::vector<char> buf(static_cast<size_t>(total_count) * slot);
  GenerateVarKeys(buf.data(), total_count, var_len, gen);
  return buf;
}

std::vector<char> GenerateSkewWorkload(uint64_t load_count, uint64_t exist_count,
                                       uint64_t nonexist_count, int var_len,
                                       bool is_variable, uint64_t load_type,
                                       double skew_factor,
                                       key_generator_t* uniform_gen) {
  const size_t total = is_variable
      ? (load_count + exist_count + nonexist_count)
            * (static_cast<size_t>(var_len) + sizeof(string_key))
      : (load_count + exist_count + nonexist_count) * sizeof(uint64_t);
  std::vector<char> buf(total);

  if (!is_variable) {
    auto* arr = reinterpret_cast<uint64_t*>(buf.data());

    if (load_type == 1) {
      auto range_gen = std::make_unique<range_key_generator_t>(1);
      GenerateFixedKeys(arr, load_count, range_gen.get());
    } else {
      GenerateFixedKeys(arr, load_count, uniform_gen);
    }

    if (exist_count) {
      auto skew_gen = std::make_unique<zipfian_key_generator_t>(
          1, exist_count, skew_factor);
      GenerateFixedKeys(arr + load_count, exist_count, skew_gen.get());
    }

    if (nonexist_count) {
      auto skew_gen = std::make_unique<zipfian_key_generator_t>(
          exist_count + load_count,
          exist_count + nonexist_count + load_count,
          skew_factor);
      GenerateFixedKeys(arr + load_count + exist_count,
                        nonexist_count, skew_gen.get());
    }
  } else {
    const int slot = static_cast<int>(sizeof(string_key)) + var_len;

    if (load_type == 1) {
      auto range_gen = std::make_unique<range_key_generator_t>(1);
      GenerateVarKeys(buf.data(), load_count, var_len, range_gen.get());
    } else {
      GenerateVarKeys(buf.data(), load_count, var_len, uniform_gen);
    }

    if (exist_count) {
      auto skew_gen = std::make_unique<zipfian_key_generator_t>(
          1, exist_count, skew_factor);
      GenerateVarKeys(buf.data() + load_count * slot,
                      exist_count, var_len, skew_gen.get());
    }

    if (nonexist_count) {
      auto skew_gen = std::make_unique<zipfian_key_generator_t>(
          exist_count + load_count,
          exist_count + nonexist_count + load_count,
          skew_factor);
      GenerateVarKeys(buf.data() + (load_count + exist_count) * slot,
                      nonexist_count, var_len, skew_gen.get());
    }
  }

  return buf;
}

// ---------------------------------------------------------------------------
// Core benchmark runner
// ---------------------------------------------------------------------------

template <class T>
static void Run(const std::string& index_type, const std::string& key_type,
                const std::string& operation, const std::string& distribution,
                int init_cap, int n_threads, uint64_t load_count,
                uint64_t op_count, int var_len, bool use_epoch,
                uint64_t epoch_dur, double read_ratio, double ins_ratio,
                double del_ratio, double skew_factor, uint64_t load_type,
                size_t pool_size) {
  (void)pool_size;

  auto uniform_gen = std::make_unique<uniform_key_generator_t>();
  Hash<T>* index = InitializeIndex<T>(init_cap, index_type);

  const bool is_variable = (key_type == "variable");
  const uint64_t total_gen = op_count * 2 + load_count;

  std::cout << "Generate workload" << std::endl;
  std::vector<char> workload_buf;
  if (distribution == "uniform") {
    workload_buf = GenerateUniformWorkload(total_gen, var_len,
                                           uniform_gen.get(), is_variable);
  } else {
    workload_buf = GenerateSkewWorkload(load_count, op_count, op_count, var_len,
                                        is_variable, load_type, skew_factor,
                                        uniform_gen.get());
  }

  // Build a persistent copy for insert workloads (variable-length keys
  // require a stable buffer).
  std::vector<char> insert_buf;
  const void* insert_workload_ptr = workload_buf.data();
  if (is_variable) {
    const size_t slot = static_cast<size_t>(sizeof(string_key)) + var_len;
    insert_buf.resize(total_gen * slot);
    std::memcpy(insert_buf.data(), workload_buf.data(), total_gen * slot);
    insert_workload_ptr = insert_buf.data();
  }

  std::cout << "Finish generating workload" << std::endl;
  std::cout << "load num = " << load_count << std::endl;

  PreLoad<T>(load_count, index, var_len,
             const_cast<void*>(insert_workload_ptr));

  // Build the post-load offset pointers (skip the pre-loaded region).
  void* op_workload;       // points past the pre-loaded region in workload_buf
  void* op_insert_workload; // points past the pre-loaded region in insert_buf/data
  if (is_variable) {
    const size_t slot = static_cast<size_t>(sizeof(string_key)) + var_len;
    op_workload = static_cast<char*>(static_cast<void*>(workload_buf.data())) + load_count * slot;
    op_insert_workload = static_cast<char*>(const_cast<void*>(insert_workload_ptr)) + load_count * slot;
  } else {
    op_workload = static_cast<uint64_t*>(static_cast<void*>(workload_buf.data())) + load_count;
    op_insert_workload = op_workload;
  }

  // Create thread partitions.
  const uint64_t chunk_size = op_count / n_threads;
  std::vector<ThreadPartition> partitions(n_threads);

  // Per-thread random seeds (match original: rand() used only during setup).
  std::random_device rd;
  std::mt19937 seed_gen(rd());
  for (int i = 0; i < n_threads; ++i) {
    partitions[i].index = i;
    partitions[i].random_seed = seed_gen();
    partitions[i].begin = i * chunk_size;
    partitions[i].end = (i + 1) * chunk_size;
    partitions[i].key_length = var_len;
    partitions[i].workload = op_workload;
  }
  partitions.back().end = op_count;

  // ---- Benchmark dispatch ----

  if (operation == "insert") {
    std::cout << "Insert-only Benchmark" << std::endl;
    for (auto& p : partitions) p.workload = op_insert_workload;
    if (use_epoch) {
      GeneralBench<T>(partitions, index, n_threads, op_count, "Insert",
        [epoch_dur](ThreadPartition& p, Hash<T>* idx) {
          OpsInsert<T, true>(p, idx, epoch_dur);
        });
    } else {
      GeneralBench<T>(partitions, index, n_threads, op_count, "Insert",
        [epoch_dur](ThreadPartition& p, Hash<T>* idx) {
          OpsInsert<T, false>(p, idx, epoch_dur);
        });
    }

  } else if (operation == "pos") {
    if (load_count == 0) {
      std::cout << "Please first specify the #pre-load keys!" << std::endl;
      return;
    }
    for (auto& p : partitions) p.workload = static_cast<void*>(workload_buf.data());
    if (use_epoch) {
      GeneralBench<T>(partitions, index, n_threads, op_count, "Pos_search",
        [epoch_dur](ThreadPartition& p, Hash<T>* idx) {
          OpsSearch<T, true>(p, idx, epoch_dur);
        });
    } else {
      GeneralBench<T>(partitions, index, n_threads, op_count, "Pos_search",
        [epoch_dur](ThreadPartition& p, Hash<T>* idx) {
          OpsSearch<T, false>(p, idx, epoch_dur);
        });
    }

  } else if (operation == "neg") {
    if (load_count == 0) {
      std::cout << "Please first specify the #pre-load keys!" << std::endl;
      return;
    }
    if (use_epoch) {
      GeneralBench<T>(partitions, index, n_threads, op_count, "Neg_search",
        [epoch_dur](ThreadPartition& p, Hash<T>* idx) {
          OpsSearch<T, true>(p, idx, epoch_dur);
        });
    } else {
      GeneralBench<T>(partitions, index, n_threads, op_count, "Neg_search",
        [epoch_dur](ThreadPartition& p, Hash<T>* idx) {
          OpsSearch<T, false>(p, idx, epoch_dur);
        });
    }

  } else if (operation == "delete") {
    if (load_count == 0) {
      std::cout << "Please first specify the #pre-load keys!" << std::endl;
      return;
    }
    for (auto& p : partitions) p.workload = static_cast<void*>(workload_buf.data());
    if (use_epoch) {
      GeneralBench<T>(partitions, index, n_threads, op_count, "Delete",
        [epoch_dur](ThreadPartition& p, Hash<T>* idx) {
          OpsDelete<T, true>(p, idx, epoch_dur);
        });
    } else {
      GeneralBench<T>(partitions, index, n_threads, op_count, "Delete",
        [epoch_dur](ThreadPartition& p, Hash<T>* idx) {
          OpsDelete<T, false>(p, idx, epoch_dur);
        });
    }

  } else if (operation == "mixed") {
    for (auto& p : partitions) p.workload = op_insert_workload;
    if (use_epoch) {
      GeneralBench<T>(partitions, index, n_threads, op_count, "Mixed",
        [epoch_dur, ins_ratio, read_ratio, del_ratio](
            ThreadPartition& p, Hash<T>* idx) {
          OpsMixed<T, true>(p, idx, epoch_dur, ins_ratio, read_ratio, del_ratio);
        });
    } else {
      GeneralBench<T>(partitions, index, n_threads, op_count, "Mixed",
        [epoch_dur, ins_ratio, read_ratio, del_ratio](
            ThreadPartition& p, Hash<T>* idx) {
          OpsMixed<T, false>(p, idx, epoch_dur, ins_ratio, read_ratio, del_ratio);
        });
    }

  } else if (operation == "skew-all") {
    std::cout << "Comprehensive skew Benchmark" << std::endl;
    for (auto& p : partitions) p.workload = op_insert_workload;
    if (use_epoch) {
      GeneralBench<T>(partitions, index, n_threads, op_count, "Insert",
        [epoch_dur](ThreadPartition& p, Hash<T>* idx) {
          OpsInsert<T, true>(p, idx, epoch_dur);
        });
    } else {
      GeneralBench<T>(partitions, index, n_threads, op_count, "Insert",
        [epoch_dur](ThreadPartition& p, Hash<T>* idx) {
          OpsInsert<T, false>(p, idx, epoch_dur);
        });
    }
    index->GetNumber();

    // Positive search.
    if (use_epoch) {
      GeneralBench<T>(partitions, index, n_threads, op_count, "Pos_search",
        [epoch_dur](ThreadPartition& p, Hash<T>* idx) {
          OpsSearch<T, true>(p, idx, epoch_dur);
        });
    } else {
      GeneralBench<T>(partitions, index, n_threads, op_count, "Pos_search",
        [epoch_dur](ThreadPartition& p, Hash<T>* idx) {
          OpsSearch<T, false>(p, idx, epoch_dur);
        });
    }

    // Negative search – shift to the second half of the workload.
    for (int i = 0; i < n_threads; ++i) {
      partitions[i].begin = op_count + i * chunk_size;
      partitions[i].end   = op_count + (i + 1) * chunk_size;
    }
    partitions.back().end = 2 * op_count;
    if (use_epoch) {
      GeneralBench<T>(partitions, index, n_threads, op_count, "Neg_search",
        [epoch_dur](ThreadPartition& p, Hash<T>* idx) {
          OpsSearch<T, true>(p, idx, epoch_dur);
        });
    } else {
      GeneralBench<T>(partitions, index, n_threads, op_count, "Neg_search",
        [epoch_dur](ThreadPartition& p, Hash<T>* idx) {
          OpsSearch<T, false>(p, idx, epoch_dur);
        });
    }

    // Restore ranges for delete.
    for (int i = 0; i < n_threads; ++i) {
      partitions[i].begin = i * chunk_size;
      partitions[i].end   = (i + 1) * chunk_size;
    }
    partitions.back().end = op_count;

    for (auto& p : partitions) p.workload = static_cast<void*>(workload_buf.data());
    if (use_epoch) {
      GeneralBench<T>(partitions, index, n_threads, op_count, "Delete",
        [epoch_dur](ThreadPartition& p, Hash<T>* idx) {
          OpsDelete<T, true>(p, idx, epoch_dur);
        });
    } else {
      GeneralBench<T>(partitions, index, n_threads, op_count, "Delete",
        [epoch_dur](ThreadPartition& p, Hash<T>* idx) {
          OpsDelete<T, false>(p, idx, epoch_dur);
        });
    }
    index->GetNumber();

  } else { // "full" – all single operations
    std::cout << "Comprehensive Benchmark" << std::endl;

    // Insert
    std::cout << "insertion start" << std::endl;
    for (auto& p : partitions) p.workload = op_insert_workload;
    if (use_epoch) {
      GeneralBench<T>(partitions, index, n_threads, op_count, "Insert",
        [epoch_dur](ThreadPartition& p, Hash<T>* idx) {
          OpsInsert<T, true>(p, idx, epoch_dur);
        });
    } else {
      GeneralBench<T>(partitions, index, n_threads, op_count, "Insert",
        [epoch_dur](ThreadPartition& p, Hash<T>* idx) {
          OpsInsert<T, false>(p, idx, epoch_dur);
        });
    }

    index->GetNumber();

    // Positive search.
    for (auto& p : partitions) p.workload = static_cast<void*>(workload_buf.data());
    if (use_epoch) {
      GeneralBench<T>(partitions, index, n_threads, op_count, "Pos_search",
        [epoch_dur](ThreadPartition& p, Hash<T>* idx) {
          OpsSearch<T, true>(p, idx, epoch_dur);
        });
    } else {
      GeneralBench<T>(partitions, index, n_threads, op_count, "Pos_search",
        [epoch_dur](ThreadPartition& p, Hash<T>* idx) {
          OpsSearch<T, false>(p, idx, epoch_dur);
        });
    }

    // Negative search.
    for (int i = 0; i < n_threads; ++i) {
      partitions[i].begin = op_count + i * chunk_size;
      partitions[i].end   = op_count + (i + 1) * chunk_size;
    }
    partitions.back().end = 2 * op_count;
    if (use_epoch) {
      GeneralBench<T>(partitions, index, n_threads, op_count, "Neg_search",
        [epoch_dur](ThreadPartition& p, Hash<T>* idx) {
          OpsSearch<T, true>(p, idx, epoch_dur);
        });
    } else {
      GeneralBench<T>(partitions, index, n_threads, op_count, "Neg_search",
        [epoch_dur](ThreadPartition& p, Hash<T>* idx) {
          OpsSearch<T, false>(p, idx, epoch_dur);
        });
    }

    // Restore ranges for delete.
    for (int i = 0; i < n_threads; ++i) {
      partitions[i].begin = i * chunk_size;
      partitions[i].end   = (i + 1) * chunk_size;
    }
    partitions.back().end = op_count;

    for (auto& p : partitions) p.workload = static_cast<void*>(workload_buf.data());
    if (use_epoch) {
      GeneralBench<T>(partitions, index, n_threads, op_count, "Delete",
        [epoch_dur](ThreadPartition& p, Hash<T>* idx) {
          OpsDelete<T, true>(p, idx, epoch_dur);
        });
    } else {
      GeneralBench<T>(partitions, index, n_threads, op_count, "Delete",
        [epoch_dur](ThreadPartition& p, Hash<T>* idx) {
          OpsDelete<T, false>(p, idx, epoch_dur);
        });
    }
    index->GetNumber();
  }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool CheckRatio(double r, double s, double d) {
  int rp = static_cast<int>(r * 100);
  int sp = static_cast<int>(s * 100);
  int dp = static_cast<int>(d * 100);
  return (rp + sp + dp) == 100;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
  SetAffinity(0);

  // All configurable parameters with their defaults (matching the original).
  std::string index_type   = "dash-ex";
  std::string key_type     = "fixed";
  std::string distribution = "uniform";
  uint64_t    init_cap     = 64;
  int         n_threads    = 1;
  uint64_t    load_count   = 0;
  uint64_t    load_type    = 0;
  uint64_t    op_count     = 20000000;
  std::string operation    = "full";
  double      read_r       = 1.0;
  double      ins_r        = 0.0;
  double      del_r        = 0.0;
  double      skew_factor  = 0.8;
  bool        use_epoch    = false;
  int         var_len      = 16;
  uint64_t    pool_size_gb = 30;
  uint64_t    epoch_dur    = 1000;

  argparse::ArgumentParser parser("test_pmem_refactored", "1.0",
                                  argparse::default_arguments::help);

  parser.add_argument("--index")
      .default_value(index_type)
      .help("which index to evaluate: dash-ex / dash-lh / cceh / level")
      .store_into(index_type);
  parser.add_argument("-k", "--key-type")
      .default_value(key_type)
      .help("type of stored keys: fixed / variable")
      .store_into(key_type);
  parser.add_argument("--distribution")
      .default_value(distribution)
      .help("distribution of the workload: uniform / skew")
      .store_into(distribution);
  parser.add_argument("-i", "--initial-segments")
      .default_value(init_cap)
      .help("initial number of segments in extendible hashing")
      .store_into(init_cap);
  parser.add_argument("-t", "--threads")
      .default_value(n_threads)
      .help("number of concurrent threads")
      .store_into(n_threads);
  parser.add_argument("-n", "--load-count")
      .default_value(load_count)
      .help("number of pre-insertion (load) keys")
      .store_into(load_count);
  parser.add_argument("--load-type")
      .default_value(load_type)
      .help("type of pre-load keys: random (0) or range (1)")
      .store_into(load_type);
  parser.add_argument("-p", "--operations")
      .default_value(op_count)
      .help("number of operations (insert / search / delete) to execute")
      .store_into(op_count);
  parser.add_argument("--op")
      .default_value(operation)
      .help("operation type: insert / pos / neg / delete / mixed / full / skew-all")
      .store_into(operation);
  parser.add_argument("-r", "--read-ratio")
      .default_value(read_r)
      .help("read ratio for mixed workload: 0.0 ~ 1.0")
      .store_into(read_r);
  parser.add_argument("-s", "--insert-ratio")
      .default_value(ins_r)
      .help("insert ratio for mixed workload: 0.0 ~ 1.0")
      .store_into(ins_r);
  parser.add_argument("-d", "--delete-ratio")
      .default_value(del_r)
      .help("delete ratio for mixed workload: 0.0 ~ 1.0")
      .store_into(del_r);
  parser.add_argument("--skew")
      .default_value(skew_factor)
      .help("skew factor of the workload")
      .store_into(skew_factor);
  parser.add_argument("-e", "--epoch")
      .help("register epoch at application level")
      .flag()
      .store_into(use_epoch);
  parser.add_argument("--var-length")
      .default_value(var_len)
      .help("length of the variable-length key")
      .store_into(var_len);
  parser.add_argument("--pool-size")
      .default_value(pool_size_gb)
      .help("size of the memory pool (GB)")
      .store_into(pool_size_gb);
  parser.add_argument("--epoch-duration")
      .default_value(epoch_dur)
      .help("frequency at which to enrol into the epoch (batch size)")
      .store_into(epoch_dur);

  try {
    parser.parse_args(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n"
              << parser.help().str() << std::endl;
    return 1;
  }

  const size_t pool_size = pool_size_gb * 1024ul * 1024ul * 1024ul;

  std::cout << "Distribution = " << distribution << std::endl;

  if (use_epoch) {
    std::cout << "Epoch registration at application level" << std::endl;
  }

  if (distribution == "skew") {
    std::cout << "Skew theta = " << skew_factor << std::endl;
  }

  if (operation == "mixed") {
    std::cout << "Search ratio = " << read_r << std::endl;
    std::cout << "Insert ratio = " << ins_r << std::endl;
    std::cout << "Delete ratio = " << del_r << std::endl;
  }

  if (!CheckRatio(read_r, ins_r, del_r)) {
    std::cout << "The ratios do not sum to 100%!" << std::endl;
    return 0;
  }

  if (key_type == "fixed") {
    Run<uint64_t>(index_type, key_type, operation, distribution,
                  static_cast<int>(init_cap), n_threads, load_count,
                  op_count, var_len, use_epoch, epoch_dur,
                  read_r, ins_r, del_r, skew_factor,
                  load_type, pool_size);
  } else {
    std::cout << "Variable-length key length = " << var_len << std::endl;
    Run<string_key*>(index_type, key_type, operation, distribution,
                     static_cast<int>(init_cap), n_threads, load_count,
                     op_count, var_len, use_epoch, epoch_dur,
                     read_r, ins_r, del_r, skew_factor,
                     load_type, pool_size);
  }

  return 0;
}
