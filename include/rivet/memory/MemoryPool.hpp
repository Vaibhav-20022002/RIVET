#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <vector>

namespace rivet::memory {

/// A strictly single-threaded, fixed-capacity object pool.
/// Used to allocate IoOperations without touching the system heap on the hot path.

template<typename T> class MemoryPool {
  std::vector<std::unique_ptr<T>> storage;
  std::vector<T *>                free_list;

public:
  explicit MemoryPool(size_t capacity) {
    storage.reserve(capacity);
    free_list.reserve(capacity);

    for (size_t i = 0; i < capacity; ++i) {
      storage.push_back(std::make_unique<T>());
      free_list.push_back(storage.back().get());
    }
  }

  /// Non-copyable, non-movable
  MemoryPool(const MemoryPool &)            = delete;
  MemoryPool &operator=(const MemoryPool &) = delete;

  /// Retrives an object from the free list. Returns nullptr if empty
  [[nodiscard]] T *allocate() noexcept {
    if (free_list.empty()) {
      return nullptr;
    }

    T *obj = free_list.back();
    free_list.pop_back();
    return obj;
  }

  /// Returns an object to the free list
  void deallocate(T *obj) noexcept {
    assert(obj != nullptr);
    free_list.push_back(obj);
  }

  [[nodiscard]] size_t available() const noexcept { return free_list.size(); }

  [[nodiscard]] size_t capacity() const noexcept { return storage.capacity(); }
};

} //< namespace rivet::memory