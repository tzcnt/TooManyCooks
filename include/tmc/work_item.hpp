// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// Selects the type of tmc::work_item based on the provided compile time
// parameter TMC_WORK_ITEM=CORO or TMC_WORK_ITEM=FUNCORO

// CORO will be the default if undefined
#ifndef TMC_WORK_ITEM
#define TMC_WORK_ITEM CORO
#endif

#define TMC_WORK_ITEM_CORO 0
#define TMC_WORK_ITEM_FUNCORO 1
#define TMC_WORK_ITEM_FUNC 2
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

#elif TMC_WORK_ITEM_IS(FUNCORO)
#include "tmc/detail/coro_functor.hpp"
namespace tmc {
using work_item = tmc::detail::coro_functor;
}
#define TMC_WORK_ITEM_AS_STD_CORO(x) (x.as_coroutine())

#elif TMC_WORK_ITEM_IS(FUNC)
static_assert(
  false, "TMC_WORK_ITEM=FUNC was removed in v1.6. Use TMC_WORK_ITEM=FUNCORO "
         "instead."
);
#endif
