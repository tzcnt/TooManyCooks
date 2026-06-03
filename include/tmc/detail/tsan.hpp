#pragma once

// The Chase-Lev deque has a racy read where it will memcpy data that may be
// invalid, and then check the validity by an atomic load afterward. If invalid,
// the potentially-garbage data is discarded. TSan doesn't like this.
// When the data size is pointer-sized (std::coroutine_handle) we can solve this
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
