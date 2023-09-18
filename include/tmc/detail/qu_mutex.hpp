#pragma once
#include <memory>
#include <vector>

namespace tmc {
namespace detail {
// Actually a LIFO stack, not a FIFO queue
template <typename Item> struct MutexQueue {
private:
  std::vector<Item> vec;
  std::mutex m;

public:
  MutexQueue() : vec{}, m{} {}
  MutexQueue(size_t initial_capacity) : vec{}, m{} {
    vec.reserve(initial_capacity);
  }
  template <typename T> void enqueue(T &&item) {
    std::lock_guard lg(m);
    vec.emplace_back(std::forward<T>(item));
  }

  template <typename It> void enqueue_bulk(It itemFirst, size_t count) {
    std::lock_guard lg(m);
    auto item = itemFirst;
    for (size_t i = 0; i < count; ++i) {
      vec.emplace_back(*item);
      ++item;
    }
  }
  bool try_dequeue(Item &item) {
    std::lock_guard lg(m);
    if (vec.empty()) {
      return false;
    }
    item = std::move(vec.back());
    vec.pop_back();
    return true;
  }

  // provided for compatibility with qu_lockfree

  template <typename It>
  void enqueue_bulk_ex_cpu(It itemFirst, size_t count, size_t prio) {
    enqueue_bulk(itemFirst, count);
  }

  template <typename T> void enqueue_ex_cpu(T &&item, size_t prio) {
    enqueue(std::forward<T>(item));
  }

  bool try_dequeue_ex_cpu(Item &item, size_t prio) { return try_dequeue(item); }

  bool empty() {
    std::lock_guard lg(m);
    return vec.empty();
  }
};
} // namespace detail
} // namespace tmc
