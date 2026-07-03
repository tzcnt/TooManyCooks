// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// The Chase-Lev deque has a racy read where it will memcpy data that may be
// invalid, and then check the validity by an atomic load afterward. If invalid,
// the potentially-garbage data is discarded. TSan doesn't like this.
// When the data size is 1 pointer (std::coroutine_handle) we can solve this
// by accessing it using a relaxed read from an atomic_ref, which TSan accepts.
// When the data size is 2 pointers (tmc::coro_functor), we would have to use a
// lot of workaround hacks. It's simpler to just disable TSan for the specific
// accesses in that case.
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define TMC_HAS_TSAN
#endif // TSAN
#endif // TSAN

// no_sanitize suppression only works if the function is not inlined.
#ifdef TMC_HAS_TSAN
#define TMC_INLINE_OR_TSAN __attribute__((no_sanitize("thread")))
#else
#define TMC_INLINE_OR_TSAN TMC_FORCE_INLINE
#endif

// Some compilers (observed on Clang 18/19 and Apple Clang) may convert
// `Cond ? mux.get<0>() : mux.get<1>()` into a select of both branches.
// The reads happen after the atomic acquire of Cond, so it's not a true race,
// but it triggers a TSan false positive because the discarded value races
// with a write from the completing task. Forcing this to be noinline under
// TSan prevents the speculative load from being issued, while still allowing
// the function to be instrumented by TSan for genuine races.
#ifdef TMC_HAS_TSAN
#define TMC_TSAN_NO_SPECULATE __attribute__((noinline))
#else
#define TMC_TSAN_NO_SPECULATE
#endif
