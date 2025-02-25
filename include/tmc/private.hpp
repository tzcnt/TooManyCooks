// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts.hpp" // IWYU pragma: keep
#include "tmc/detail/thread_locals.hpp"

#include <coroutine>

// If this thread is an executor thread, adds this work item to its private work
// queue. Otherwise, adds it to the shared work queue, but when an executor
// thread takes a task from the shared queue, it will move it to its private
// queue instead.

// Needs to mark the task itself as private (not just the queue). Child tasks
// should inherit this property.

// This dovetails with a simple allocator strategy - for tasks less than half
// the size of a 4k block, bump allocate them from the block's arena. A simple
// atomic counter can track the number of currently live tasks. When this atomic
// counter reaches 0, the block can be deleted. This is much more efficient for
// private tasks, as the atomic counter won't be shared across threads.

// This is a property only of tasks, not of other kinds of awaitables. Need to
// also create a member function on task that allows setting this for a child
// task.

// TODO - how to handle an awaitable that is running on another thread? When
// resuming, this task needs to be sent back to its owning thread's private
// queue. A pointer to the queue could be used; however, this requires an MPSC
// queue which is not as efficient as a true "private" queue.

namespace tmc {
/// The awaitable type returned by `tmc::go_private()`.
class [[nodiscard("You must co_await aw_go_private for it to have any "
                  "effect.")]] aw_go_private
    : tmc::detail::AwaitTagNoGroupAsIs {};

/// Returns an awaitable that moves this task onto the requested executor. If
/// this task is already running on the requested executor, the co_await will do
/// nothing.
inline aw_go_private go_private() { return aw_go_private(); }

inline aw_go_public go_public() { return aw_go_public(); }
} // namespace tmc
