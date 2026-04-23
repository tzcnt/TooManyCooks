#pragma once

#include <coroutine>

#include "tmc/task.hpp"

// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

namespace tmc {
// NOTE: this approach doesn't work if the awaitable is something like
// aw_spawn_fork, as it needs to receive the real handle and access to this
// (non-destroyed) coro at a later time
//
// What if we pass in Outer's parent to the inner await_suspend, and then
// destroy Outer in await_resume()? We'd also need to update the flags of inner
// awaitable, in case this was part of a group. Might be better to use
// awaitable_traits interface rather than calling await_suspend. But if that
// leads to the creation of a wrapper task, then it's not worth it.
template <typename Awaitable> struct aw_become {
  Awaitable wrapped;
  bool await_ready() { return wrapped.await_ready(); }

  template <typename P>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<P> Outer) {
    auto result = wrapped.await_suspend(std::noop_coroutine());
    Outer.promise().customizer.post_continuation(Outer);
    return result;
  }
  auto await_resume() { return wrapped.await_resume(); }

  // TODO make this work with operator co_await types
};

} // namespace tmc

// TODO make this a TMC awaitable, otherwise the "destroyed outer" is just
// tmc::task_wrapper

// TODO better name for this? "return_after_void" / "return_after_value"
// One should take a return value and set it
