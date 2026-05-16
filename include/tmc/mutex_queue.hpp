// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/concepts_awaitable.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/detail/waiter_list.hpp"
#include "tmc/mutex.hpp"
#include "tmc/task.hpp"

#include <coroutine>
#include <deque>
#include <optional>
#include <utility>

namespace tmc {

/// A simple async queue backed by std::deque and protected by tmc::mutex.
template <typename T> class mutex_queue {
  class [[nodiscard(
    "You must co_await aw_mutex_queue_pop_waiter for it to have any effect."
  )]] aw_mutex_queue_pop_waiter : tmc::detail::AwaitTagNoGroupAsIs {
    mutex_queue& parent;
    tmc::detail::waiter_list_waiter waiter;

  public:
    inline aw_mutex_queue_pop_waiter(mutex_queue& Parent) noexcept
        : parent(Parent) {}

    inline bool await_ready() noexcept { return false; }

    inline void await_suspend(std::coroutine_handle<> Outer) noexcept {
      waiter.continuation = Outer;
      waiter.continuation_executor = tmc::detail::this_thread::executor();
      waiter.continuation_priority = tmc::detail::this_thread::this_task().prio;

      parent.waiters.push_back(this);
      parent.mut.unlock();
    }

    inline void await_resume() noexcept {}

    inline void resume() noexcept { waiter.resume(); }

    aw_mutex_queue_pop_waiter(aw_mutex_queue_pop_waiter const&) = delete;
    aw_mutex_queue_pop_waiter&
    operator=(aw_mutex_queue_pop_waiter const&) = delete;
    aw_mutex_queue_pop_waiter(aw_mutex_queue_pop_waiter&&) = delete;
    aw_mutex_queue_pop_waiter&
    operator=(aw_mutex_queue_pop_waiter&&) = delete;
  };

  tmc::mutex mut;
  std::deque<T> items;
  std::deque<aw_mutex_queue_pop_waiter*> waiters;

  inline void wake_one() noexcept {
    if (!waiters.empty()) {
      aw_mutex_queue_pop_waiter* waiter = waiters.front();
      waiters.pop_front();
      waiter->resume();
    }
  }

public:
  mutex_queue() = default;
  mutex_queue(mutex_queue const&) = delete;
  mutex_queue& operator=(mutex_queue const&) = delete;
  mutex_queue(mutex_queue&&) = delete;
  mutex_queue& operator=(mutex_queue&&) = delete;

  tmc::task<void> push_front(T value) {
    auto scope = co_await mut.lock_scope();
    items.push_front(std::move(value));
    wake_one();
  }

  tmc::task<void> push_back(T value) {
    auto scope = co_await mut.lock_scope();
    items.push_back(std::move(value));
    wake_one();
  }

  tmc::task<std::optional<T>> try_pop_front() {
    auto scope = co_await mut.lock_scope();
    if (items.empty()) {
      co_return std::nullopt;
    }
    T value = std::move(items.front());
    items.pop_front();
    co_return std::move(value);
  }

  tmc::task<std::optional<T>> try_pop_back() {
    auto scope = co_await mut.lock_scope();
    if (items.empty()) {
      co_return std::nullopt;
    }
    T value = std::move(items.back());
    items.pop_back();
    co_return std::move(value);
  }

  tmc::task<T> pop_front() {
    while (true) {
      co_await mut;
      if (!items.empty()) {
        T value = std::move(items.front());
        items.pop_front();
        mut.unlock();
        co_return std::move(value);
      }
      co_await aw_mutex_queue_pop_waiter(*this);
    }
  }

  tmc::task<T> pop_back() {
    while (true) {
      co_await mut;
      if (!items.empty()) {
        T value = std::move(items.back());
        items.pop_back();
        mut.unlock();
        co_return std::move(value);
      }
      co_await aw_mutex_queue_pop_waiter(*this);
    }
  }
};

} // namespace tmc
