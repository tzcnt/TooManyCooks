#pragma once
#include "tmc/detail/thread_locals.hpp"
#include <coroutine>
#include <mutex>
namespace tmc {
static inline bool yield_requested() {
  // yield if the yield_priority value is smaller (higher priority)
  // than our currently running task
  return detail::this_thread::this_task.yield_priority->load(
             std::memory_order_relaxed) < detail::this_thread::this_task.prio;
}

struct aw_yield {
  inline constexpr bool await_ready() const noexcept { return false; }

  inline void await_suspend(std::coroutine_handle<> outer) const noexcept {
    detail::this_thread::executor->post_variant(
        std::move(outer), detail::this_thread::this_task.prio);
  }

  inline void await_resume() const noexcept {}
};

// convenience function so you can `co_await yield();`
// instead of
// `co_await aw_yield{};`
[[nodiscard("You must co_await the return of yield().")]] constexpr aw_yield
yield() {
  return {};
}

struct aw_yield_if_requested {
  inline bool await_ready() const noexcept { return !yield_requested(); }

  inline void await_suspend(std::coroutine_handle<> outer) const noexcept {
    detail::this_thread::executor->post_variant(
        std::move(outer), detail::this_thread::this_task.prio);
  }

  inline void await_resume() const noexcept {}
};

// convenience function so you can `co_await yield_if_requested();`
// instead of
// `if (yield_requested()) { co_await yield(); }`
[[nodiscard("You must co_await the return of "
            "yield_if_requested().")]] constexpr aw_yield_if_requested
yield_if_requested() {
  return {};
}

// Every `n` calls to `co_await`, this will check yield_requested() and yield if
// that is true.
struct aw_yield_counter_dynamic {
  int64_t count;
  int64_t n;
  aw_yield_counter_dynamic(int64_t n_in) : count(0), n(n_in) {}
  inline bool await_ready() noexcept {
    ++count;
    if (count < n) [[likely]] {
      return true;
    } else [[unlikely]] {
      count = 0;
      return !yield_requested();
    }
  }

  inline void await_suspend(std::coroutine_handle<> outer) const noexcept {
    detail::this_thread::executor->post_variant(
        std::move(outer), detail::this_thread::this_task.prio);
  }

  inline void await_resume() const noexcept {}

  inline void reset() { count = 0; }
};

// Every `n` calls to `co_await`, this will check yield_requested() and yield if
// that is true.
inline aw_yield_counter_dynamic check_yield_counter_dynamic(size_t n) {
  return aw_yield_counter_dynamic(n);
}

// Every `N` calls to `co_await`, this will check yield_requested() and yield if
// that is true. This provides more opportunity for the compiler to unroll than
// the dynamic version.
template <int64_t N> struct aw_yield_counter {
  int64_t count;
  aw_yield_counter() : count(0) {}
  inline bool await_ready() noexcept {
    ++count;
    if (count < N) [[likely]] {
      return true;
    } else [[unlikely]] {
      count = 0;
      return !yield_requested();
    }
  }

  inline void await_suspend(std::coroutine_handle<> outer) const noexcept {
    detail::this_thread::executor->post_variant(
        std::move(outer), detail::this_thread::this_task.prio);
  }

  inline void await_resume() const noexcept {}

  inline void reset() { count = 0; }
};

// Every `N` calls to `co_await`, this will check yield_requested() and yield if
// that is true. This provides more opportunity for the compiler to unroll than
// the dynamic version.
template <int64_t N> inline aw_yield_counter<N> check_yield_counter() {
  return aw_yield_counter<N>();
}
} // namespace tmc
