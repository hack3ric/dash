
// Copyright (c) Simon Fraser University & The Chinese University of Hong Kong. All rights reserved.
// Licensed under the MIT license.
#pragma once
#include <garbage_list.h>
#include <sys/mman.h>
#include <cstring>
#include <memory>

#include "../util/utils.h"
#include "x86intrin.h"

using DestroyCallback = void (*)(void* callback_context, void* object);

class Allocator {
 public:
  static void Initialize() {
    instance_ = std::unique_ptr<Allocator>(new Allocator());
    instance_->epoch_manager_.Initialize();
    instance_->garbage_list_.Initialize(&instance_->epoch_manager_, 1024 * 8);
  }

  static void Close_pool() { instance_.reset(); }

  static Allocator* Get() { return instance_.get(); }

  static void* AllocateRaw(size_t alignment, size_t size) {
    void* ptr;
    posix_memalign(&ptr, alignment, size);
    return ptr;
  }

  static void* ZAllocateRaw(size_t alignment, size_t size) {
    void* ptr;
    posix_memalign(&ptr, alignment, size);
    memset(ptr, 0, size);
    return ptr;
  }

  template <typename T, typename... Args>
  static T* New(size_t alignment, size_t extra_bytes, Args&&... args) {
    void* raw = ZAllocateRaw(alignment, sizeof(T) + extra_bytes);
    return std::construct_at(static_cast<T*>(raw),
                             std::forward<Args>(args)...);
  }

  template <typename T>
  static T* New(size_t alignment, size_t extra_bytes) {
    return static_cast<T*>(ZAllocateRaw(alignment, sizeof(T) + extra_bytes));
  }

  template <typename T>
  struct Deleter {
    void operator()(T* ptr) const { Delete(ptr); }
  };

  template <typename T>
  using uptr = std::unique_ptr<T, Deleter<T>>;

  template <typename T, typename... Args>
  static auto MakeUnique(size_t alignment, size_t extra_bytes,
                         Args&&... args) {
    return uptr<T>(New<T>(alignment, extra_bytes, std::forward<Args>(args)...));
  }

  static void Allocate(void** ptr, uint32_t alignment, size_t size) {
    *ptr = AllocateRaw(alignment, size);
  }

  static void ZAllocate(void** ptr, uint32_t alignment, size_t size) {
    *ptr = ZAllocateRaw(alignment, size);
  }

  static void DefaultCallback(void* callback_context, void* ptr) { free(ptr); }

  static void Free(void* ptr, DestroyCallback callback = DefaultCallback,
                   void* context = nullptr) {
    instance_->garbage_list_.Push(ptr, callback, context);
  }

  static void Free(GarbageList::Item* item, void* ptr,
                   DestroyCallback callback = DefaultCallback,
                   void* context = nullptr) {
    item->SetValue(ptr, instance_->epoch_manager_.GetCurrentEpoch(), callback,
                   context);
  }

  template <typename T>
  static void Delete(T* ptr) {
    Free(static_cast<void*>(ptr));
  }

  static EpochGuard AcquireEpochGuard() {
    return EpochGuard{&instance_->epoch_manager_};
  }

  static void Protect() { instance_->epoch_manager_.Protect(); }

  static void Unprotect() { instance_->epoch_manager_.Unprotect(); }

  static GarbageList::Item* ReserveItem() {
    return instance_->garbage_list_.ReserveItem();
  }

  static void ResetItem(GarbageList::Item* mem) {
    instance_->garbage_list_.ResetItem(mem);
  }

 private:
  Allocator() = default;
  EpochManager epoch_manager_{};
  GarbageList garbage_list_{};
  static inline std::unique_ptr<Allocator> instance_{};
};
