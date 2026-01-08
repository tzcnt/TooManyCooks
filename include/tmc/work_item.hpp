// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// Selects the type of tmc::work_item based on the provided compile time
// parameter TMC_WORK_ITEM=
// CORO, FUNC, or FUNCORO

// CORO will be the default if undefined

// Macro hackery to enable defines TMC_WORK_ITEM=CORO / TMC_WORK_ITEM=FUNC, etc
#ifndef TMC_WORK_ITEM
#define TMC_WORK_ITEM CORO
#endif

#define TMC_WORK_ITEM_CORO 0
#define TMC_WORK_ITEM_FUNC 1
#define TMC_WORK_ITEM_FUNCORO 2
#define TMC_CONCAT_impl(a, b) a##b
#define TMC_CONCAT(a, b) TMC_CONCAT_impl(a, b)
#define TMC_WORK_ITEM_IS_impl(WORK_ITEM_TYPE)                                  \
  TMC_CONCAT(TMC_WORK_ITEM_, TMC_WORK_ITEM) ==                                 \
    TMC_CONCAT(TMC_WORK_ITEM_, WORK_ITEM_TYPE)
#define TMC_WORK_ITEM_IS(WORK_ITEM_TYPE) TMC_WORK_ITEM_IS_impl(WORK_ITEM_TYPE)

#if TMC_WORK_ITEM_IS(CORO)
#include <coroutine>
namespace tmc {
using work_item = std::coroutine_handle<>;
}
#define TMC_WORK_ITEM_AS_STD_CORO(x) (x)

#elif TMC_WORK_ITEM_IS(FUNC)
#ifndef TMC_TRIVIAL_TASK
// This is because std::function requires its template type to be copyable.
static_assert(
  false, "If TMC_WORK_ITEM=FUNC, then TMC_TRIVIAL_TASK must also be defined."
);
#endif
#include <functional>
namespace tmc {
using work_item = std::function<void()>;
}
#define TMC_WORK_ITEM_AS_STD_CORO(x)                                           \
  (*x.template target<std::coroutine_handle<>>())

#elif TMC_WORK_ITEM_IS(FUNCORO)
#include "tmc/detail/coro_functor.hpp"
namespace tmc {
using work_item = tmc::coro_functor;
}
#define TMC_WORK_ITEM_AS_STD_CORO(x) (x.as_coroutine())

#endif
