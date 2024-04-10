#pragma once
#include <mutex>
#include <vector>

namespace tmc {
namespace detail {
// Actually a LIFO stack, not a FIFO queue
template <typename WorkItem> class MutexQueue {
  std::vector<WorkItem> vec;
  std::mutex m;

public:
  MutexQueue() : vec{}, m{} {}
  MutexQueue(size_t InitialCapacity) : vec{}, m{} {
    vec.reserve(InitialCapacity);
  }
  template <typename T> void enqueue(T&& Item) {
    std::lock_guard<std::mutex> lg(m);
    vec.emplace_back(static_cast<T&&>(Item));
  }

  template <typename It> void enqueue_bulk(It ItemFirst, size_t Count) {
    std::lock_guard<std::mutex> lg(m);
    auto item = ItemFirst;
    for (size_t i = 0; i < Count; ++i) {
      vec.emplace_back(*item);
      ++item;
    }
  }
  bool try_dequeue(WorkItem& Item) {
    std::lock_guard<std::mutex> lg(m);
    if (vec.empty()) {
      return false;
    }
    Item = std::move(vec.back());
    vec.pop_back();
    return true;
  }

  // provided for compatibility with qu_lockfree

  template <typename It>
  void enqueue_bulk_ex_cpu(
    It ItemFirst, size_t Count, [[maybe_unused]] size_t Priority
  ) {
    enqueue_bulk(ItemFirst, Count);
  }

  template <typename T>
  void enqueue_ex_cpu(T&& item, [[maybe_unused]] size_t Priority) {
    enqueue(static_cast<T&&>(item));
  }

  bool try_dequeue_ex_cpu(WorkItem& Item, [[maybe_unused]] size_t Priority) {
    return try_dequeue(Item);
  }

  bool empty() {
    std::lock_guard<std::mutex> lg(m);
    return vec.empty();
  }
};
} // namespace detail
} // namespace tmc
