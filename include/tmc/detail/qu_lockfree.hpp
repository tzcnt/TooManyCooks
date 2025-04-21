// last upstream patch: 2025-01-26
// https://github.com/cameron314/concurrentqueue/commit/0d54c6794510018f42b1b987f55c313dd1358320

// Copyright (c) 2013-2020, Cameron Desrochers
// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

// This queue has been modified from the original at:
// https://github.com/cameron314/concurrentqueue/blob/master/concurrentqueue.h
// for integration within TooManyCooks. It now contains a preallocated list of
// explicit producers which can be read from as thread-local members by ex_cpu.
// Each of these producers (which represents a work-stealing thread) may dequeue
// from its own queue as a stack (LIFO) but only FIFO from other producers.

// The original software is offered under either the BSD or Boost license.
// The Boost license has been chosen here for compatibility.

#pragma once

#include "tmc/detail/compat.hpp"
#include "tmc/detail/qu_inbox.hpp"
#include "tmc/detail/thread_locals.hpp"

#if defined(__GNUC__) && !defined(__INTEL_COMPILER)
// Disable -Wconversion warnings (spuriously triggered when Traits::size_t and
// Traits::index_t are set to < 32 bits, causing integer promotion, causing
// warnings upon assigning any computed values)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"

#ifdef MCDBGQ_USE_RELACY
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
#endif
#endif

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4146 4293 4307 4309 4706)
#if (!defined(_HAS_CXX17) || !_HAS_CXX17)
// VS2019 with /W4 warns about constant conditional expressions but unless
// /std=c++17 or higher does not support `if constexpr`, so we have no choice
// but to simply disable the warning
#pragma warning(disable : 4127) // conditional expression is constant
#endif
#endif

#if defined(__APPLE__)
#include "TargetConditionals.h"
#endif

#ifdef MCDBGQ_USE_RELACY
#include "relacy/relacy_std.hpp"
#include "relacy_shims.h"
// We only use malloc/free anyway, and the delete macro messes up `= delete`
// method declarations. We'll override the default trait malloc ourselves
// without a macro.
#undef new
#undef delete
#undef malloc
#undef free
#else
#include <atomic>
#include <cassert>
#endif

#if defined(MCDBGQ_USE_RELACY) || (defined(__APPLE__) && TARGET_OS_IPHONE) ||  \
  defined(__MVS__) || defined(MOODYCAMEL_NO_THREAD_LOCAL)
#include <thread>
#endif

#include "tmc/detail/tiny_lock.hpp" // used for thread exit synchronization
#include <algorithm>
#include <array>
#include <climits> // for CHAR_BIT
#include <cstddef> // for max_align_t
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <type_traits>

// Platform-specific definitions of a numeric thread ID type and an invalid
// value
namespace tmc::queue {
namespace details {
template <typename thread_id_t> struct thread_id_converter {
  typedef thread_id_t thread_id_numeric_size_t;
  typedef thread_id_t thread_id_hash_t;
  static thread_id_hash_t prehash(thread_id_t const& x) { return x; }
};
} // namespace details
} // namespace tmc::queue
#if defined(MCDBGQ_USE_RELACY)
namespace tmc::queue {
namespace details {
typedef std::uint32_t thread_id_t;
static const thread_id_t invalid_thread_id = 0xFFFFFFFFU;
static const thread_id_t invalid_thread_id2 = 0xFFFFFFFEU;
static inline thread_id_t thread_id() { return rl::thread_index(); }
} // namespace details
} // namespace tmc::queue
#elif defined(_WIN32) || defined(__WINDOWS__) || defined(__WIN32__)
// No sense pulling in windows.h in a header, we'll manually declare the
// function we use and rely on backwards-compatibility for this not to break
extern "C"
  __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
namespace tmc::queue {
namespace details {
static_assert(
  sizeof(unsigned long) == sizeof(std::uint32_t),
  "Expected size of unsigned long to be 32 bits on Windows"
);
typedef std::uint32_t thread_id_t;
static const thread_id_t invalid_thread_id =
  0; // See http://blogs.msdn.com/b/oldnewthing/archive/2004/02/23/78395.aspx
static const thread_id_t invalid_thread_id2 =
  0xFFFFFFFFU; // Not technically guaranteed to be invalid, but is never used
               // in practice. Note that all Win32 thread IDs are presently
               // multiples of 4.
static inline thread_id_t thread_id() {
  return static_cast<thread_id_t>(::GetCurrentThreadId());
}
} // namespace details
} // namespace tmc::queue
#elif (defined(__APPLE__) && TARGET_OS_IPHONE) || defined(__MVS__) ||          \
  defined(MOODYCAMEL_NO_THREAD_LOCAL)
namespace tmc::queue {
namespace details {
static_assert(
  sizeof(std::thread::id) == 4 || sizeof(std::thread::id) == 8,
  "std::thread::id is expected to be either 4 or 8 bytes"
);

typedef std::thread::id thread_id_t;
static const thread_id_t invalid_thread_id; // Default ctor creates invalid ID

// Note we don't define a invalid_thread_id2 since std::thread::id doesn't have
// one; it's only used if MOODYCAMEL_CPP11_THREAD_LOCAL_SUPPORTED is defined
// anyway, which it won't be.
static inline thread_id_t thread_id() { return std::this_thread::get_id(); }

template <std::size_t> struct thread_id_size {};
template <> struct thread_id_size<4> {
  typedef std::uint32_t numeric_t;
};
template <> struct thread_id_size<8> {
  typedef std::uint64_t numeric_t;
};

template <> struct thread_id_converter<thread_id_t> {
  typedef thread_id_size<sizeof(thread_id_t)>::numeric_t
    thread_id_numeric_size_t;
#ifndef __APPLE__
  typedef std::size_t thread_id_hash_t;
#else
  typedef thread_id_numeric_size_t thread_id_hash_t;
#endif

  static thread_id_hash_t prehash(thread_id_t const& x) {
#ifndef __APPLE__
    return std::hash<std::thread::id>()(x);
#else
    return *reinterpret_cast<thread_id_hash_t const*>(&x);
#endif
  }
};
} // namespace details
} // namespace tmc::queue
#else
// Use a nice trick from this answer: http://stackoverflow.com/a/8438730/21475
// In order to get a numeric thread ID in a platform-independent way, we use a
// thread-local static variable's address as a thread identifier :-)
namespace tmc::queue {
namespace details {
typedef std::uintptr_t thread_id_t;
static const thread_id_t invalid_thread_id = 0; // Address can't be nullptr
static const thread_id_t invalid_thread_id2 =
  1; // Member accesses off a null pointer are also generally invalid. Plus
     // it's not aligned.
inline thread_id_t thread_id() {
  static thread_local int x;
  return reinterpret_cast<thread_id_t>(&x);
}
} // namespace details
} // namespace tmc::queue
#endif

// Exceptions
#ifndef MOODYCAMEL_EXCEPTIONS_ENABLED
#if (defined(_MSC_VER) && defined(_CPPUNWIND)) ||                              \
  (defined(__GNUC__) && defined(__EXCEPTIONS)) ||                              \
  (!defined(_MSC_VER) && !defined(__GNUC__))
#define MOODYCAMEL_EXCEPTIONS_ENABLED
#endif
#endif
#ifdef MOODYCAMEL_EXCEPTIONS_ENABLED
#define MOODYCAMEL_TRY try
#define MOODYCAMEL_CATCH(...) catch (__VA_ARGS__)
#define MOODYCAMEL_RETHROW throw
#define MOODYCAMEL_THROW(expr) throw(expr)
#else
#define MOODYCAMEL_TRY if constexpr (true)
#define MOODYCAMEL_CATCH(...) else if constexpr (false)
#define MOODYCAMEL_RETHROW
#define MOODYCAMEL_THROW(expr)
#endif

#ifndef MOODYCAMEL_NOEXCEPT
#if !defined(MOODYCAMEL_EXCEPTIONS_ENABLED)
#define MOODYCAMEL_NOEXCEPT
#define MOODYCAMEL_NOEXCEPT_CTOR(expr) true
#define MOODYCAMEL_NOEXCEPT_ASSIGN(expr) true
#else
#define MOODYCAMEL_NOEXCEPT noexcept
#define MOODYCAMEL_NOEXCEPT_CTOR(expr) noexcept(expr)
#define MOODYCAMEL_NOEXCEPT_ASSIGN(expr) noexcept(expr)
#endif
#endif

namespace tmc::queue {
namespace details {
#ifndef MOODYCAMEL_ALIGNAS
// VS2013 doesn't support alignas or alignof, and align() requires a constant
// literal
#if defined(_MSC_VER) && _MSC_VER <= 1800
#define MOODYCAMEL_ALIGNAS(alignment) __declspec(align(alignment))
#define MOODYCAMEL_ALIGNOF(obj) __alignof(obj)
#define MOODYCAMEL_ALIGNED_TYPE_LIKE(T, obj)                                   \
  typename details::Vs2013Aligned<std::alignment_of<obj>::value, T>::type
template <int Align, typename T>
struct Vs2013Aligned {}; // default, unsupported alignment
template <typename T> struct Vs2013Aligned<1, T> {
  typedef __declspec(align(1)) T type;
};
template <typename T> struct Vs2013Aligned<2, T> {
  typedef __declspec(align(2)) T type;
};
template <typename T> struct Vs2013Aligned<4, T> {
  typedef __declspec(align(4)) T type;
};
template <typename T> struct Vs2013Aligned<8, T> {
  typedef __declspec(align(8)) T type;
};
template <typename T> struct Vs2013Aligned<16, T> {
  typedef __declspec(align(16)) T type;
};
template <typename T> struct Vs2013Aligned<32, T> {
  typedef __declspec(align(32)) T type;
};
template <typename T> struct Vs2013Aligned<64, T> {
  typedef __declspec(align(64)) T type;
};
template <typename T> struct Vs2013Aligned<128, T> {
  typedef __declspec(align(128)) T type;
};
template <typename T> struct Vs2013Aligned<256, T> {
  typedef __declspec(align(256)) T type;
};
#else
template <typename T> struct identity {
  typedef T type;
};
#define MOODYCAMEL_ALIGNAS(alignment) alignas(alignment)
#define MOODYCAMEL_ALIGNOF(obj) alignof(obj)
#define MOODYCAMEL_ALIGNED_TYPE_LIKE(T, obj)                                   \
  alignas(alignof(obj)) typename details::identity<T>::type
#endif
#endif
} // namespace details
} // namespace tmc::queue

// TSAN can false report races in lock-free code.  To enable TSAN to be used
// from projects that use this one, we can apply per-function compile-time
// suppression. See
// https://clang.llvm.org/docs/ThreadSanitizer.html#has-feature-thread-sanitizer
#define MOODYCAMEL_NO_TSAN
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#undef MOODYCAMEL_NO_TSAN
#define MOODYCAMEL_NO_TSAN __attribute__((no_sanitize("thread")))
#endif // TSAN
#endif // TSAN

#ifdef MOODYCAMEL_QUEUE_INTERNAL_DEBUG
#include "internal/concurrentqueue_internal_debug.h"
#endif

namespace tmc::queue {
namespace details {
template <typename T> struct const_numeric_max {
  static_assert(
    std::is_integral<T>::value,
    "const_numeric_max can only be used with integers"
  );
  static const T value =
    std::numeric_limits<T>::is_signed
      ? (static_cast<T>(1) << (sizeof(T) * CHAR_BIT - 1)) - static_cast<T>(1)
      : static_cast<T>(-1);
};

#if defined(__GLIBCXX__)
typedef ::max_align_t
  std_max_align_t; // libstdc++ forgot to add it to std:: for a while
#else
typedef std::max_align_t std_max_align_t; // Others (e.g. MSVC) insist it can
                                          // *only* be accessed via std::
#endif

// Some platforms have incorrectly set max_align_t to a type with <8 bytes
// alignment even while supporting 8-byte aligned scalar values (*cough* 32-bit
// iOS). Work around this with our own union. See issue #64.
typedef union {
  std_max_align_t x;
  long long y;
  void* z;
} max_align_t;
} // namespace details

// Default traits for the ConcurrentQueue. To change some of the
// traits without re-implementing all of them, inherit from this
// struct and shadow the declarations you wish to be different;
// since the traits are used as a template type parameter, the
// shadowed declarations will be used where defined, and the defaults
// otherwise.
struct ConcurrentQueueDefaultTraits {
  // General-purpose size type. std::size_t is strongly recommended.
  typedef std::size_t size_t;

  // The type used for the enqueue and dequeue indices. Must be at least as
  // large as size_t. Should be significantly larger than the number of elements
  // you expect to hold at once, especially if you have a high turnover rate;
  // for example, on 32-bit x86, if you expect to have over a hundred million
  // elements or pump several million elements through your queue in a very
  // short space of time, using a 32-bit type *may* trigger a race condition.
  // A 64-bit int type is recommended in that case, and in practice will
  // prevent a race condition no matter the usage of the queue. Note that
  // whether the queue is lock-free with a 64-int type depends on the whether
  // std::atomic<std::size_t> is lock-free, which is platform-specific.
  typedef std::size_t index_t;

  // Internally, all elements are enqueued and dequeued from multi-element
  // blocks; this is the smallest controllable unit. If you expect few elements
  // but many producers, a smaller block size should be favoured. For few
  // producers and/or many elements, a larger block size is preferred. A sane
  // default is provided. Must be a power of 2.
  static const size_t PRODUCER_BLOCK_SIZE = 512;

  // TZCNT: setting this to 2 has a small positive impact on some benchmarks,
  // and negative impact on others.
  static const size_t ELEM_INTERLEAVING = 1;

  // How many full blocks can be expected for a single explicit producer? This
  // should reflect that number's maximum for optimal performance. Must be a
  // power of 2.
  static const size_t EXPLICIT_INITIAL_INDEX_SIZE = 32;

  // How many full blocks can be expected for a single implicit producer? This
  // should reflect that number's maximum for optimal performance. Must be a
  // power of 2.
  static const size_t IMPLICIT_INITIAL_INDEX_SIZE = 32;

  // The initial size of the hash table mapping thread IDs to implicit
  // producers. Note that the hash is resized every time it becomes half full.
  // Must be a power of two, and either 0 or at least 1. This must be non-zero.
  static const size_t INITIAL_IMPLICIT_PRODUCER_HASH_SIZE = 32;

  // The maximum number of elements (inclusive) that can be enqueued to a
  // sub-queue. Enqueue operations that would cause this limit to be surpassed
  // will fail. Note that this limit is enforced at the block level (for
  // performance reasons), i.e. it's rounded up to the nearest block size.
  static const size_t MAX_SUBQUEUE_SIZE =
    details::const_numeric_max<size_t>::value;

  // Whether to recycle dynamically-allocated blocks into an internal free list
  // or not. If false, only pre-allocated blocks (controlled by the constructor
  // arguments) will be recycled, and all others will be `free`d back to the
  // heap. Note that blocks consumed by explicit producers are only freed on
  // destruction of the queue (not following destruction of the token)
  // regardless of this trait.
  static const bool RECYCLE_ALLOCATED_BLOCKS = false;

#ifndef MCDBGQ_USE_RELACY
  // Memory allocation can be customized if needed.
  // malloc should return nullptr on failure, and handle alignment like
  // std::malloc.
#if defined(malloc) || defined(free)
  // Gah, this is 2015, stop defining macros that break standard code already!
  // Work around malloc/free being special macros:
  static inline void* WORKAROUND_malloc(size_t size) { return malloc(size); }
  static inline void WORKAROUND_free(void* ptr) { return free(ptr); }
  static inline void*(malloc)(size_t size) { return WORKAROUND_malloc(size); }
  static inline void(free)(void* ptr) { return WORKAROUND_free(ptr); }
#else
  static inline void* malloc(size_t size) { return std::malloc(size); }
  static inline void free(void* ptr) { return std::free(ptr); }
#endif
#else
  // Debug versions when running under the Relacy race detector (ignore
  // these in user code)
  static inline void* malloc(size_t size) { return rl::rl_malloc(size, $); }
  static inline void free(void* ptr) { return rl::rl_free(ptr, $); }
#endif
};

template <typename T, typename Traits> class ConcurrentQueue;
class ConcurrentQueueTests;

namespace details {
struct alignas(64) ConcurrentQueueProducerTypelessBase {
  ConcurrentQueueProducerTypelessBase* next;
  std::atomic<bool> inactive;

  ConcurrentQueueProducerTypelessBase() : next(nullptr), inactive(false) {}
};

template <bool use64> struct _hash_32_or_64 {
  static inline std::uint32_t hash(std::uint32_t h) {
    // MurmurHash3 finalizer -- see
    // https://code.google.com/p/smhasher/source/browse/trunk/MurmurHash3.cpp
    // Since the thread ID is already unique, all we really want to do is
    // propagate that uniqueness evenly across all the bits, so that we can use
    // a subset of the bits while reducing collisions significantly
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    return h ^ (h >> 16);
  }
};
template <> struct _hash_32_or_64<1> {
  static inline std::uint64_t hash(std::uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccd;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53;
    return h ^ (h >> 33);
  }
};
template <std::size_t size>
struct hash_32_or_64 : public _hash_32_or_64<(size > 4)> {};

static inline size_t hash_thread_id(thread_id_t id) {
  static_assert(
    sizeof(thread_id_t) <= 8,
    "Expected a platform where thread IDs are at most 64-bit values"
  );
  return static_cast<size_t>(
    hash_32_or_64<sizeof(thread_id_converter<thread_id_t>::thread_id_hash_t
    )>::hash(thread_id_converter<thread_id_t>::prehash(id))
  );
}

template <typename T> static inline bool circular_less_than(T a, T b) {
  static_assert(
    std::is_integral<T>::value && !std::numeric_limits<T>::is_signed,
    "circular_less_than is intended to be used only with unsigned "
    "integer types"
  );
  return static_cast<T>(a - b) >
         static_cast<T>(
           static_cast<T>(1) << (static_cast<T>(sizeof(T) * CHAR_BIT - 1))
         );
  // Note: extra parens around rhs of operator<< is MSVC bug:
  // https://developercommunity2.visualstudio.com/t/C4554-triggers-when-both-lhs-and-rhs-is/10034931
  //       silencing the bug requires #pragma warning(disable: 4554) around the
  //       calling code and has no effect when done here.
}

template <typename U> static inline char* align_for(char* ptr) {
  const std::size_t alignment = std::alignment_of<U>::value;
  return ptr +
         (alignment - (reinterpret_cast<std::uintptr_t>(ptr) % alignment)) %
           alignment;
}

template <typename T> static inline T ceil_to_pow_2(T x) {
  static_assert(
    std::is_integral<T>::value && !std::numeric_limits<T>::is_signed,
    "ceil_to_pow_2 is intended to be used only with unsigned integer types"
  );

  // Adapted from
  // http://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2
  --x;
  x |= x >> 1;
  x |= x >> 2;
  x |= x >> 4;
  for (std::size_t i = 1; i < sizeof(T); i <<= 1) {
    x |= x >> (i << 3);
  }
  ++x;
  return x;
}

template <typename T> static inline T const& nomove(T const& x) { return x; }

template <typename It>
static inline auto deref_noexcept(It& it) MOODYCAMEL_NOEXCEPT -> decltype(*it) {
  return *it;
}

#if defined(__clang__) || !defined(__GNUC__) || __GNUC__ > 4 ||                \
  (__GNUC__ == 4 && __GNUC_MINOR__ >= 8)
template <typename T>
struct is_trivially_destructible : std::is_trivially_destructible<T> {};
#else
template <typename T>
struct is_trivially_destructible : std::has_trivial_destructor<T> {};
#endif

#ifdef MCDBGQ_USE_RELACY
typedef RelacyThreadExitListener ThreadExitListener;
typedef RelacyThreadExitNotifier ThreadExitNotifier;
#else
class ThreadExitNotifier;

struct ThreadExitListener {
  typedef void (*callback_t)(void*);
  callback_t callback;
  void* userData;

  ThreadExitListener* next;  // reserved for use by the ThreadExitNotifier
  ThreadExitNotifier* chain; // reserved for use by the ThreadExitNotifier
};

class ThreadExitNotifier {
public:
  static void subscribe(ThreadExitListener* listener) {
    auto& tlsInst = instance();
    tmc::tiny_lock_guard lg{mutex()};
    listener->next = tlsInst.tail;
    listener->chain = &tlsInst;
    tlsInst.tail = listener;
  }

  static void unsubscribe(ThreadExitListener* listener) {
    tmc::tiny_lock_guard lg{mutex()};
    if (!listener->chain) {
      return; // race with ~ThreadExitNotifier
    }
    auto& tlsInst = *listener->chain;
    listener->chain = nullptr;
    ThreadExitListener** prev = &tlsInst.tail;
    for (auto ptr = tlsInst.tail; ptr != nullptr; ptr = ptr->next) {
      if (ptr == listener) {
        *prev = ptr->next;
        break;
      }
      prev = &ptr->next;
    }
  }

private:
  ThreadExitNotifier() : tail(nullptr) {}
  ThreadExitNotifier(ThreadExitNotifier const&) = delete;
  ThreadExitNotifier& operator=(ThreadExitNotifier const&) = delete;

  ~ThreadExitNotifier() {
    // This thread is about to exit, let everyone know!
    assert(
      this == &instance() &&
      "If this assert fails, you likely have a buggy compiler! Change the "
      "preprocessor "
      "conditions such that MOODYCAMEL_CPP11_THREAD_LOCAL_SUPPORTED is no "
      "longer defined."
    );
    tmc::tiny_lock_guard lg{mutex()};
    for (auto ptr = tail; ptr != nullptr; ptr = ptr->next) {
      ptr->chain = nullptr;
      ptr->callback(ptr->userData);
    }
  }

  // Thread-local
  static inline ThreadExitNotifier& instance() {
    static thread_local ThreadExitNotifier notifier;
    return notifier;
  }

  static inline tmc::tiny_lock& mutex() {
    // Must be static because the ThreadExitNotifier could be destroyed while
    // unsubscribe is called
    static tmc::tiny_lock mutex;
    return mutex;
  }

private:
  ThreadExitListener* tail;
};
#endif

template <typename T> struct static_is_lock_free_num {
  enum { value = 0 };
};
template <> struct static_is_lock_free_num<signed char> {
  enum { value = ATOMIC_CHAR_LOCK_FREE };
};
template <> struct static_is_lock_free_num<short> {
  enum { value = ATOMIC_SHORT_LOCK_FREE };
};
template <> struct static_is_lock_free_num<int> {
  enum { value = ATOMIC_INT_LOCK_FREE };
};
template <> struct static_is_lock_free_num<long> {
  enum { value = ATOMIC_LONG_LOCK_FREE };
};
template <> struct static_is_lock_free_num<long long> {
  enum { value = ATOMIC_LLONG_LOCK_FREE };
};
template <typename T>
struct static_is_lock_free
    : static_is_lock_free_num<typename std::make_signed<T>::type> {};
template <> struct static_is_lock_free<bool> {
  enum { value = ATOMIC_BOOL_LOCK_FREE };
};
template <typename U> struct static_is_lock_free<U*> {
  enum { value = ATOMIC_POINTER_LOCK_FREE };
};
} // namespace details

template <typename T, typename Traits = ConcurrentQueueDefaultTraits>
class ConcurrentQueue {
public:
  typedef typename Traits::index_t index_t;
  typedef typename Traits::size_t size_t;

  static constexpr size_t PRODUCER_BLOCK_SIZE =
    static_cast<size_t>(Traits::PRODUCER_BLOCK_SIZE);
  static constexpr size_t BLOCK_MASK =
    static_cast<size_t>(Traits::PRODUCER_BLOCK_SIZE) - 1;

  static constexpr size_t BLOCK_EMPTY_ELEM_SIZE =
    PRODUCER_BLOCK_SIZE < TMC_PLATFORM_BITS ? PRODUCER_BLOCK_SIZE
                                            : TMC_PLATFORM_BITS;
  static constexpr size_t BLOCK_EMPTY_MASK =
    PRODUCER_BLOCK_SIZE < TMC_PLATFORM_BITS
      ? (TMC_ONE_BIT << Traits::PRODUCER_BLOCK_SIZE) - 1
      : TMC_ALL_ONES;
  static constexpr size_t BLOCK_EMPTY_ARRAY_SIZE =
    PRODUCER_BLOCK_SIZE < TMC_PLATFORM_BITS
      ? 1
      : (PRODUCER_BLOCK_SIZE / TMC_PLATFORM_BITS);

  static constexpr size_t PLATFORM_CACHELINE_BYTES = 64;

  static constexpr size_t ELEM_INTERLEAVING = Traits::ELEM_INTERLEAVING;
  static constexpr size_t ELEM_INTERLEAVING_MASK = ELEM_INTERLEAVING - 1;
  static constexpr size_t ELEMS_PER_CACHELINE =
    PLATFORM_CACHELINE_BYTES / sizeof(T);
  static constexpr size_t ELEM_INTERLEAVING_ALL_MASK =
    (ELEMS_PER_CACHELINE * ELEM_INTERLEAVING) - 1;

  static constexpr size_t EXPLICIT_INITIAL_INDEX_SIZE =
    static_cast<size_t>(Traits::EXPLICIT_INITIAL_INDEX_SIZE);
  static constexpr size_t IMPLICIT_INITIAL_INDEX_SIZE =
    static_cast<size_t>(Traits::IMPLICIT_INITIAL_INDEX_SIZE);
  static constexpr size_t INITIAL_IMPLICIT_PRODUCER_HASH_SIZE =
    static_cast<size_t>(Traits::INITIAL_IMPLICIT_PRODUCER_HASH_SIZE);
  static constexpr size_t MAX_SUBQUEUE_SIZE =
    (details::const_numeric_max<size_t>::value -
       static_cast<size_t>(Traits::MAX_SUBQUEUE_SIZE) <
     PRODUCER_BLOCK_SIZE)
      ? details::const_numeric_max<size_t>::value
      : ((static_cast<size_t>(Traits::MAX_SUBQUEUE_SIZE) + (BLOCK_MASK)) /
         PRODUCER_BLOCK_SIZE * PRODUCER_BLOCK_SIZE);

  static_assert(
    !std::numeric_limits<size_t>::is_signed && std::is_integral<size_t>::value,
    "Traits::size_t must be an unsigned integral type"
  );
  static_assert(
    !std::numeric_limits<index_t>::is_signed &&
      std::is_integral<index_t>::value,
    "Traits::index_t must be an unsigned integral type"
  );
  static_assert(
    sizeof(index_t) >= sizeof(size_t),
    "Traits::index_t must be at least as wide as Traits::size_t"
  );
  static_assert(
    (PRODUCER_BLOCK_SIZE > 1) && !(PRODUCER_BLOCK_SIZE & (BLOCK_MASK)),
    "Traits::PRODUCER_BLOCK_SIZE must be a power of 2 (and at least 2)"
  );
  static_assert(
    (BLOCK_EMPTY_ARRAY_SIZE >= 1) &&
      !(BLOCK_EMPTY_ARRAY_SIZE & (BLOCK_EMPTY_ARRAY_SIZE - 1)),
    "Traits::BLOCK_EMPTY_ARRAY_SIZE must be a power of 2 (and at least 1)"
  );
  static_assert(
    (EXPLICIT_INITIAL_INDEX_SIZE > 1) &&
      !(EXPLICIT_INITIAL_INDEX_SIZE & (EXPLICIT_INITIAL_INDEX_SIZE - 1)),
    "Traits::EXPLICIT_INITIAL_INDEX_SIZE must be a power of 2 (and "
    "greater than 1)"
  );
  static_assert(
    (IMPLICIT_INITIAL_INDEX_SIZE > 1) &&
      !(IMPLICIT_INITIAL_INDEX_SIZE & (IMPLICIT_INITIAL_INDEX_SIZE - 1)),
    "Traits::IMPLICIT_INITIAL_INDEX_SIZE must be a power of 2 (and "
    "greater than 1)"
  );
  static_assert(
    (INITIAL_IMPLICIT_PRODUCER_HASH_SIZE == 0) ||
      !(INITIAL_IMPLICIT_PRODUCER_HASH_SIZE &
        (INITIAL_IMPLICIT_PRODUCER_HASH_SIZE - 1)),
    "Traits::INITIAL_IMPLICIT_PRODUCER_HASH_SIZE must be a power of 2"
  );
  static_assert(
    INITIAL_IMPLICIT_PRODUCER_HASH_SIZE == 0 ||
      INITIAL_IMPLICIT_PRODUCER_HASH_SIZE >= 1,
    "Traits::INITIAL_IMPLICIT_PRODUCER_HASH_SIZE must be at least "
    "1 (or 0 to disable implicit enqueueing)"
  );

public:
  struct ExplicitProducer;

  // Creates a queue with `capacity` preallocated blocks. This method is not
  // thread safe -- it is up to the user to ensure that the queue is fully
  // constructed before it starts being used by other threads (this includes
  // making the memory effects of construction visible, possibly with a memory
  // barrier).
  explicit ConcurrentQueue(size_t capacity = 1)
      : producerListTail(nullptr), producerCount(0), initialBlockPoolIndex(0),
        nextExplicitConsumerId(0), globalExplicitConsumerOffset(0) {
    implicitProducerHashResizeInProgress.clear(std::memory_order_relaxed);
    populate_initial_implicit_producer_hash();
    populate_initial_block_list(capacity);

#ifdef MOODYCAMEL_QUEUE_INTERNAL_DEBUG
    // Track all the producers using a fully-resolved typed list for
    // each kind; this makes it possible to debug them starting from
    // the root queue object (otherwise wacky casts are needed that
    // don't compile in the debugger's expression evaluator).
    explicitProducers.store(nullptr, std::memory_order_relaxed);
    implicitProducers.store(nullptr, std::memory_order_relaxed);
#endif
  }

  // Computes the correct amount of pre-allocated blocks for you based
  // on the minimum number of elements you want available at any given
  // time, and the maximum concurrent number of each type of producer.
  ConcurrentQueue(
    size_t minCapacity, size_t maxExplicitProducers, size_t maxImplicitProducers
  )
      : producerListTail(nullptr), producerCount(0), initialBlockPoolIndex(0),
        nextExplicitConsumerId(0), globalExplicitConsumerOffset(0) {
    implicitProducerHashResizeInProgress.clear(std::memory_order_relaxed);
    populate_initial_implicit_producer_hash();
    size_t blocks = (((minCapacity + BLOCK_MASK) / PRODUCER_BLOCK_SIZE) - 1) *
                      (maxExplicitProducers + 1) +
                    2 * (maxExplicitProducers + maxImplicitProducers);
    populate_initial_block_list(blocks);

#ifdef MOODYCAMEL_QUEUE_INTERNAL_DEBUG
    explicitProducers.store(nullptr, std::memory_order_relaxed);
    implicitProducers.store(nullptr, std::memory_order_relaxed);
#endif
  }

  // Note: The queue should not be accessed concurrently while it's
  // being deleted. It's up to the user to synchronize this.
  // This method is not thread safe.
  ~ConcurrentQueue() {
    // Destroy producers
    auto ptr = producerListTail.load(std::memory_order_relaxed);
    while (ptr != nullptr) {
      auto next = ptr->next_prod();
      destroy(ptr);
      ptr = next;
    }

    // Destroy implicit producer hash tables
    if constexpr (INITIAL_IMPLICIT_PRODUCER_HASH_SIZE != 0) {
      auto hash = implicitProducerHash.load(std::memory_order_relaxed);
      while (hash != nullptr) {
        auto prev = hash->prev;
        if (prev != nullptr) { // The last hash is part of this object and was
                               // not allocated dynamically
          for (size_t i = 0; i != hash->capacity; ++i) {
            hash->entries[i].~ImplicitProducerKVP();
          }
          hash->~ImplicitProducerHash();
          (Traits::free)(hash);
        }
        hash = prev;
      }
    }

    // Destroy global free list
    auto block = freeList.head_unsafe();
    while (block != nullptr) {
      auto next = block->freeListNext.load(std::memory_order_relaxed);
      if (block->dynamicallyAllocated) {
        destroy(block);
      }
      block = next;
    }

    // Destroy initial free list
    destroy_array(initialBlockPool, initialBlockPoolSize);
  }

  // Disable copying and copy assignment
  ConcurrentQueue(ConcurrentQueue const&) = delete;
  ConcurrentQueue& operator=(ConcurrentQueue const&) = delete;

  ConcurrentQueue(ConcurrentQueue&& other) = delete;

  inline ConcurrentQueue& operator=(ConcurrentQueue&& other) = delete;

  // Enqueues a single item (by copying it).
  // Allocates memory if required. Only fails if memory allocation fails (or
  // implicit production is disabled because
  // Traits::INITIAL_IMPLICIT_PRODUCER_HASH_SIZE is 0, or
  // Traits::MAX_SUBQUEUE_SIZE has been defined and would be surpassed).
  // Thread-safe.
  inline bool enqueue(T const& item) {
    if constexpr (INITIAL_IMPLICIT_PRODUCER_HASH_SIZE == 0)
      return false;
    else
      return inner_enqueue<CanAlloc>(item);
  }

  // Enqueues a single item (by moving it, if possible).
  // Allocates memory if required. Only fails if memory allocation fails (or
  // implicit production is disabled because
  // Traits::INITIAL_IMPLICIT_PRODUCER_HASH_SIZE is 0, or
  // Traits::MAX_SUBQUEUE_SIZE has been defined and would be surpassed).
  // Thread-safe.
  inline bool enqueue(T&& item) {
    if constexpr (INITIAL_IMPLICIT_PRODUCER_HASH_SIZE == 0)
      return false;
    else
      return inner_enqueue<CanAlloc>(static_cast<T&&>(item));
  }

  inline bool enqueue_ex_cpu(T const& item, size_t priority) {
    ExplicitProducer** producers =
      static_cast<ExplicitProducer**>(tmc::detail::this_thread::producers);
    ExplicitProducer* this_thread_prod =
      static_cast<ExplicitProducer*>(producers[priority * dequeueProducerCount]
      );
    return this_thread_prod->template enqueue<CanAlloc>(item);
  }

  inline bool enqueue_ex_cpu(T&& item, size_t priority) {
    ExplicitProducer** producers =
      static_cast<ExplicitProducer**>(tmc::detail::this_thread::producers);
    ExplicitProducer* this_thread_prod =
      static_cast<ExplicitProducer*>(producers[priority * dequeueProducerCount]
      );
    return this_thread_prod->template enqueue<CanAlloc>(static_cast<T&&>(item));
  }

  // Enqueues several items.
  // Allocates memory if required. Only fails if memory allocation fails (or
  // implicit production is disabled because
  // Traits::INITIAL_IMPLICIT_PRODUCER_HASH_SIZE is 0, or
  // Traits::MAX_SUBQUEUE_SIZE has been defined and would be surpassed). Note:
  // Use std::make_move_iterator if the elements should be moved instead of
  // copied. Thread-safe.
  template <typename It> bool enqueue_bulk(It itemFirst, size_t count) {
    if constexpr (INITIAL_IMPLICIT_PRODUCER_HASH_SIZE == 0)
      return false;
    else
      return inner_enqueue_bulk<CanAlloc>(itemFirst, count);
  }

  template <typename It>
  bool enqueue_bulk_ex_cpu(It itemFirst, size_t count, size_t priority) {
    ExplicitProducer** producers =
      static_cast<ExplicitProducer**>(tmc::detail::this_thread::producers);
    if (producers != nullptr) {
      ExplicitProducer* this_thread_prod = static_cast<ExplicitProducer*>(
        producers[priority * dequeueProducerCount]
      );
      return this_thread_prod->template enqueue_bulk<CanAlloc>(
        itemFirst, count
      );
    }

    if constexpr (INITIAL_IMPLICIT_PRODUCER_HASH_SIZE == 0) {
      return false;
    }
    return inner_enqueue_bulk<CanAlloc>(itemFirst, count);
  }

  // Enqueues a single item (by copying it).
  // Does not allocate memory. Fails if not enough room to enqueue (or implicit
  // production is disabled because Traits::INITIAL_IMPLICIT_PRODUCER_HASH_SIZE
  // is 0).
  // Thread-safe.
  inline bool try_enqueue(T const& item) {
    if constexpr (INITIAL_IMPLICIT_PRODUCER_HASH_SIZE == 0)
      return false;
    else
      return inner_enqueue<CannotAlloc>(item);
  }

  // Enqueues a single item (by moving it, if possible).
  // Does not allocate memory (except for one-time implicit producer).
  // Fails if not enough room to enqueue (or implicit production is
  // disabled because Traits::INITIAL_IMPLICIT_PRODUCER_HASH_SIZE is 0).
  // Thread-safe.
  inline bool try_enqueue(T&& item) {
    if constexpr (INITIAL_IMPLICIT_PRODUCER_HASH_SIZE == 0)
      return false;
    else
      return inner_enqueue<CannotAlloc>(static_cast<T&&>(item));
  }

  // Enqueues several items.
  // Does not allocate memory (except for one-time implicit producer).
  // Fails if not enough room to enqueue (or implicit production is
  // disabled because Traits::INITIAL_IMPLICIT_PRODUCER_HASH_SIZE is 0).
  // Note: Use std::make_move_iterator if the elements should be moved
  // instead of copied.
  // Thread-safe.
  template <typename It> bool try_enqueue_bulk(It itemFirst, size_t count) {
    if constexpr (INITIAL_IMPLICIT_PRODUCER_HASH_SIZE == 0)
      return false;
    else
      return inner_enqueue_bulk<CannotAlloc>(itemFirst, count);
  }

  // Attempts to dequeue from the queue.
  // Returns false if all producer streams appeared empty at the time they
  // were checked (so, the queue is likely but not guaranteed to be empty).
  // Never allocates. Thread-safe.
  template <typename U> bool try_dequeue(U& item) {
    // Instead of simply trying each producer in turn (which could cause
    // needless contention on the first producer), we score them heuristically.
    size_t nonEmptyCount = 0;
    ProducerBase* best = nullptr;
    size_t bestSize = 0;
    for (auto ptr = producerListTail.load(std::memory_order_acquire);
         nonEmptyCount < 3 && ptr != nullptr; ptr = ptr->next_prod()) {
      auto size = ptr->size_approx();
      if (size > 0) {
        if (size > bestSize) {
          bestSize = size;
          best = ptr;
        }
        ++nonEmptyCount;
      }
    }

    // If there was at least one non-empty queue but it appears empty at the
    // time we try to dequeue from it, we need to make sure every queue's been
    // tried
    if (nonEmptyCount > 0) {
      if (best->dequeue(item)) [[likely]] {
        return true;
      }
      for (auto ptr = producerListTail.load(std::memory_order_acquire);
           ptr != nullptr; ptr = ptr->next_prod()) {
        if (ptr != best && ptr->dequeue(item)) {
          return true;
        }
      }
    }
    return false;
  }

  // Attempts to dequeue from the queue.
  // Returns false if all producer streams appeared empty at the time they
  // were checked (so, the queue is likely but not guaranteed to be empty).
  // This differs from the try_dequeue(item) method in that this one does
  // not attempt to reduce contention by interleaving the order that producer
  // streams are dequeued from. So, using this method can reduce overall
  // throughput under contention, but will give more predictable results in
  // single-threaded consumer scenarios. This is mostly only useful for internal
  // unit tests. Never allocates. Thread-safe.
  template <typename U> bool try_dequeue_non_interleaved(U& item) {
    for (auto ptr = producerListTail.load(std::memory_order_acquire);
         ptr != nullptr; ptr = ptr->next_prod()) {
      if (ptr->dequeue(item)) {
        return true;
      }
    }
    return false;
  }

  // TZCNT MODIFIED: New function, used only by ex_cpu threads. Uses
  // precalculated iteration order to check queues.
  TMC_FORCE_INLINE bool try_dequeue_ex_cpu(
    T& item, size_t prio, tmc::detail::qu_inbox<T, 4096>* inbox
  ) {
    auto dequeue_count = dequeueProducerCount;
    size_t baseOffset = prio * dequeue_count;
    ExplicitProducer** producers =
      static_cast<ExplicitProducer**>(tmc::detail::this_thread::producers) +
      baseOffset;
    // CHECK this thread's work queue first
    // this thread's producer is always the first element of the producers array
    if (static_cast<ExplicitProducer*>(producers[0])->dequeue_lifo(item)) {
      return true;
    }

    if (inbox != nullptr && inbox->try_pull(item)) {
      return true;
    }

    // CHECK the implicit producers (main thread, I/O, etc)
    ImplicitProducer* implicit_prod = static_cast<ImplicitProducer*>(
      producerListTail.load(std::memory_order_acquire)
    );
    while (implicit_prod != nullptr) {
      if (implicit_prod->dequeue(item)) {
        return true;
      }
      implicit_prod =
        static_cast<ImplicitProducer*>(implicit_prod->next_prod());
    }

    // producers[1] is the previously consumed-from producer
    // If we didn't find work last time, it will be null.
    size_t pidx = producers[1] == nullptr ? 2 : 1;

    // CHECK the remaining threads in the predefined order
    for (; pidx < dequeue_count; ++pidx) {
      ExplicitProducer* prod = static_cast<ExplicitProducer*>(producers[pidx]);
      if (prod->dequeue(item)) {
        // update prev_prod
        producers[1] = prod;
        return true;
      }
    }

    // Some synthetic benchmarks get 1-2% faster if this line is commented
    // out, but I think that might have undesirable side effects
    producers[1] = nullptr;
    return false;
  }

  // Attempts to dequeue several elements from the queue.
  // Returns the number of items actually dequeued.
  // Returns 0 if all producer streams appeared empty at the time they
  // were checked (so, the queue is likely but not guaranteed to be empty).
  // Never allocates. Thread-safe.
  template <typename It> size_t try_dequeue_bulk(It itemFirst, size_t max) {
    size_t count = 0;
    for (auto ptr = producerListTail.load(std::memory_order_acquire);
         ptr != nullptr; ptr = ptr->next_prod()) {
      count += ptr->dequeue_bulk(itemFirst, max - count);
      if (count == max) {
        break;
      }
    }
    return count;
  }

  // Returns an estimate of the total number of elements currently in the queue.
  // This estimate is only accurate if the queue has completely stabilized
  // before it is called (i.e. all enqueue and dequeue operations have completed
  // and their memory effects are visible on the calling thread, and no further
  // operations start while this method is being called). Thread-safe.
  size_t size_approx() const {
    size_t size = 0;
    for (auto ptr = producerListTail.load(std::memory_order_acquire);
         ptr != nullptr; ptr = ptr->next_prod()) {
      size += ptr->size_approx();
    }
    return size;
  }

  // Returns an estimate of whether or not the queue is empty. This
  // estimate is only accurate if the queue has completely stabilized before it
  // is called (i.e. all enqueue and dequeue operations have completed and their
  // memory effects are visible on the calling thread, and no further operations
  // start while this method is being called). Thread-safe.
  bool empty() const {
    // TODO make a producer thread version of this that uses this thread's
    // static iteration order
    auto static_producer_count =
      static_cast<ptrdiff_t>(dequeueProducerCount) - 1;
    for (ptrdiff_t pidx = 0; pidx < static_producer_count; ++pidx) {
      ExplicitProducer& prod = staticProducers[pidx];
      if (prod.size_approx() != 0) {
        return false;
      }
    }

    for (auto ptr = producerListTail.load(std::memory_order_seq_cst);
         ptr != nullptr; ptr = ptr->next_prod()) {
      if (ptr->size_approx() != 0) {
        return false;
      }
    }
    return true;
  }

  // Returns true if the underlying atomic variables used by
  // the queue are lock-free (they should be on most platforms).
  // Thread-safe.
  static constexpr bool is_lock_free() {
    return details::static_is_lock_free<bool>::value == 2 &&
           details::static_is_lock_free<size_t>::value == 2 &&
           details::static_is_lock_free<std::uint32_t>::value == 2 &&
           details::static_is_lock_free<index_t>::value == 2 &&
           details::static_is_lock_free<void*>::value == 2 &&
           details::static_is_lock_free<typename details::thread_id_converter<
             details::thread_id_t>::thread_id_numeric_size_t>::value == 2;
  }

private:
  friend struct ExplicitProducer;
  struct ImplicitProducer;
  friend struct ImplicitProducer;
  friend class ConcurrentQueueTests;

  enum AllocationMode { CanAlloc, CannotAlloc };

  ///////////////////////////////
  // Queue methods
  ///////////////////////////////

  template <AllocationMode canAlloc, typename U>
  inline bool inner_enqueue(U&& element) {
    auto producer = get_or_add_implicit_producer();
    return producer == nullptr
             ? false
             : producer->ConcurrentQueue::ImplicitProducer::template enqueue<
                 canAlloc>(static_cast<U&&>(element));
  }

  template <AllocationMode canAlloc, typename It>
  inline bool inner_enqueue_bulk(It itemFirst, size_t count) {
    auto producer = get_or_add_implicit_producer();
    return producer == nullptr
             ? false
             : producer->ConcurrentQueue::ImplicitProducer::
                 template enqueue_bulk<canAlloc>(itemFirst, count);
  }

  ///////////////////////////
  // Free list
  ///////////////////////////

  template <typename N> struct FreeListNode {
    FreeListNode() : freeListRefs(0), freeListNext(nullptr) {}

    std::atomic<std::uint32_t> freeListRefs;
    std::atomic<N*> freeListNext;
  };

  // A simple CAS-based lock-free free list. Not the fastest thing in the world
  // under heavy contention, but simple and correct (assuming nodes are never
  // freed until after the free list is destroyed), and fairly speedy under low
  // contention.
  template <typename N> // N must inherit FreeListNode or have the same fields
                        // (and initialization of them)
  struct FreeList {
    FreeList() noexcept : freeListHead(nullptr) {}
    FreeList(FreeList&& other) noexcept
        : freeListHead(other.freeListHead.load(std::memory_order_relaxed)) {
      other.freeListHead.store(nullptr, std::memory_order_relaxed);
    }

    FreeList(FreeList const&) = delete;
    FreeList& operator=(FreeList const&) = delete;

    inline void add(N* node) {
#ifdef MCDBGQ_NOLOCKFREE_FREELIST
      debug::DebugLock lock(mutex);
#endif
      // We know that the should-be-on-freelist bit is 0 at this point, so it's
      // safe to set it using a fetch_add
      if (node->freeListRefs.fetch_add(
            SHOULD_BE_ON_FREELIST, std::memory_order_acq_rel
          ) == 0) {
        // Oh look! We were the last ones referencing this node, and we know
        // we want to add it to the free list, so let's do it!
        add_knowing_refcount_is_zero(node);
      }
    }

    inline N* try_get() {
#ifdef MCDBGQ_NOLOCKFREE_FREELIST
      debug::DebugLock lock(mutex);
#endif
      auto head = freeListHead.load(std::memory_order_acquire);
      while (head != nullptr) {
        auto prevHead = head;
        auto refs = head->freeListRefs.load(std::memory_order_relaxed);
        if ((refs & REFS_MASK) == 0 ||
            !head->freeListRefs.compare_exchange_strong(
              refs, refs + 1, std::memory_order_acquire
            )) {
          head = freeListHead.load(std::memory_order_acquire);
          continue;
        }

        // Good, reference count has been incremented (it wasn't at zero), which
        // means we can read the next and not worry about it changing between
        // now and the time we do the CAS
        auto next = head->freeListNext.load(std::memory_order_relaxed);
        if (freeListHead.compare_exchange_strong(
              head, next, std::memory_order_acquire, std::memory_order_relaxed
            )) {
          // Yay, got the node. This means it was on the list, which means
          // shouldBeOnFreeList must be false no matter the refcount (because
          // nobody else knows it's been taken off yet, it can't have been put
          // back on).
          assert(
            (head->freeListRefs.load(std::memory_order_relaxed) &
             SHOULD_BE_ON_FREELIST) == 0
          );

          // Decrease refcount twice, once for our ref, and once for the list's
          // ref
          head->freeListRefs.fetch_sub(2, std::memory_order_release);
          return head;
        }

        // OK, the head must have changed on us, but we still need to decrease
        // the refcount we increased. Note that we don't need to release any
        // memory effects, but we do need to ensure that the reference count
        // decrement happens-after the CAS on the head.
        refs = prevHead->freeListRefs.fetch_sub(1, std::memory_order_acq_rel);
        if (refs == SHOULD_BE_ON_FREELIST + 1) {
          add_knowing_refcount_is_zero(prevHead);
        }
      }

      return nullptr;
    }

    // Useful for traversing the list when there's no contention (e.g. to
    // destroy remaining nodes)
    N* head_unsafe() const {
      return freeListHead.load(std::memory_order_relaxed);
    }

  private:
    inline void add_knowing_refcount_is_zero(N* node) {
      // Since the refcount is zero, and nobody can increase it once it's zero
      // (except us, and we run only one copy of this method per node at a time,
      // i.e. the single thread case), then we know we can safely change the
      // next pointer of the node; however, once the refcount is back above
      // zero, then other threads could increase it (happens under heavy
      // contention, when the refcount goes to zero in between a load and a
      // refcount increment of a node in try_get, then back up to something
      // non-zero, then the refcount increment is done by the other thread) --
      // so, if the CAS to add the node to the actual list fails, decrease the
      // refcount and leave the add operation to the next thread who puts the
      // refcount back at zero (which could be us, hence the loop).
      auto head = freeListHead.load(std::memory_order_relaxed);
      while (true) {
        node->freeListNext.store(head, std::memory_order_relaxed);
        node->freeListRefs.store(1, std::memory_order_release);
        if (!freeListHead.compare_exchange_strong(
              head, node, std::memory_order_release, std::memory_order_relaxed
            )) {
          // Hmm, the add failed, but we can only try again when the refcount
          // goes back to zero
          if (node->freeListRefs.fetch_add(
                SHOULD_BE_ON_FREELIST - 1, std::memory_order_acq_rel
              ) == 1) {
            continue;
          }
        }
        return;
      }
    }

  private:
    // Implemented like a stack, but where node order doesn't matter (nodes are
    // inserted out of order under contention)
    std::atomic<N*> freeListHead;

    static const std::uint32_t REFS_MASK = 0x7FFFFFFF;
    static const std::uint32_t SHOULD_BE_ON_FREELIST = 0x80000000;

#ifdef MCDBGQ_NOLOCKFREE_FREELIST
    debug::DebugMutex mutex;
#endif
  };

  ///////////////////////////
  // Block
  ///////////////////////////

  enum InnerQueueContext { implicit_context = 0, explicit_context = 1 };

  struct Block {
    Block()
        : next(nullptr), elementsCompletelyDequeued(0), freeListRefs(0),
          freeListNext(nullptr), dynamicallyAllocated(true) {
      for (size_t i = 0; i < BLOCK_EMPTY_ARRAY_SIZE; ++i) {
        emptyFlags[i] = BLOCK_EMPTY_MASK;
      }
#ifdef MCDBGQ_TRACKMEM
      owner = nullptr;
#endif
    }

    template <InnerQueueContext context> inline bool is_empty() const {
      if constexpr (context == explicit_context) {
        for (size_t i = 0; i < BLOCK_EMPTY_ARRAY_SIZE; ++i) {
          if (emptyFlags[i].load(std::memory_order_relaxed) !=
              BLOCK_EMPTY_MASK) {
            return false;
          }
        }

        // Aha, empty; make sure we have all other memory effects that happened
        // before the empty flags were set
        std::atomic_thread_fence(std::memory_order_acquire);
        return true;
      } else {
        // Check counter
        if (elementsCompletelyDequeued.load(std::memory_order_relaxed) ==
            PRODUCER_BLOCK_SIZE) {
          std::atomic_thread_fence(std::memory_order_acquire);
          return true;
        }
        assert(
          elementsCompletelyDequeued.load(std::memory_order_relaxed) <=
          PRODUCER_BLOCK_SIZE
        );
        return false;
      }
    }

    // Returns true if the block is now empty (does not apply in explicit
    // context)
    template <InnerQueueContext context>
    inline bool set_empty([[maybe_unused]] index_t i) {
      if constexpr (context == explicit_context) {
        auto rawIndex =
          static_cast<size_t>(i & static_cast<index_t>(BLOCK_MASK));
        size_t arrIndex, bitIndex;
        arrIndex = rawIndex / BLOCK_EMPTY_ELEM_SIZE;
        bitIndex = rawIndex & (BLOCK_EMPTY_ELEM_SIZE - 1);
        size_t bit = TMC_ONE_BIT << bitIndex;
        // Set flag
        assert(
          (emptyFlags[arrIndex].load(std::memory_order_relaxed) & bit) == 0
        );
        emptyFlags[arrIndex].fetch_or(bit, std::memory_order_release);
        return false;
      } else {
        // Increment counter
        auto prevVal =
          elementsCompletelyDequeued.fetch_add(1, std::memory_order_acq_rel);
        assert(prevVal < PRODUCER_BLOCK_SIZE);
        return prevVal == BLOCK_MASK;
      }
    }

    // Sets multiple contiguous item statuses to 'empty' (assumes no wrapping
    // and count > 0). Returns true if the block is now empty (does not apply in
    // explicit context).
    // Unused and incomplete since the transition to empty bitmask
    template <InnerQueueContext context>
    inline bool set_many_empty([[maybe_unused]] index_t i, size_t count) {
      if constexpr (context == explicit_context) {
        // Set flags
        std::atomic_thread_fence(std::memory_order_release);
        // TODO implement this with interleaving
        i = static_cast<size_t>(i & static_cast<index_t>(BLOCK_MASK));
        auto rawIndexStart =
          static_cast<size_t>(i & static_cast<index_t>(BLOCK_MASK));
        auto arrIndexStart = rawIndexStart / BLOCK_EMPTY_ELEM_SIZE;
        auto bitIndexStart = rawIndexStart & (BLOCK_EMPTY_ELEM_SIZE - 1);

        auto rawIndexEnd = i + count; // this shouldn't wrap
        auto arrIndexEnd = rawIndexEnd / BLOCK_EMPTY_ELEM_SIZE;
        auto bitIndexEnd = rawIndexEnd & (BLOCK_EMPTY_ELEM_SIZE - 1);

        // Begin
        auto arrIndex = arrIndexStart;
        auto bitIndex = bitIndexStart;
        if (arrIndex < arrIndexEnd) {
          size_t bits =
            -(TMC_ONE_BIT << bitIndex); // set all bits from bitIndex and higher
          assert(
            (emptyFlags[arrIndex].load(std::memory_order_relaxed) & bits) == 0
          );
          emptyFlags[arrIndex].fetch_or(bits, std::memory_order_relaxed);
          count -= (BLOCK_EMPTY_ELEM_SIZE - bitIndex);
          arrIndex++;
        }

        // Middle
        while (count > TMC_PLATFORM_BITS) {
          assert(count % TMC_PLATFORM_BITS == 0);
          assert(emptyFlags[arrIndex].load(std::memory_order_relaxed) == 0);
          emptyFlags[arrIndex].fetch_or(
            TMC_ALL_ONES, std::memory_order_relaxed
          );
          count -= TMC_PLATFORM_BITS;
          arrIndex++;
        }

        // End
        assert(arrIndex == arrIndexEnd);
        size_t bits = ((TMC_ONE_BIT << count) - 1) << (bitIndexEnd + 1 - count);
        assert(
          (emptyFlags[arrIndex].load(std::memory_order_relaxed) & bits) == 0
        );
        emptyFlags[arrIndex].fetch_or(bits, std::memory_order_relaxed);
        return false;
      } else {
        // Increment counter
        auto prevVal = elementsCompletelyDequeued.fetch_add(
          count, std::memory_order_acq_rel
        );
        assert(prevVal + count <= PRODUCER_BLOCK_SIZE);
        return prevVal + count == PRODUCER_BLOCK_SIZE;
      }
    }

    template <InnerQueueContext context> inline void set_all_empty() {
      if constexpr (context == explicit_context) {
        for (size_t i = 0; i < BLOCK_EMPTY_ARRAY_SIZE; ++i) {
          emptyFlags[i].store(BLOCK_EMPTY_MASK, std::memory_order_relaxed);
        }
      } else {
        // Reset counter
        elementsCompletelyDequeued.store(
          PRODUCER_BLOCK_SIZE, std::memory_order_relaxed
        );
      }
    }

    template <InnerQueueContext context> inline void reset_empty() {
      if constexpr (context == explicit_context) {
        for (size_t i = 0; i < BLOCK_EMPTY_ARRAY_SIZE; ++i) {
          emptyFlags[i].store(0, std::memory_order_relaxed);
        }
      } else {
        // Reset counter
        elementsCompletelyDequeued.store(0, std::memory_order_relaxed);
      }
    }

    inline T* operator[](index_t idx) MOODYCAMEL_NOEXCEPT {
      size_t raw_off =
        static_cast<size_t>(idx & static_cast<index_t>(BLOCK_MASK));
      if constexpr (ELEM_INTERLEAVING > 1) {
        size_t off_unaffected = raw_off & ~ELEM_INTERLEAVING_ALL_MASK;
        size_t il_bits = raw_off & ELEM_INTERLEAVING_ALL_MASK;
        size_t low_bits = il_bits & ELEM_INTERLEAVING_MASK;
        size_t high_bits = il_bits / ELEM_INTERLEAVING;
        size_t low_bits_in_place = low_bits * ELEMS_PER_CACHELINE;
        size_t off = off_unaffected | high_bits | low_bits_in_place;
        return static_cast<T*>(static_cast<void*>(elements)) + off;
      } else {
        return static_cast<T*>(static_cast<void*>(elements)) + raw_off;
      }
    }

    inline T const* operator[](index_t idx) const MOODYCAMEL_NOEXCEPT {
      size_t raw_off =
        static_cast<size_t>(idx & static_cast<index_t>(BLOCK_MASK));

      if constexpr (ELEM_INTERLEAVING > 1) {
        size_t off_unaffected = raw_off & ~ELEM_INTERLEAVING_ALL_MASK;
        size_t il_bits = raw_off & ELEM_INTERLEAVING_ALL_MASK;
        size_t low_bits = il_bits & ELEM_INTERLEAVING_MASK;
        size_t high_bits = il_bits / ELEM_INTERLEAVING;
        size_t low_bits_in_place = low_bits * ELEMS_PER_CACHELINE;
        size_t off = off_unaffected | high_bits | low_bits_in_place;
        return static_cast<T const*>(static_cast<void const*>(elements)) + off;
      } else {
        return static_cast<T const*>(static_cast<void const*>(elements)) +
               raw_off;
      }
    }

  private:
    static_assert(
      std::alignment_of<T>::value <= sizeof(T),
      "The queue does not support types with an alignment greater "
      "than their size at this time"
    );
    MOODYCAMEL_ALIGNED_TYPE_LIKE(char[sizeof(T) * PRODUCER_BLOCK_SIZE], T)
    elements;

  public:
    alignas(64) std::atomic<size_t> emptyFlags[BLOCK_EMPTY_ARRAY_SIZE];
    Block* next;
    std::atomic<size_t> elementsCompletelyDequeued;

  public:
    std::atomic<std::uint32_t> freeListRefs;
    std::atomic<Block*> freeListNext;
    bool dynamicallyAllocated; // Perhaps a better name for this would be
                               // 'isNotPartOfInitialBlockPool'

#ifdef MCDBGQ_TRACKMEM
    void* owner;
#endif
  };
  static_assert(
    std::alignment_of<Block>::value >= std::alignment_of<T>::value,
    "Internal error: Blocks must be at least as aligned as the "
    "type they are wrapping"
  );

#ifdef MCDBGQ_TRACKMEM
public:
  struct MemStats;

private:
#endif

  ///////////////////////////
  // Producer base
  ///////////////////////////

  struct ProducerBase : public details::ConcurrentQueueProducerTypelessBase {
    ProducerBase(ConcurrentQueue* parent_, bool isExplicit_)
        : tailIndex(0), headIndex(0), dequeueOptimisticCount(0),
          dequeueOvercommit(0), tailBlock(nullptr), isExplicit(isExplicit_),
          parent(parent_) {}

    virtual ~ProducerBase() {}

    template <typename U> inline bool dequeue(U& element) {
      if (isExplicit) {
        return static_cast<ExplicitProducer*>(this)->dequeue(element);
      } else {
        return static_cast<ImplicitProducer*>(this)->dequeue(element);
      }
    }

    template <typename It>
    inline size_t dequeue_bulk(It& itemFirst, size_t max) {
      if (isExplicit) {
        return static_cast<ExplicitProducer*>(this)->dequeue_bulk(
          itemFirst, max
        );
      } else {
        return static_cast<ImplicitProducer*>(this)->dequeue_bulk(
          itemFirst, max
        );
      }
    }

    inline ProducerBase* next_prod() const {
      return static_cast<ProducerBase*>(next);
    }

    inline size_t size_approx() const {
      auto tail = tailIndex.load(std::memory_order_relaxed);
      auto head = headIndex.load(std::memory_order_relaxed);
      return details::circular_less_than(head, tail)
               ? static_cast<size_t>(tail - head)
               : 0;
    }

    inline index_t getTail() const {
      return tailIndex.load(std::memory_order_relaxed);
    }

  protected:
    std::atomic<index_t> tailIndex; // Where to enqueue to next
    std::atomic<index_t> headIndex; // Where to dequeue from next

    std::atomic<index_t> dequeueOptimisticCount;
    std::atomic<index_t> dequeueOvercommit;

    Block* tailBlock;

  public:
    bool isExplicit;
    ConcurrentQueue* parent;

  protected:
#ifdef MCDBGQ_TRACKMEM
    friend struct MemStats;
#endif
  };

  ///////////////////////////
  // Explicit queue
  ///////////////////////////
public:
  struct ExplicitProducer : public ProducerBase {
    explicit ExplicitProducer()
        : ProducerBase(nullptr, true), blockIndex(nullptr),
          pr_blockIndexSlotsUsed(0),
          pr_blockIndexSize(EXPLICIT_INITIAL_INDEX_SIZE >> 1),
          pr_blockIndexFront(0), pr_blockIndexFrontMax(0),
          pr_blockIndexEntries(nullptr), pr_blockIndexRaw(nullptr) {}

    void init(ConcurrentQueue* parent_) {
      ProducerBase::parent = parent_;
      size_t poolBasedIndexSize =
        details::ceil_to_pow_2(parent_->initialBlockPoolSize) >> 1;
      if (poolBasedIndexSize > pr_blockIndexSize) {
        pr_blockIndexSize = poolBasedIndexSize;
      }

      new_block_index(0); // This creates an index with double the number of
                          // current entries, i.e. EXPLICIT_INITIAL_INDEX_SIZE
    }

    ~ExplicitProducer() override {
      // Destruct any elements not yet dequeued.
      // Since we're in the destructor, we can assume all elements
      // are either completely dequeued or completely not (no halfways).
      if (this->tailBlock != nullptr) { // Note this means there must be a block
                                        // index too
        // First find the block that's partially dequeued, if any
        Block* halfDequeuedBlock = nullptr;
        if ((this->headIndex.load(std::memory_order_relaxed) &
             static_cast<index_t>(BLOCK_MASK)) != 0) {
          // The head's not on a block boundary, meaning a block somewhere is
          // partially dequeued (or the head block is the tail block and was
          // fully dequeued, but the head/tail are still not on a boundary)
          size_t i = (pr_blockIndexFrontMax - pr_blockIndexSlotsUsed) &
                     (pr_blockIndexSize - 1);
          while (details::circular_less_than<index_t>(
            pr_blockIndexEntries[i].base + PRODUCER_BLOCK_SIZE,
            this->headIndex.load(std::memory_order_relaxed)
          )) {
            i = (i + 1) & (pr_blockIndexSize - 1);
          }
          assert(details::circular_less_than<index_t>(
            pr_blockIndexEntries[i].base,
            this->headIndex.load(std::memory_order_relaxed)
          ));
          halfDequeuedBlock = pr_blockIndexEntries[i].block;
        }

        // Start at the head block (note the first line in the loop gives us the
        // head from the tail on the first iteration)
        auto block = this->tailBlock;
        do {
          block = block->next;
          if (block
                ->ConcurrentQueue::Block::template is_empty<explicit_context>(
                )) {
            continue;
          }

          size_t i = 0; // Offset into block
          if (block == halfDequeuedBlock) {
            i = static_cast<size_t>(
              this->headIndex.load(std::memory_order_relaxed) &
              static_cast<index_t>(BLOCK_MASK)
            );
          }

          // Walk through all the items in the block; if this is the tail block,
          // we need to stop when we reach the tail index
          auto lastValidIndex =
            (this->tailIndex.load(std::memory_order_relaxed) &
             static_cast<index_t>(BLOCK_MASK)) == 0
              ? PRODUCER_BLOCK_SIZE
              : static_cast<size_t>(
                  this->tailIndex.load(std::memory_order_relaxed) &
                  static_cast<index_t>(BLOCK_MASK)
                );
          while (i != PRODUCER_BLOCK_SIZE &&
                 (block != this->tailBlock || i != lastValidIndex)) {
            (*block)[i]->~T();
            ++i;
          }
        } while (block != this->tailBlock);
      }

      // Destroy all blocks that we own
      if (this->tailBlock != nullptr) {
        auto block = this->tailBlock;
        do {
          auto nextBlock = block->next;
          this->parent->add_block_to_free_list(block);
          block = nextBlock;
        } while (block != this->tailBlock);
      }

      // Destroy the block indices
      auto header = static_cast<BlockIndexHeader*>(pr_blockIndexRaw);
      while (header != nullptr) {
        auto prev = static_cast<BlockIndexHeader*>(header->prev);
        header->~BlockIndexHeader();
        (Traits::free)(header);
        header = prev;
      }
    }

    template <AllocationMode allocMode, typename U>
    inline bool enqueue(U&& element) {
      index_t currentTailIndex =
        this->tailIndex.load(std::memory_order_relaxed);
      index_t newTailIndex = 1 + currentTailIndex;
      if ((currentTailIndex & static_cast<index_t>(BLOCK_MASK)) == 0) {
        // We reached the end of a block, start a new one
        auto startBlock = this->tailBlock;
        auto originalBlockIndexSlotsUsed = pr_blockIndexSlotsUsed;
        if (this->tailBlock != nullptr &&
            this->tailBlock->next
              ->ConcurrentQueue::Block::template is_empty<explicit_context>()) {
          // We can re-use the block ahead of us, it's empty!
          this->tailBlock = this->tailBlock->next;
          this->tailBlock
            ->ConcurrentQueue::Block::template reset_empty<explicit_context>();

          // We'll put the block on the block index (guaranteed to be room since
          // we're conceptually removing the last block from it first -- except
          // instead of removing then adding, we can just overwrite). Note that
          // there must be a valid block index here, since even if allocation
          // failed in the ctor, it would have been re-attempted when adding the
          // first block to the queue; since there is such a block, a block
          // index must have been successfully allocated.
        } else {
          // Whatever head value we see here is >= the last value we saw here
          // (relatively), and <= its current value. Since we have the most
          // recent tail, the head must be
          // <= to it.
          auto head = this->headIndex.load(std::memory_order_relaxed);
          assert(!details::circular_less_than<index_t>(currentTailIndex, head));
          if (!details::circular_less_than<index_t>(
                head, currentTailIndex + PRODUCER_BLOCK_SIZE
              ) ||
              (MAX_SUBQUEUE_SIZE != details::const_numeric_max<size_t>::value &&
               (MAX_SUBQUEUE_SIZE == 0 ||
                MAX_SUBQUEUE_SIZE - PRODUCER_BLOCK_SIZE <
                  currentTailIndex - head))) {
            // We can't enqueue in another block because there's not enough
            // leeway -- the tail could surpass the head by the time the block
            // fills up! (Or we'll exceed the size limit, if the second part of
            // the condition was true.)
            return false;
          }
          // We're going to need a new block; check that the block index has
          // room
          if (pr_blockIndexRaw == nullptr ||
              pr_blockIndexSlotsUsed == pr_blockIndexSize) {
            // Hmm, the circular block index is already full -- we'll need
            // to allocate a new index. Note pr_blockIndexRaw can only be
            // nullptr if the initial allocation failed in the constructor.

            if constexpr (allocMode == CannotAlloc) {
              return false;
            } else if (!new_block_index(pr_blockIndexSlotsUsed)) {
              return false;
            }
          }

          // Insert a new block in the circular linked list
          auto newBlock =
            this->parent
              ->ConcurrentQueue::template requisition_block<allocMode>();
          if (newBlock == nullptr) {
            return false;
          }
#ifdef MCDBGQ_TRACKMEM
          newBlock->owner = this;
#endif
          newBlock
            ->ConcurrentQueue::Block::template reset_empty<explicit_context>();
          if (this->tailBlock == nullptr) {
            newBlock->next = newBlock;
          } else {
            newBlock->next = this->tailBlock->next;
            this->tailBlock->next = newBlock;
          }
          this->tailBlock = newBlock;
          ++pr_blockIndexSlotsUsed;
        }

        if constexpr (!MOODYCAMEL_NOEXCEPT_CTOR(new (static_cast<T*>(nullptr)
                      ) T(static_cast<U&&>(element)))) {
          // The constructor may throw. We want the element not to appear in the
          // queue in that case (without corrupting the queue):
          MOODYCAMEL_TRY {
            new ((*this->tailBlock)[currentTailIndex])
              T(static_cast<U&&>(element));
          }
          MOODYCAMEL_CATCH(...) {
            // Revert change to the current block, but leave the new block
            // available for next time
            pr_blockIndexSlotsUsed = originalBlockIndexSlotsUsed;
            this->tailBlock =
              startBlock == nullptr ? this->tailBlock : startBlock;
            MOODYCAMEL_RETHROW;
          }
        } else {
          (void)startBlock;
          (void)originalBlockIndexSlotsUsed;
        }

        auto localBlockIndex = blockIndex.load(std::memory_order_relaxed);
        size_t nextFront = (pr_blockIndexFront + 1) & (pr_blockIndexSize - 1);
        if (pr_blockIndexFrontMax == pr_blockIndexFront) {
          pr_blockIndexFrontMax = nextFront;
          // Add block to block index. We skip this if not at FrontMax (because
          // we popped this off with dequeue_lifo), because this block already
          // exists in the block index with the same base value, and it causes a
          // TSan warning to write the same data again. However this is a false
          // positive, and the below lines could be run unconditionally since
          // the data being overwritten is identical.
          auto& entry = localBlockIndex->entries[pr_blockIndexFront];
          entry.base = currentTailIndex;
          entry.block = this->tailBlock;
        }
        localBlockIndex->front.store(
          pr_blockIndexFront, std::memory_order_release
        );
        pr_blockIndexFront = nextFront;

        if constexpr (!MOODYCAMEL_NOEXCEPT_CTOR(new (static_cast<T*>(nullptr)
                      ) T(static_cast<U&&>(element)))) {
          this->tailIndex.store(newTailIndex, std::memory_order_release);
          return true;
        }
      }

      // Enqueue
      new ((*this->tailBlock)[currentTailIndex]) T(static_cast<U&&>(element));

      this->tailIndex.store(newTailIndex, std::memory_order_release);
      return true;
    }

    // TZCNT MODIFIED: Pops from tail (like a LIFO stack) instead of from head
    // (like a FIFO queue) of this thread's locally owned producer. This must
    // not be called on any other thread's producer.
    //
    // This is always called in exactly one place. TMC_FORCE_INLINE empirically
    // determined to improve perf.
    template <typename U> TMC_FORCE_INLINE bool dequeue_lifo(U& element) {
      // Since this is our own queue, just be optimistic and go for it
      // without checking if there are actually any elements first.
      auto prevIndex = this->tailIndex.fetch_sub(1, std::memory_order_seq_cst);
      // StoreLoad barrier required to see other readers
      // Overcommit must be loaded before optimistic for correct operation
      auto myOvercommit =
        this->dequeueOvercommit.load(std::memory_order_seq_cst);
      auto myDequeueCount =
        this->dequeueOptimisticCount.load(std::memory_order_seq_cst);
      if (!details::circular_less_than<index_t>(
            myDequeueCount - myOvercommit, prevIndex
          )) {
        // Wasn't anything to dequeue after all; make the effective dequeue
        // count eventually consistent
        this->tailIndex.store(prevIndex, std::memory_order_release);
        return false;
      }

      auto index = prevIndex - 1;
      // Relaxed loads are OK since these values are only written by this thread
      auto localBlockIndex = blockIndex.load(std::memory_order_relaxed);
      auto currentTailBlockIndex =
        localBlockIndex->front.load(std::memory_order_relaxed);

      assert((
        localBlockIndex->entries[currentTailBlockIndex].block == this->tailBlock
      ));
      Block* block = this->tailBlock;
      if ((index & static_cast<index_t>(BLOCK_MASK)) == 0) {
        // tailIndex was pointing at the first (empty) element of a new block.
        // index now points at the last element of the previous block.
        // as we dequeue from index, we need to back up tailIndex so that it is
        // also at the end of the previous block.
        assert(currentTailBlockIndex != TMC_ALL_ONES);
        assert((localBlockIndex->entries[currentTailBlockIndex].base == index));

        // When backing up, we can underflow the array:
        // (index wraps from 0 to pr_blockIndexSize - 1)
        // or underflow the used slots:
        // (index wraps from pr_blockIndexFrontMax - pr_blockIndexSlotsUsed
        //   to pr_blockIndexFrontMax - 1)
        if (pr_blockIndexSlotsUsed > 1) {
          size_t underflowFrom;
          if (currentTailBlockIndex ==
              ((pr_blockIndexFrontMax - pr_blockIndexSlotsUsed) &
               (pr_blockIndexSize - 1))) {
            underflowFrom = pr_blockIndexFrontMax;
          } else {
            underflowFrom = currentTailBlockIndex;
          }
          auto blockBeforeTailBlockIndex =
            (underflowFrom - 1) & (pr_blockIndexSize - 1);
          Block* blockBeforeTailBlock =
            localBlockIndex->entries[blockBeforeTailBlockIndex].block;
          localBlockIndex->front.store(
            blockBeforeTailBlockIndex, std::memory_order_release
          );
          pr_blockIndexFront = currentTailBlockIndex;

          assert((blockBeforeTailBlock->next == this->tailBlock));
          this->tailBlock = blockBeforeTailBlock;
        }
        block->ConcurrentQueue::Block::template set_all_empty<explicit_context>(
        );
      }

      // Dequeue
      T& el = *((*block)[index]);
      if constexpr (!MOODYCAMEL_NOEXCEPT_ASSIGN(
                      element = static_cast<T&&>(el)
                    )) {
        struct Guard {
          Block* block;
          index_t index;

          ~Guard() {
            (*block)[index]->~T();
            // set_empty() not needed here - that happens in the underflow block
          }
        } guard = {block, index};
        element = static_cast<T&&>(el); // NOLINT
      } else {
        element = static_cast<T&&>(el); // NOLINT
        el.~T();                        // NOLINT
        // set_empty() not needed here - that happens in the underflow block
      }

      return true;
    }

    template <typename U> bool dequeue(U& element) {
      auto tail = this->tailIndex.load(std::memory_order_relaxed);
      auto overcommit = this->dequeueOvercommit.load(std::memory_order_relaxed);
      if (!details::circular_less_than<index_t>(
            this->dequeueOptimisticCount.load(std::memory_order_relaxed) -
              overcommit,
            tail
          )) {
        return false;
      }
      // Might be something to dequeue, let's give it a try

      // Note that this if is purely for performance purposes in the common
      // case when the queue is empty and the values are eventually consistent
      // -- we may enter here spuriously.

      // Note that whatever the values of overcommit and tail are, they are
      // not going to change (unless we change them) and must be the same
      // value at this point (inside the if) as when the if condition was
      // evaluated.

      // We insert an acquire fence here to synchronize-with the release upon
      // incrementing dequeueOvercommit below. This ensures that whatever the
      // value we got loaded into overcommit, the load of dequeueOptisticCount
      // in the fetch_add below will result in a value at least as recent as
      // that (and therefore at least as large). Note that I believe a
      // compiler (signal) fence here would be sufficient due to the nature of
      // fetch_add (all read-modify-write operations are guaranteed to work on
      // the latest value in the modification order), but unfortunately that
      // can't be shown to be correct using only the C++11 standard. See
      // http://stackoverflow.com/questions/18223161/what-are-the-c11-memory-ordering-guarantees-in-this-corner-case
      std::atomic_thread_fence(std::memory_order_acquire);

      // Increment optimistic counter, then check if it went over the boundary
      // TZCNT MODIFIED: From relaxed to acq_rel. This is required to
      // synchronize with dequeue_lifo (on explicit producers only - implicit
      // producers don't have dequeue_lifo).
      auto myDequeueCount =
        this->dequeueOptimisticCount.fetch_add(1, std::memory_order_acq_rel);

      // Note that since dequeueOvercommit must be <= dequeueOptimisticCount
      // (because dequeueOvercommit is only ever incremented after
      // dequeueOptimisticCount -- this is enforced in the `else` block
      // below), and since we now have a version of dequeueOptimisticCount
      // that is at least as recent as overcommit (due to the release upon
      // incrementing dequeueOvercommit and the acquire above that
      // synchronizes with it), overcommit <= myDequeueCount. However, we
      // can't assert this since both dequeueOptimisticCount and
      // dequeueOvercommit may (independently) overflow; in such a case,
      // though, the logic still holds since the difference between the two is
      // maintained.

      // Note that we reload tail here in case it changed; it will be the same
      // value as before or greater, since this load is sequenced after
      // (happens after) the earlier load above. This is supported by
      // read-read coherency (as defined in the standard), explained here:
      // http://en.cppreference.com/w/cpp/atomic/memory_order
      tail = this->tailIndex.load(std::memory_order_acquire);
      if (!details::circular_less_than<index_t>(
            myDequeueCount - overcommit, tail
          )) {
        // Wasn't anything to dequeue after all; make the effective dequeue
        // count eventually consistent
        this->dequeueOvercommit.fetch_add(1, std::memory_order_release);
        return false;
      }

      // Guaranteed to be at least one element to dequeue!
      // Get the index. Note that since there's guaranteed to be at least
      // one element, this will never exceed tail. We need to do an
      // acquire-release fence here since it's possible that whatever
      // condition got us to this point was for an earlier enqueued element
      // (that we already see the memory effects for), but that by the time
      // we increment somebody else has incremented it, and we need to see
      // the memory effects for *that* element, which is in such a case is
      // necessarily visible on the thread that incremented it in the first
      // place with the more current condition (they must have acquired a
      // tail that is at least as recent).
      auto index = this->headIndex.fetch_add(1, std::memory_order_acq_rel);

      // Determine which block the element is in
      auto localBlockIndex = blockIndex.load(std::memory_order_acquire);
      auto localBlockIndexHead =
        localBlockIndex->front.load(std::memory_order_acquire);

      // We need to be careful here about subtracting and dividing because
      // of index wrap-around. When an index wraps, we need to preserve the
      // sign of the offset when dividing it by the block size (in order to
      // get a correct signed block count offset in all cases):
      auto headBase = localBlockIndex->entries[localBlockIndexHead].base;
      auto blockBaseIndex = index & ~static_cast<index_t>(BLOCK_MASK);
      auto offset = static_cast<size_t>(
        static_cast<typename std::make_signed<index_t>::type>(
          blockBaseIndex - headBase
        ) /
        static_cast<typename std::make_signed<index_t>::type>(
          PRODUCER_BLOCK_SIZE
        )
      );
      auto block =
        localBlockIndex
          ->entries
            [(localBlockIndexHead + offset) & (localBlockIndex->size - 1)]
          .block;

      // Dequeue
      T& el = *((*block)[index]);
      if constexpr (!MOODYCAMEL_NOEXCEPT_ASSIGN(
                      element = static_cast<T&&>(el)
                    )) {
        // Make sure the element is still fully dequeued and destroyed even
        // if the assignment throws
        struct Guard {
          Block* block;
          index_t index;

          ~Guard() {
            (*block)[index]->~T();
            block->ConcurrentQueue::Block::template set_empty<explicit_context>(
              index
            );
          }
        } guard = {block, index};

        element = static_cast<T&&>(el); // NOLINT
      } else {
        element = static_cast<T&&>(el); // NOLINT
        el.~T();                        // NOLINT
        block->ConcurrentQueue::Block::template set_empty<explicit_context>(
          index
        );
      }

      return true;
    }

    template <AllocationMode allocMode, typename It>
    bool MOODYCAMEL_NO_TSAN enqueue_bulk(It itemFirst, size_t count) {
      static constexpr bool HasMoveConstructor = std::is_constructible_v<
        T, std::add_rvalue_reference_t<std::iter_value_t<It>>>;
      static constexpr bool HasNoexceptMoveConstructor =
        std::is_nothrow_constructible_v<
          T, std::add_rvalue_reference_t<std::iter_value_t<It>>>;

      static constexpr bool HasCopyConstructor = std::is_constructible_v<
        T, std::add_lvalue_reference_t<std::iter_value_t<It>>>;
      static constexpr bool HasNoexceptCopyConstructor =
        std::is_nothrow_constructible_v<
          T, std::add_lvalue_reference_t<std::iter_value_t<It>>>;

      // Prefer constructors in this order:
      // 1. Noexcept move constructor
      // 2. Noexcept copy constructor
      // 3. Copy constructor
      // 4. Move constructor

      // For 3. and 4., prefer copy constructor even if move constructor is
      // available because we may have to revert if there's an
      // exception.
      static constexpr bool UseMoveConstructor =
        HasNoexceptMoveConstructor || !HasCopyConstructor;

      static constexpr bool IsConstructorNoexcept =
        HasNoexceptMoveConstructor || HasNoexceptCopyConstructor;

      // First, we need to make sure we have enough room to enqueue all of the
      // elements; this means pre-allocating blocks and putting them in the
      // block index (but only if all the allocations succeeded).
      index_t startTailIndex = this->tailIndex.load(std::memory_order_relaxed);
      auto startBlock = this->tailBlock;
      auto originalBlockIndexFront = pr_blockIndexFront;
      auto originalBlockIndexFrontMax = pr_blockIndexFrontMax;
      auto originalBlockIndexSlotsUsed = pr_blockIndexSlotsUsed;

      Block* firstAllocatedBlock = nullptr;

      // Figure out how many blocks we'll need to allocate, and do so
      size_t blockBaseDiff =
        ((startTailIndex + count - 1) & ~static_cast<index_t>(BLOCK_MASK)) -
        ((startTailIndex - 1) & ~static_cast<index_t>(BLOCK_MASK));
      index_t currentTailIndex =
        (startTailIndex - 1) & ~static_cast<index_t>(BLOCK_MASK);
      if (blockBaseDiff > 0) {
        auto localBlockIndex = blockIndex.load(std::memory_order_relaxed);
        // Allocate as many blocks as possible from ahead
        while (blockBaseDiff > 0 && this->tailBlock != nullptr &&
               this->tailBlock->next != firstAllocatedBlock &&
               this->tailBlock->next
                 ->ConcurrentQueue::Block::template is_empty<explicit_context>()
        ) {
          blockBaseDiff -= static_cast<index_t>(PRODUCER_BLOCK_SIZE);
          currentTailIndex += static_cast<index_t>(PRODUCER_BLOCK_SIZE);

          this->tailBlock = this->tailBlock->next;
          firstAllocatedBlock = firstAllocatedBlock == nullptr
                                  ? this->tailBlock
                                  : firstAllocatedBlock;

          size_t nextFront = (pr_blockIndexFront + 1) & (pr_blockIndexSize - 1);
          if (pr_blockIndexFrontMax == pr_blockIndexFront) {
            pr_blockIndexFrontMax = nextFront;
            // Add block to block index. We skip this if not at FrontMax
            // (because we popped this off with dequeue_lifo), because this
            // block already exists in the block index with the same base value,
            // and it causes a TSan warning to write the same data again.
            // However this is a false positive, and the below lines could be
            // run unconditionally since the data being overwritten is
            // identical.
            auto& entry = localBlockIndex->entries[pr_blockIndexFront];
            entry.base = currentTailIndex;
            entry.block = this->tailBlock;
          }
          pr_blockIndexFront = nextFront;
        }

        // Now allocate as many blocks as necessary from the block pool
        while (blockBaseDiff > 0) {
          blockBaseDiff -= static_cast<index_t>(PRODUCER_BLOCK_SIZE);
          currentTailIndex += static_cast<index_t>(PRODUCER_BLOCK_SIZE);

          auto head = this->headIndex.load(std::memory_order_relaxed);
          assert(!details::circular_less_than<index_t>(currentTailIndex, head));
          bool full =
            !details::circular_less_than<index_t>(
              head, currentTailIndex + PRODUCER_BLOCK_SIZE
            ) ||
            (MAX_SUBQUEUE_SIZE != details::const_numeric_max<size_t>::value &&
             (MAX_SUBQUEUE_SIZE == 0 ||
              MAX_SUBQUEUE_SIZE - PRODUCER_BLOCK_SIZE < currentTailIndex - head)
            );
          if (pr_blockIndexRaw == nullptr ||
              pr_blockIndexSlotsUsed == pr_blockIndexSize || full) {
            if constexpr (allocMode == CannotAlloc) {
              // Failed to allocate, undo changes (but keep injected blocks)
              pr_blockIndexFront = originalBlockIndexFront;
              pr_blockIndexFrontMax = originalBlockIndexFrontMax;
              pr_blockIndexSlotsUsed = originalBlockIndexSlotsUsed;
              this->tailBlock =
                startBlock == nullptr ? firstAllocatedBlock : startBlock;
              return false;
            } else if (full || !new_block_index(originalBlockIndexSlotsUsed)) {
              // Failed to allocate, undo changes (but keep injected blocks)
              pr_blockIndexFront = originalBlockIndexFront;
              pr_blockIndexFrontMax = originalBlockIndexFrontMax;
              pr_blockIndexSlotsUsed = originalBlockIndexSlotsUsed;
              this->tailBlock =
                startBlock == nullptr ? firstAllocatedBlock : startBlock;
              return false;
            }

            // pr_blockIndexFront is updated inside new_block_index, so we need
            // to update our fallback value too (since we keep the new index
            // even if we later fail)
            originalBlockIndexFront = originalBlockIndexSlotsUsed;
            originalBlockIndexFrontMax = originalBlockIndexSlotsUsed;
          }

          // Insert a new block in the circular linked list
          auto newBlock =
            this->parent
              ->ConcurrentQueue::template requisition_block<allocMode>();
          if (newBlock == nullptr) {
            pr_blockIndexFront = originalBlockIndexFront;
            pr_blockIndexFrontMax = originalBlockIndexFrontMax;
            pr_blockIndexSlotsUsed = originalBlockIndexSlotsUsed;
            this->tailBlock =
              startBlock == nullptr ? firstAllocatedBlock : startBlock;
            return false;
          }

#ifdef MCDBGQ_TRACKMEM
          newBlock->owner = this;
#endif
          newBlock
            ->ConcurrentQueue::Block::template set_all_empty<explicit_context>(
            );
          if (this->tailBlock == nullptr) {
            newBlock->next = newBlock;
          } else {
            newBlock->next = this->tailBlock->next;
            this->tailBlock->next = newBlock;
          }
          this->tailBlock = newBlock;
          firstAllocatedBlock = firstAllocatedBlock == nullptr
                                  ? this->tailBlock
                                  : firstAllocatedBlock;

          ++pr_blockIndexSlotsUsed;

          auto& entry = blockIndex.load(std::memory_order_relaxed)
                          ->entries[pr_blockIndexFront];
          entry.base = currentTailIndex;
          entry.block = this->tailBlock;
          bool frontMatched = pr_blockIndexFrontMax == pr_blockIndexFront;
          pr_blockIndexFront =
            (pr_blockIndexFront + 1) & (pr_blockIndexSize - 1);
          if (frontMatched) {
            pr_blockIndexFrontMax = pr_blockIndexFront;
          }
        }

        // Excellent, all allocations succeeded. Reset each block's emptiness
        // before we fill them up, and publish the new block index front
        auto block = firstAllocatedBlock;
        while (true) {
          block->ConcurrentQueue::Block::template reset_empty<explicit_context>(
          );
          if (block == this->tailBlock) {
            break;
          }
          block = block->next;
        }

        if constexpr (IsConstructorNoexcept) {
          blockIndex.load(std::memory_order_relaxed)
            ->front.store(
              (pr_blockIndexFront - 1) & (pr_blockIndexSize - 1),
              std::memory_order_release
            );
        }
      }

      // Enqueue, one block at a time
      index_t newTailIndex = startTailIndex + static_cast<index_t>(count);
      currentTailIndex = startTailIndex;
      auto endBlock = this->tailBlock;
      this->tailBlock = startBlock;
      assert(
        (startTailIndex & static_cast<index_t>(BLOCK_MASK)) != 0 ||
        firstAllocatedBlock != nullptr || count == 0
      );
      if ((startTailIndex & static_cast<index_t>(BLOCK_MASK)) == 0 &&
          firstAllocatedBlock != nullptr) {
        this->tailBlock = firstAllocatedBlock;
      }
      while (true) {
        index_t stopIndex =
          (currentTailIndex & ~static_cast<index_t>(BLOCK_MASK)) +
          static_cast<index_t>(PRODUCER_BLOCK_SIZE);
        if (details::circular_less_than<index_t>(newTailIndex, stopIndex)) {
          stopIndex = newTailIndex;
        }

        if constexpr (IsConstructorNoexcept) {
          while (currentTailIndex != stopIndex) {
            if constexpr (UseMoveConstructor) {
              new ((*this->tailBlock)[currentTailIndex])
                T(std::move(*itemFirst));
            } else {
              new ((*this->tailBlock)[currentTailIndex])
                T(details::nomove(*itemFirst));
            }
            ++currentTailIndex;
            ++itemFirst;
          }
        } else {
          MOODYCAMEL_TRY {
            while (currentTailIndex != stopIndex) {
              if constexpr (UseMoveConstructor) {
                new ((*this->tailBlock)[currentTailIndex])
                  T(std::move(*itemFirst));
              } else {
                new ((*this->tailBlock)[currentTailIndex])
                  T(details::nomove(*itemFirst));
              }
              ++currentTailIndex;
              ++itemFirst;
            }
          }
          MOODYCAMEL_CATCH(...) {
            // Oh dear, an exception's been thrown -- destroy the elements that
            // were enqueued so far and revert the entire bulk operation (we'll
            // keep any allocated blocks in our linked list for later, though).
            auto constructedStopIndex = currentTailIndex;
            auto lastBlockEnqueued = this->tailBlock;

            pr_blockIndexFront = originalBlockIndexFront;
            pr_blockIndexFrontMax = originalBlockIndexFrontMax;
            pr_blockIndexSlotsUsed = originalBlockIndexSlotsUsed;
            this->tailBlock =
              startBlock == nullptr ? firstAllocatedBlock : startBlock;

            if (!details::is_trivially_destructible<T>::value) {
              auto block = startBlock;
              if ((startTailIndex & static_cast<index_t>(BLOCK_MASK)) == 0) {
                block = firstAllocatedBlock;
              }
              currentTailIndex = startTailIndex;
              while (true) {
                stopIndex =
                  (currentTailIndex & ~static_cast<index_t>(BLOCK_MASK)) +
                  static_cast<index_t>(PRODUCER_BLOCK_SIZE);
                if (details::circular_less_than<index_t>(
                      constructedStopIndex, stopIndex
                    )) {
                  stopIndex = constructedStopIndex;
                }
                while (currentTailIndex != stopIndex) {
                  (*block)[currentTailIndex]->~T();
                  ++currentTailIndex;
                }
                if (block == lastBlockEnqueued) {
                  break;
                }
                block = block->next;
              }
            }
            MOODYCAMEL_RETHROW;
          }
        }

        if (this->tailBlock == endBlock) {
          assert(currentTailIndex == newTailIndex);
          break;
        }
        this->tailBlock = this->tailBlock->next;
      }

      if constexpr (!IsConstructorNoexcept) {
        if (firstAllocatedBlock != nullptr)
          blockIndex.load(std::memory_order_relaxed)
            ->front.store(
              (pr_blockIndexFront - 1) & (pr_blockIndexSize - 1),
              std::memory_order_release
            );
      }

      this->tailIndex.store(newTailIndex, std::memory_order_release);
      return true;
    }

    template <typename It> size_t dequeue_bulk(It& itemFirst, size_t max) {
      auto tail = this->tailIndex.load(std::memory_order_relaxed);
      auto overcommit = this->dequeueOvercommit.load(std::memory_order_relaxed);
      auto desiredCount = static_cast<size_t>(
        tail - (this->dequeueOptimisticCount.load(std::memory_order_relaxed) -
                overcommit)
      );
      if (details::circular_less_than<size_t>(0, desiredCount)) {
        desiredCount = desiredCount < max ? desiredCount : max;
        std::atomic_thread_fence(std::memory_order_acquire);

        auto myDequeueCount = this->dequeueOptimisticCount.fetch_add(
          desiredCount, std::memory_order_relaxed
        );

        tail = this->tailIndex.load(std::memory_order_acquire);
        auto actualCount =
          static_cast<size_t>(tail - (myDequeueCount - overcommit));
        if (details::circular_less_than<size_t>(0, actualCount)) {
          actualCount = desiredCount < actualCount ? desiredCount : actualCount;
          if (actualCount < desiredCount) {
            this->dequeueOvercommit.fetch_add(
              desiredCount - actualCount, std::memory_order_release
            );
          }

          // Get the first index. Note that since there's guaranteed to be at
          // least actualCount elements, this will never exceed tail.
          auto firstIndex =
            this->headIndex.fetch_add(actualCount, std::memory_order_acq_rel);

          // Determine which block the first element is in
          auto localBlockIndex = blockIndex.load(std::memory_order_acquire);
          auto localBlockIndexHead =
            localBlockIndex->front.load(std::memory_order_acquire);

          auto headBase = localBlockIndex->entries[localBlockIndexHead].base;
          auto firstBlockBaseIndex =
            firstIndex & ~static_cast<index_t>(BLOCK_MASK);
          auto offset = static_cast<size_t>(
            static_cast<typename std::make_signed<index_t>::type>(
              firstBlockBaseIndex - headBase
            ) /
            static_cast<typename std::make_signed<index_t>::type>(
              PRODUCER_BLOCK_SIZE
            )
          );
          auto indexIndex =
            (localBlockIndexHead + offset) & (localBlockIndex->size - 1);

          // Iterate the blocks and dequeue
          auto index = firstIndex;
          do {
            auto firstIndexInBlock = index;
            index_t endIndex = (index & ~static_cast<index_t>(BLOCK_MASK)) +
                               static_cast<index_t>(PRODUCER_BLOCK_SIZE);
            endIndex =
              details::circular_less_than<index_t>(
                firstIndex + static_cast<index_t>(actualCount), endIndex
              )
                ? firstIndex + static_cast<index_t>(actualCount)
                : endIndex;
            auto block = localBlockIndex->entries[indexIndex].block;
            if constexpr (MOODYCAMEL_NOEXCEPT_ASSIGN(
                            details::deref_noexcept(itemFirst) =
                              static_cast<T&&>((*(*block)[index]))
                          )) {
              while (index != endIndex) {
                T& el = *((*block)[index]);
                *itemFirst = static_cast<T&&>(el);
                el.~T();
                ++index;
                ++itemFirst;
              }
            } else {
              MOODYCAMEL_TRY {
                while (index != endIndex) {
                  T& el = *((*block)[index]);
                  *itemFirst = static_cast<T&&>(el);
                  ++itemFirst;
                  el.~T();
                  ++index;
                }
              }
              MOODYCAMEL_CATCH(...) {
                // It's too late to revert the dequeue, but we can make sure
                // that all the dequeued objects are properly destroyed and the
                // block index (and empty count) are properly updated before we
                // propagate the exception
                do {
                  block = localBlockIndex->entries[indexIndex].block;
                  while (index != endIndex) {
                    (*block)[index]->~T();
                    ++index;
                  }
                  block->ConcurrentQueue::Block::template set_many_empty<
                    explicit_context>(
                    firstIndexInBlock,
                    static_cast<size_t>(endIndex - firstIndexInBlock)
                  );
                  indexIndex = (indexIndex + 1) & (localBlockIndex->size - 1);

                  firstIndexInBlock = index;
                  endIndex = (index & ~static_cast<index_t>(BLOCK_MASK)) +
                             static_cast<index_t>(PRODUCER_BLOCK_SIZE);
                  endIndex =
                    details::circular_less_than<index_t>(
                      firstIndex + static_cast<index_t>(actualCount), endIndex
                    )
                      ? firstIndex + static_cast<index_t>(actualCount)
                      : endIndex;
                } while (index != firstIndex + actualCount);

                MOODYCAMEL_RETHROW;
              }
            }
            block->ConcurrentQueue::Block::template set_many_empty<
              explicit_context>(
              firstIndexInBlock,
              static_cast<size_t>(endIndex - firstIndexInBlock)
            );
            indexIndex = (indexIndex + 1) & (localBlockIndex->size - 1);
          } while (index != firstIndex + actualCount);

          return actualCount;
        } else {
          // Wasn't anything to dequeue after all; make the effective dequeue
          // count eventually consistent
          this->dequeueOvercommit.fetch_add(
            desiredCount, std::memory_order_release
          );
        }
      }

      return 0;
    }

  private:
    struct BlockIndexEntry {
      index_t base;
      Block* block;
    };

    struct BlockIndexHeader {
      size_t size;
      std::atomic<size_t>
        front; // Current slot (not next, like pr_blockIndexFront)
      BlockIndexEntry* entries;
      void* prev;
    };

    bool new_block_index(size_t numberOfFilledSlotsToExpose) {
      auto prevBlockSizeMask = pr_blockIndexSize - 1;

      // Create the new block
      pr_blockIndexSize <<= 1;
      auto newRawPtr = static_cast<char*>((Traits::malloc)(
        sizeof(BlockIndexHeader) + std::alignment_of<BlockIndexEntry>::value -
        1 + sizeof(BlockIndexEntry) * pr_blockIndexSize
      ));
      if (newRawPtr == nullptr) {
        pr_blockIndexSize >>= 1; // Reset to allow graceful retry
        return false;
      }

      auto newBlockIndexEntries =
        reinterpret_cast<BlockIndexEntry*>(details::align_for<BlockIndexEntry>(
          newRawPtr + sizeof(BlockIndexHeader)
        ));

      assert(pr_blockIndexFront == pr_blockIndexFrontMax);
      // Copy in all the old indices, if any
      size_t j = 0;
      if (pr_blockIndexSlotsUsed != 0) {
        auto i =
          (pr_blockIndexFront - pr_blockIndexSlotsUsed) & prevBlockSizeMask;
        do {
          newBlockIndexEntries[j] = pr_blockIndexEntries[i];
          ++j;
          i = (i + 1) & prevBlockSizeMask;
        } while (i != pr_blockIndexFront);
      }

      // Update everything
      auto header = new (newRawPtr) BlockIndexHeader;
      header->size = pr_blockIndexSize;
      header->front.store(
        numberOfFilledSlotsToExpose - 1, std::memory_order_relaxed
      );
      header->entries = newBlockIndexEntries;
      header->prev = pr_blockIndexRaw; // we link the new block to the old one
                                       // so we can free it later

      pr_blockIndexFront = j;
      pr_blockIndexFrontMax = j;
      pr_blockIndexEntries = newBlockIndexEntries;
      pr_blockIndexRaw = newRawPtr;
      blockIndex.store(header, std::memory_order_release);

      return true;
    }

  private:
    std::atomic<BlockIndexHeader*> blockIndex;

    // To be used by producer only -- consumer must use the ones in referenced
    // by blockIndex
    size_t pr_blockIndexSlotsUsed;
    size_t pr_blockIndexSize;
    size_t pr_blockIndexFront; // Next slot (not current)
    size_t
      pr_blockIndexFrontMax; // Highest value of pr_blockIndexFront we've set
    BlockIndexEntry* pr_blockIndexEntries;
    void* pr_blockIndexRaw;

#ifdef MOODYCAMEL_QUEUE_INTERNAL_DEBUG
  public:
    ExplicitProducer* nextExplicitProducer;

  private:
#endif

#ifdef MCDBGQ_TRACKMEM
    friend struct MemStats;
#endif
  };

private:
  //////////////////////////////////
  // Implicit queue
  //////////////////////////////////

  struct ImplicitProducer : public ProducerBase {
    ImplicitProducer(ConcurrentQueue* parent_)
        : ProducerBase(parent_, false),
          nextBlockIndexCapacity(IMPLICIT_INITIAL_INDEX_SIZE),
          blockIndex(nullptr) {
      new_block_index();
    }

    ~ImplicitProducer() override {
      // Note that since we're in the destructor we can assume that all
      // enqueue/dequeue operations completed already; this means that all
      // undequeued elements are placed contiguously across contiguous blocks,
      // and that only the first and last remaining blocks can be only partially
      // empty (all other remaining blocks must be completely full).

      // Unregister ourselves for thread termination notification
      if (!this->inactive.load(std::memory_order_relaxed)) {
        details::ThreadExitNotifier::unsubscribe(&threadExitListener);
      }

      // Destroy all remaining elements!
      auto tail = this->tailIndex.load(std::memory_order_relaxed);
      auto index = this->headIndex.load(std::memory_order_relaxed);
      Block* block = nullptr;
      assert(index == tail || details::circular_less_than(index, tail));
      bool forceFreeLastBlock =
        index != tail; // If we enter the loop, then the last (tail) block
                       // will not be freed
      while (index != tail) {
        if ((index & static_cast<index_t>(BLOCK_MASK)) == 0 ||
            block == nullptr) {
          if (block != nullptr) {
            // Free the old block
            this->parent->add_block_to_free_list(block);
          }

          block = get_block_index_entry_for_index(index)->value.load(
            std::memory_order_relaxed
          );
        }

        ((*block)[index])->~T();
        ++index;
      }
      // Even if the queue is empty, there's still one block that's not on the
      // free list (unless the head index reached the end of it, in which case
      // the tail will be poised to create a new block).
      if (this->tailBlock != nullptr &&
          (forceFreeLastBlock || (tail & static_cast<index_t>(BLOCK_MASK)) != 0
          )) {
        this->parent->add_block_to_free_list(this->tailBlock);
      }

      // Destroy block index
      auto localBlockIndex = blockIndex.load(std::memory_order_relaxed);
      if (localBlockIndex != nullptr) {
        for (size_t i = 0; i != localBlockIndex->capacity; ++i) {
          localBlockIndex->index[i]->~BlockIndexEntry();
        }
        do {
          auto prev = localBlockIndex->prev;
          localBlockIndex->~BlockIndexHeader();
          (Traits::free)(localBlockIndex);
          localBlockIndex = prev;
        } while (localBlockIndex != nullptr);
      }
    }

    template <AllocationMode allocMode, typename U>
    inline bool enqueue(U&& element) {
      index_t currentTailIndex =
        this->tailIndex.load(std::memory_order_relaxed);
      index_t newTailIndex = 1 + currentTailIndex;
      if ((currentTailIndex & static_cast<index_t>(BLOCK_MASK)) == 0) {
        // We reached the end of a block, start a new one
        auto head = this->headIndex.load(std::memory_order_relaxed);
        assert(!details::circular_less_than<index_t>(currentTailIndex, head));
        if (!details::circular_less_than<index_t>(
              head, currentTailIndex + PRODUCER_BLOCK_SIZE
            ) ||
            (MAX_SUBQUEUE_SIZE != details::const_numeric_max<size_t>::value &&
             (MAX_SUBQUEUE_SIZE == 0 ||
              MAX_SUBQUEUE_SIZE - PRODUCER_BLOCK_SIZE < currentTailIndex - head)
            )) {
          return false;
        }
#ifdef MCDBGQ_NOLOCKFREE_IMPLICITPRODBLOCKINDEX
        debug::DebugLock lock(mutex);
#endif
        // Find out where we'll be inserting this block in the block index
        BlockIndexEntry* idxEntry;
        if (!insert_block_index_entry<allocMode>(idxEntry, currentTailIndex)) {
          return false;
        }

        // Get ahold of a new block
        auto newBlock =
          this->parent->ConcurrentQueue::template requisition_block<allocMode>(
          );
        if (newBlock == nullptr) {
          rewind_block_index_tail();
          idxEntry->value.store(nullptr, std::memory_order_relaxed);
          return false;
        }
#ifdef MCDBGQ_TRACKMEM
        newBlock->owner = this;
#endif
        newBlock
          ->ConcurrentQueue::Block::template reset_empty<implicit_context>();

        if constexpr (!MOODYCAMEL_NOEXCEPT_CTOR(new (static_cast<T*>(nullptr)
                      ) T(static_cast<U&&>(element)))) {
          // May throw, try to insert now before we publish the fact that we
          // have this new block
          MOODYCAMEL_TRY {
            new ((*newBlock)[currentTailIndex]) T(static_cast<U&&>(element));
          }
          MOODYCAMEL_CATCH(...) {
            rewind_block_index_tail();
            idxEntry->value.store(nullptr, std::memory_order_relaxed);
            this->parent->add_block_to_free_list(newBlock);
            MOODYCAMEL_RETHROW;
          }
        }

        // Insert the new block into the index
        idxEntry->value.store(newBlock, std::memory_order_relaxed);

        this->tailBlock = newBlock;

        if constexpr (!MOODYCAMEL_NOEXCEPT_CTOR(new (static_cast<T*>(nullptr)
                      ) T(static_cast<U&&>(element)))) {
          this->tailIndex.store(newTailIndex, std::memory_order_release);
          return true;
        }
      }

      // Enqueue
      new ((*this->tailBlock)[currentTailIndex]) T(static_cast<U&&>(element));

      this->tailIndex.store(newTailIndex, std::memory_order_release);
      return true;
    }

    template <typename U> bool dequeue(U& element) {
      // See ExplicitProducer::dequeue for rationale and explanation
      index_t tail = this->tailIndex.load(std::memory_order_relaxed);
      index_t overcommit =
        this->dequeueOvercommit.load(std::memory_order_relaxed);
      if (!details::circular_less_than<index_t>(
            this->dequeueOptimisticCount.load(std::memory_order_relaxed) -
              overcommit,
            tail
          )) {
        return false;
      }
      std::atomic_thread_fence(std::memory_order_acquire);

      index_t myDequeueCount =
        this->dequeueOptimisticCount.fetch_add(1, std::memory_order_relaxed);
      tail = this->tailIndex.load(std::memory_order_acquire);
      if (!details::circular_less_than<index_t>(
            myDequeueCount - overcommit, tail
          )) {
        this->dequeueOvercommit.fetch_add(1, std::memory_order_release);
        return false;
      }
      index_t index = this->headIndex.fetch_add(1, std::memory_order_acq_rel);

      // Determine which block the element is in
      auto entry = get_block_index_entry_for_index(index);

      // Dequeue
      auto block = entry->value.load(std::memory_order_relaxed);
      T& el = *((*block)[index]);

      if constexpr (!MOODYCAMEL_NOEXCEPT_ASSIGN(
                      element = static_cast<T&&>(el)
                    )) {
#ifdef MCDBGQ_NOLOCKFREE_IMPLICITPRODBLOCKINDEX
        // Note: Acquiring the mutex with every dequeue instead of only when
        // a block is released is very sub-optimal, but it is, after all,
        // purely debug code.
        debug::DebugLock lock(producer->mutex);
#endif
        struct Guard {
          Block* block;
          index_t index;
          BlockIndexEntry* entry;
          ConcurrentQueue* parent;

          ~Guard() {
            (*block)[index]->~T();
            if (block->ConcurrentQueue::Block::template set_empty<
                  implicit_context>(index)) {
              entry->value.store(nullptr, std::memory_order_relaxed);
              parent->add_block_to_free_list(block);
            }
          }
        } guard = {block, index, entry, this->parent};

        element = static_cast<T&&>(el); // NOLINT
      } else {
        element = static_cast<T&&>(el); // NOLINT
        el.~T();                        // NOLINT

        if (block->ConcurrentQueue::Block::template set_empty<implicit_context>(
              index
            )) {
          {
#ifdef MCDBGQ_NOLOCKFREE_IMPLICITPRODBLOCKINDEX
            debug::DebugLock lock(mutex);
#endif
            // Add the block back into the global free pool (and remove from
            // block index)
            // TODO steal the entire block by swapping it with our explicit
            // producer block and releasing that block instead (earlier in
            // the function) if the number of tasks available is greater
            // than some threshold
            entry->value.store(nullptr, std::memory_order_relaxed);
          }
          this->parent->add_block_to_free_list(block
          ); // releases the above store
        }
      }

      return true;
    }

    template <AllocationMode allocMode, typename It>
    bool enqueue_bulk(It itemFirst, size_t count) {
      static constexpr bool HasMoveConstructor = std::is_constructible_v<
        T, std::add_rvalue_reference_t<std::iter_value_t<It>>>;
      static constexpr bool HasNoexceptMoveConstructor =
        std::is_nothrow_constructible_v<
          T, std::add_rvalue_reference_t<std::iter_value_t<It>>>;

      static constexpr bool HasCopyConstructor = std::is_constructible_v<
        T, std::add_lvalue_reference_t<std::iter_value_t<It>>>;
      static constexpr bool HasNoexceptCopyConstructor =
        std::is_nothrow_constructible_v<
          T, std::add_lvalue_reference_t<std::iter_value_t<It>>>;

      // Prefer constructors in this order:
      // 1. Noexcept move constructor
      // 2. Noexcept copy constructor
      // 3. Copy constructor
      // 4. Move constructor

      // For 3. and 4., prefer copy constructor even if move constructor is
      // available because we may have to revert if there's an
      // exception.
      static constexpr bool UseMoveConstructor =
        HasNoexceptMoveConstructor || !HasCopyConstructor;

      static constexpr bool IsConstructorNoexcept =
        HasNoexceptMoveConstructor || HasNoexceptCopyConstructor;

      // First, we need to make sure we have enough room to enqueue all of the
      // elements; this means pre-allocating blocks and putting them in the
      // block index (but only if all the allocations succeeded).

      // Note that the tailBlock we start off with may not be owned by us any
      // more; this happens if it was filled up exactly to the top (setting
      // tailIndex to the first index of the next block which is not yet
      // allocated), then dequeued completely (putting it on the free list)
      // before we enqueue again.
      index_t startTailIndex = this->tailIndex.load(std::memory_order_relaxed);
      auto startBlock = this->tailBlock;
      Block* firstAllocatedBlock = nullptr;
      auto endBlock = this->tailBlock;

      // Figure out how many blocks we'll need to allocate, and do so
      size_t blockBaseDiff =
        ((startTailIndex + count - 1) & ~static_cast<index_t>(BLOCK_MASK)) -
        ((startTailIndex - 1) & ~static_cast<index_t>(BLOCK_MASK));
      index_t currentTailIndex =
        (startTailIndex - 1) & ~static_cast<index_t>(BLOCK_MASK);
      if (blockBaseDiff > 0) {
#ifdef MCDBGQ_NOLOCKFREE_IMPLICITPRODBLOCKINDEX
        debug::DebugLock lock(mutex);
#endif
        do {
          blockBaseDiff -= static_cast<index_t>(PRODUCER_BLOCK_SIZE);
          currentTailIndex += static_cast<index_t>(PRODUCER_BLOCK_SIZE);

          // Find out where we'll be inserting this block in the block index
          BlockIndexEntry* idxEntry =
            nullptr; // initialization here unnecessary but compiler can't
                     // always tell
          Block* newBlock;
          bool indexInserted = false;
          auto head = this->headIndex.load(std::memory_order_relaxed);
          assert(!details::circular_less_than<index_t>(currentTailIndex, head));
          bool full =
            !details::circular_less_than<index_t>(
              head, currentTailIndex + PRODUCER_BLOCK_SIZE
            ) ||
            (MAX_SUBQUEUE_SIZE != details::const_numeric_max<size_t>::value &&
             (MAX_SUBQUEUE_SIZE == 0 ||
              MAX_SUBQUEUE_SIZE - PRODUCER_BLOCK_SIZE < currentTailIndex - head)
            );

          if (full ||
              !(indexInserted = insert_block_index_entry<allocMode>(
                  idxEntry, currentTailIndex
                )) ||
              (newBlock =
                 this->parent
                   ->ConcurrentQueue::template requisition_block<allocMode>()
              ) == nullptr) {
            // Index allocation or block allocation failed; revert any other
            // allocations and index insertions done so far for this operation
            if (indexInserted) {
              rewind_block_index_tail();
              idxEntry->value.store(nullptr, std::memory_order_relaxed);
            }
            currentTailIndex =
              (startTailIndex - 1) & ~static_cast<index_t>(BLOCK_MASK);
            for (auto block = firstAllocatedBlock; block != nullptr;
                 block = block->next) {
              currentTailIndex += static_cast<index_t>(PRODUCER_BLOCK_SIZE);
              idxEntry = get_block_index_entry_for_index(currentTailIndex);
              idxEntry->value.store(nullptr, std::memory_order_relaxed);
              rewind_block_index_tail();
            }
            this->parent->add_blocks_to_free_list(firstAllocatedBlock);
            this->tailBlock = startBlock;

            return false;
          }

#ifdef MCDBGQ_TRACKMEM
          newBlock->owner = this;
#endif
          newBlock
            ->ConcurrentQueue::Block::template reset_empty<implicit_context>();
          newBlock->next = nullptr;

          // Insert the new block into the index
          idxEntry->value.store(newBlock, std::memory_order_relaxed);

          // Store the chain of blocks so that we can undo if later allocations
          // fail, and so that we can find the blocks when we do the actual
          // enqueueing
          if ((startTailIndex & static_cast<index_t>(BLOCK_MASK)) != 0 ||
              firstAllocatedBlock != nullptr) {
            assert(this->tailBlock != nullptr);
            this->tailBlock->next = newBlock;
          }
          this->tailBlock = newBlock;
          endBlock = newBlock;
          firstAllocatedBlock =
            firstAllocatedBlock == nullptr ? newBlock : firstAllocatedBlock;
        } while (blockBaseDiff > 0);
      }

      // Enqueue, one block at a time
      index_t newTailIndex = startTailIndex + static_cast<index_t>(count);
      currentTailIndex = startTailIndex;
      this->tailBlock = startBlock;
      assert(
        (startTailIndex & static_cast<index_t>(BLOCK_MASK)) != 0 ||
        firstAllocatedBlock != nullptr || count == 0
      );
      if ((startTailIndex & static_cast<index_t>(BLOCK_MASK)) == 0 &&
          firstAllocatedBlock != nullptr) {
        this->tailBlock = firstAllocatedBlock;
      }
      while (true) {
        index_t stopIndex =
          (currentTailIndex & ~static_cast<index_t>(BLOCK_MASK)) +
          static_cast<index_t>(PRODUCER_BLOCK_SIZE);
        if (details::circular_less_than<index_t>(newTailIndex, stopIndex)) {
          stopIndex = newTailIndex;
        }

        if constexpr (IsConstructorNoexcept) {
          while (currentTailIndex != stopIndex) {
            if constexpr (UseMoveConstructor) {
              new ((*this->tailBlock)[currentTailIndex])
                T(std::move(*itemFirst));
            } else {
              new ((*this->tailBlock)[currentTailIndex])
                T(details::nomove(*itemFirst));
            }
            ++currentTailIndex;
            ++itemFirst;
          }
        } else {
          MOODYCAMEL_TRY {
            while (currentTailIndex != stopIndex) {
              if constexpr (UseMoveConstructor) {
                new ((*this->tailBlock)[currentTailIndex])
                  T(std::move(*itemFirst));
              } else {
                new ((*this->tailBlock)[currentTailIndex])
                  T(details::nomove(*itemFirst));
              }
              ++currentTailIndex;
              ++itemFirst;
            }
          }
          MOODYCAMEL_CATCH(...) {
            auto constructedStopIndex = currentTailIndex;
            auto lastBlockEnqueued = this->tailBlock;

            if (!details::is_trivially_destructible<T>::value) {
              auto block = startBlock;
              if ((startTailIndex & static_cast<index_t>(BLOCK_MASK)) == 0) {
                block = firstAllocatedBlock;
              }
              currentTailIndex = startTailIndex;
              while (true) {
                stopIndex =
                  (currentTailIndex & ~static_cast<index_t>(BLOCK_MASK)) +
                  static_cast<index_t>(PRODUCER_BLOCK_SIZE);
                if (details::circular_less_than<index_t>(
                      constructedStopIndex, stopIndex
                    )) {
                  stopIndex = constructedStopIndex;
                }
                while (currentTailIndex != stopIndex) {
                  (*block)[currentTailIndex]->~T();
                  ++currentTailIndex;
                }
                if (block == lastBlockEnqueued) {
                  break;
                }
                block = block->next;
              }
            }

            currentTailIndex =
              (startTailIndex - 1) & ~static_cast<index_t>(BLOCK_MASK);
            for (auto block = firstAllocatedBlock; block != nullptr;
                 block = block->next) {
              currentTailIndex += static_cast<index_t>(PRODUCER_BLOCK_SIZE);
              auto idxEntry = get_block_index_entry_for_index(currentTailIndex);
              idxEntry->value.store(nullptr, std::memory_order_relaxed);
              rewind_block_index_tail();
            }
            this->parent->add_blocks_to_free_list(firstAllocatedBlock);
            this->tailBlock = startBlock;
            MOODYCAMEL_RETHROW;
          }
        }

        if (this->tailBlock == endBlock) {
          assert(currentTailIndex == newTailIndex);
          break;
        }
        this->tailBlock = this->tailBlock->next;
      }
      this->tailIndex.store(newTailIndex, std::memory_order_release);
      return true;
    }

    template <typename It> size_t dequeue_bulk(It& itemFirst, size_t max) {
      auto tail = this->tailIndex.load(std::memory_order_relaxed);
      auto overcommit = this->dequeueOvercommit.load(std::memory_order_relaxed);
      auto desiredCount = static_cast<size_t>(
        tail - (this->dequeueOptimisticCount.load(std::memory_order_relaxed) -
                overcommit)
      );
      if (details::circular_less_than<size_t>(0, desiredCount)) {
        desiredCount = desiredCount < max ? desiredCount : max;
        std::atomic_thread_fence(std::memory_order_acquire);

        auto myDequeueCount = this->dequeueOptimisticCount.fetch_add(
          desiredCount, std::memory_order_relaxed
        );

        tail = this->tailIndex.load(std::memory_order_acquire);
        auto actualCount =
          static_cast<size_t>(tail - (myDequeueCount - overcommit));
        if (details::circular_less_than<size_t>(0, actualCount)) {
          actualCount = desiredCount < actualCount ? desiredCount : actualCount;
          if (actualCount < desiredCount) {
            this->dequeueOvercommit.fetch_add(
              desiredCount - actualCount, std::memory_order_release
            );
          }

          // Get the first index. Note that since there's guaranteed to be at
          // least actualCount elements, this will never exceed tail.
          auto firstIndex =
            this->headIndex.fetch_add(actualCount, std::memory_order_acq_rel);

          // Iterate the blocks and dequeue
          auto index = firstIndex;
          BlockIndexHeader* localBlockIndex;
          auto indexIndex =
            get_block_index_index_for_index(index, localBlockIndex);
          do {
            auto blockStartIndex = index;
            index_t endIndex = (index & ~static_cast<index_t>(BLOCK_MASK)) +
                               static_cast<index_t>(PRODUCER_BLOCK_SIZE);
            endIndex =
              details::circular_less_than<index_t>(
                firstIndex + static_cast<index_t>(actualCount), endIndex
              )
                ? firstIndex + static_cast<index_t>(actualCount)
                : endIndex;

            auto entry = localBlockIndex->index[indexIndex];
            auto block = entry->value.load(std::memory_order_relaxed);
            if constexpr (MOODYCAMEL_NOEXCEPT_ASSIGN(
                            details::deref_noexcept(itemFirst) =
                              static_cast<T&&>((*(*block)[index]))
                          )) {
              while (index != endIndex) {
                T& el = *((*block)[index]);
                *itemFirst = static_cast<T&&>(el);
                el.~T();
                ++index;
                ++itemFirst;
              }
            } else {
              MOODYCAMEL_TRY {
                while (index != endIndex) {
                  T& el = *((*block)[index]);
                  *itemFirst = static_cast<T&&>(el);
                  ++itemFirst;
                  el.~T();
                  ++index;
                }
              }
              MOODYCAMEL_CATCH(...) {
                do {
                  entry = localBlockIndex->index[indexIndex];
                  block = entry->value.load(std::memory_order_relaxed);
                  while (index != endIndex) {
                    (*block)[index]->~T();
                    ++index;
                  }

                  if (block->ConcurrentQueue::Block::template set_many_empty<
                        implicit_context>(
                        blockStartIndex,
                        static_cast<size_t>(endIndex - blockStartIndex)
                      )) {
#ifdef MCDBGQ_NOLOCKFREE_IMPLICITPRODBLOCKINDEX
                    debug::DebugLock lock(mutex);
#endif
                    entry->value.store(nullptr, std::memory_order_relaxed);
                    this->parent->add_block_to_free_list(block);
                  }
                  indexIndex =
                    (indexIndex + 1) & (localBlockIndex->capacity - 1);

                  blockStartIndex = index;
                  endIndex = (index & ~static_cast<index_t>(BLOCK_MASK)) +
                             static_cast<index_t>(PRODUCER_BLOCK_SIZE);
                  endIndex =
                    details::circular_less_than<index_t>(
                      firstIndex + static_cast<index_t>(actualCount), endIndex
                    )
                      ? firstIndex + static_cast<index_t>(actualCount)
                      : endIndex;
                } while (index != firstIndex + actualCount);

                MOODYCAMEL_RETHROW;
              }
            }
            if (block->ConcurrentQueue::Block::template set_many_empty<
                  implicit_context>(
                  blockStartIndex,
                  static_cast<size_t>(endIndex - blockStartIndex)
                )) {
              {
#ifdef MCDBGQ_NOLOCKFREE_IMPLICITPRODBLOCKINDEX
                debug::DebugLock lock(mutex);
#endif
                // Note that the set_many_empty above did a release, meaning
                // that anybody who acquires the block we're about to free can
                // use it safely since our writes (and reads!) will have
                // happened-before then.
                entry->value.store(nullptr, std::memory_order_relaxed);
              }
              this->parent->add_block_to_free_list(block
              ); // releases the above store
            }
            indexIndex = (indexIndex + 1) & (localBlockIndex->capacity - 1);
          } while (index != firstIndex + actualCount);

          return actualCount;
        } else {
          this->dequeueOvercommit.fetch_add(
            desiredCount, std::memory_order_release
          );
        }
      }

      return 0;
    }

  private:
    // The block size must be > 1, so any number with the low bit set is an
    // invalid block base index
    static const index_t INVALID_BLOCK_BASE = 1;

    struct BlockIndexEntry {
      std::atomic<index_t> key;
      std::atomic<Block*> value;
    };

    struct BlockIndexHeader {
      size_t capacity;
      std::atomic<size_t> tail;
      BlockIndexEntry* entries;
      BlockIndexEntry** index;
      BlockIndexHeader* prev;
    };

    template <AllocationMode allocMode>
    inline bool insert_block_index_entry(
      BlockIndexEntry*& idxEntry, index_t blockStartIndex
    ) {
      auto localBlockIndex =
        blockIndex.load(std::memory_order_relaxed); // We're the only writer
                                                    // thread, relaxed is OK
      if (localBlockIndex == nullptr) {
        return false; // this can happen if new_block_index failed in the
                      // constructor
      }
      size_t newTail =
        (localBlockIndex->tail.load(std::memory_order_relaxed) + 1) &
        (localBlockIndex->capacity - 1);
      idxEntry = localBlockIndex->index[newTail];
      if (idxEntry->key.load(std::memory_order_relaxed) == INVALID_BLOCK_BASE ||
          idxEntry->value.load(std::memory_order_relaxed) == nullptr) {

        idxEntry->key.store(blockStartIndex, std::memory_order_relaxed);
        localBlockIndex->tail.store(newTail, std::memory_order_release);
        return true;
      }

      // No room in the old block index, try to allocate another one!
      if constexpr (allocMode == CannotAlloc) {
        return false;
      } else if (!new_block_index()) {
        return false;
      } else {
        localBlockIndex = blockIndex.load(std::memory_order_relaxed);
        newTail = (localBlockIndex->tail.load(std::memory_order_relaxed) + 1) &
                  (localBlockIndex->capacity - 1);
        idxEntry = localBlockIndex->index[newTail];
        assert(
          idxEntry->key.load(std::memory_order_relaxed) == INVALID_BLOCK_BASE
        );
        idxEntry->key.store(blockStartIndex, std::memory_order_relaxed);
        localBlockIndex->tail.store(newTail, std::memory_order_release);
        return true;
      }
    }

    inline void rewind_block_index_tail() {
      auto localBlockIndex = blockIndex.load(std::memory_order_relaxed);
      localBlockIndex->tail.store(
        (localBlockIndex->tail.load(std::memory_order_relaxed) - 1) &
          (localBlockIndex->capacity - 1),
        std::memory_order_relaxed
      );
    }

    inline BlockIndexEntry* get_block_index_entry_for_index(index_t index
    ) const {
      BlockIndexHeader* localBlockIndex;
      auto idx = get_block_index_index_for_index(index, localBlockIndex);
      return localBlockIndex->index[idx];
    }

    inline size_t get_block_index_index_for_index(
      index_t index, BlockIndexHeader*& localBlockIndex
    ) const {
#ifdef MCDBGQ_NOLOCKFREE_IMPLICITPRODBLOCKINDEX
      debug::DebugLock lock(mutex);
#endif
      index &= ~static_cast<index_t>(BLOCK_MASK);
      localBlockIndex = blockIndex.load(std::memory_order_acquire);
      auto tail = localBlockIndex->tail.load(std::memory_order_acquire);
      auto tailBase =
        localBlockIndex->index[tail]->key.load(std::memory_order_relaxed);
      assert(tailBase != INVALID_BLOCK_BASE);
      // Note: Must use division instead of shift because the index may wrap
      // around, causing a negative offset, whose negativity we want to preserve
      auto offset = static_cast<size_t>(
        static_cast<typename std::make_signed<index_t>::type>(
          index - tailBase
        ) /
        static_cast<typename std::make_signed<index_t>::type>(
          PRODUCER_BLOCK_SIZE
        )
      );
      size_t idx = (tail + offset) & (localBlockIndex->capacity - 1);
      assert(
        localBlockIndex->index[idx]->key.load(std::memory_order_relaxed) ==
          index &&
        localBlockIndex->index[idx]->value.load(std::memory_order_relaxed) !=
          nullptr
      );
      return idx;
    }

    bool new_block_index() {
      auto prev = blockIndex.load(std::memory_order_relaxed);
      size_t prevCapacity = prev == nullptr ? 0 : prev->capacity;
      auto entryCount = prev == nullptr ? nextBlockIndexCapacity : prevCapacity;
      auto raw = static_cast<char*>((Traits::malloc)(
        sizeof(BlockIndexHeader) + std::alignment_of<BlockIndexEntry>::value -
        1 + sizeof(BlockIndexEntry) * entryCount +
        std::alignment_of<BlockIndexEntry*>::value - 1 +
        sizeof(BlockIndexEntry*) * nextBlockIndexCapacity
      ));
      if (raw == nullptr) {
        return false;
      }

      auto header = new (raw) BlockIndexHeader;
      auto entries = reinterpret_cast<BlockIndexEntry*>(
        details::align_for<BlockIndexEntry>(raw + sizeof(BlockIndexHeader))
      );
      auto index = reinterpret_cast<BlockIndexEntry**>(
        details::align_for<BlockIndexEntry*>(
          reinterpret_cast<char*>(entries) +
          sizeof(BlockIndexEntry) * entryCount
        )
      );
      if (prev != nullptr) {
        auto prevTail = prev->tail.load(std::memory_order_relaxed);
        auto prevPos = prevTail;
        size_t i = 0;
        do {
          prevPos = (prevPos + 1) & (prev->capacity - 1);
          index[i] = prev->index[prevPos];
          ++i;
        } while (prevPos != prevTail);
        assert(i == prevCapacity);
      }
      for (size_t i = 0; i != entryCount; ++i) {
        new (entries + i) BlockIndexEntry;
        entries[i].key.store(INVALID_BLOCK_BASE, std::memory_order_relaxed);
        index[prevCapacity + i] = entries + i;
      }
      header->prev = prev;
      header->entries = entries;
      header->index = index;
      header->capacity = nextBlockIndexCapacity;
      header->tail.store(
        (prevCapacity - 1) & (nextBlockIndexCapacity - 1),
        std::memory_order_relaxed
      );

      blockIndex.store(header, std::memory_order_release);

      nextBlockIndexCapacity <<= 1;

      return true;
    }

  private:
    size_t nextBlockIndexCapacity;
    std::atomic<BlockIndexHeader*> blockIndex;

  public:
    details::ThreadExitListener threadExitListener;

  private:
#ifdef MOODYCAMEL_QUEUE_INTERNAL_DEBUG
  public:
    ImplicitProducer* nextImplicitProducer;

  private:
#endif

#ifdef MCDBGQ_NOLOCKFREE_IMPLICITPRODBLOCKINDEX
    mutable debug::DebugMutex mutex;
#endif
#ifdef MCDBGQ_TRACKMEM
    friend struct MemStats;
#endif
  };

  //////////////////////////////////
  // Block pool manipulation
  //////////////////////////////////

  void populate_initial_block_list(size_t blockCount) {
    initialBlockPoolSize = blockCount;
    if (initialBlockPoolSize == 0) {
      initialBlockPool = nullptr;
      return;
    }

    initialBlockPool = create_array<Block>(blockCount);
    if (initialBlockPool == nullptr) {
      initialBlockPoolSize = 0;
    }
    for (size_t i = 0; i < initialBlockPoolSize; ++i) {
      initialBlockPool[i].dynamicallyAllocated = false;
    }
  }

  inline Block* try_get_block_from_initial_pool() {
    if (initialBlockPoolIndex.load(std::memory_order_relaxed) >=
        initialBlockPoolSize) {
      return nullptr;
    }

    auto index = initialBlockPoolIndex.fetch_add(1, std::memory_order_relaxed);

    return index < initialBlockPoolSize ? (initialBlockPool + index) : nullptr;
  }

  inline void add_block_to_free_list(Block* block) {
#ifdef MCDBGQ_TRACKMEM
    block->owner = nullptr;
#endif
    if (!Traits::RECYCLE_ALLOCATED_BLOCKS && block->dynamicallyAllocated) {
      destroy(block);
    } else {
      freeList.add(block);
    }
  }

  inline void add_blocks_to_free_list(Block* block) {
    while (block != nullptr) {
      auto next = block->next;
      add_block_to_free_list(block);
      block = next;
    }
  }

  inline Block* try_get_block_from_free_list() { return freeList.try_get(); }

  // Gets a free block from one of the memory pools, or allocates a new one (if
  // applicable)
  template <AllocationMode canAlloc> Block* requisition_block() {
    auto block = try_get_block_from_initial_pool();
    if (block != nullptr) {
      return block;
    }

    block = try_get_block_from_free_list();
    if (block != nullptr) {
      return block;
    }

    if constexpr (canAlloc == CanAlloc) {
      return create<Block>();
    } else {
      return nullptr;
    }
  }

#ifdef MCDBGQ_TRACKMEM
public:
  struct MemStats {
    size_t allocatedBlocks;
    size_t usedBlocks;
    size_t freeBlocks;
    size_t ownedBlocksExplicit;
    size_t ownedBlocksImplicit;
    size_t implicitProducers;
    size_t explicitProducers;
    size_t elementsEnqueued;
    size_t blockClassBytes;
    size_t queueClassBytes;
    size_t implicitBlockIndexBytes;
    size_t explicitBlockIndexBytes;

    friend class ConcurrentQueue;

  private:
    static MemStats getFor(ConcurrentQueue* q) {
      MemStats stats = {0};

      stats.elementsEnqueued = q->size_approx();

      auto block = q->freeList.head_unsafe();
      while (block != nullptr) {
        ++stats.allocatedBlocks;
        ++stats.freeBlocks;
        block = block->freeListNext.load(std::memory_order_relaxed);
      }

      for (auto ptr = q->producerListTail.load(std::memory_order_acquire);
           ptr != nullptr; ptr = ptr->next_prod()) {
        bool implicit = dynamic_cast<ImplicitProducer*>(ptr) != nullptr;
        stats.implicitProducers += implicit ? 1 : 0;
        stats.explicitProducers += implicit ? 0 : 1;

        if (implicit) {
          auto prod = static_cast<ImplicitProducer*>(ptr);
          stats.queueClassBytes += sizeof(ImplicitProducer);
          auto head = prod->headIndex.load(std::memory_order_relaxed);
          auto tail = prod->tailIndex.load(std::memory_order_relaxed);
          auto hash = prod->blockIndex.load(std::memory_order_relaxed);
          if (hash != nullptr) {
            for (size_t i = 0; i != hash->capacity; ++i) {
              if (hash->index[i]->key.load(std::memory_order_relaxed) !=
                    ImplicitProducer::INVALID_BLOCK_BASE &&
                  hash->index[i]->value.load(std::memory_order_relaxed) !=
                    nullptr) {
                ++stats.allocatedBlocks;
                ++stats.ownedBlocksImplicit;
              }
            }
            stats.implicitBlockIndexBytes +=
              hash->capacity *
              sizeof(typename ImplicitProducer::BlockIndexEntry);
            for (; hash != nullptr; hash = hash->prev) {
              stats.implicitBlockIndexBytes +=
                sizeof(typename ImplicitProducer::BlockIndexHeader) +
                hash->capacity *
                  sizeof(typename ImplicitProducer::BlockIndexEntry*);
            }
          }
          for (; details::circular_less_than<index_t>(head, tail);
               head += PRODUCER_BLOCK_SIZE) {
            // auto block = prod->get_block_index_entry_for_index(head);
            ++stats.usedBlocks;
          }
        } else {
          auto prod = static_cast<ExplicitProducer*>(ptr);
          stats.queueClassBytes += sizeof(ExplicitProducer);
          auto tailBlock = prod->tailBlock;
          bool wasNonEmpty = false;
          if (tailBlock != nullptr) {
            auto block = tailBlock;
            do {
              ++stats.allocatedBlocks;
              if (!block->ConcurrentQueue::Block::template is_empty<
                    explicit_context>() ||
                  wasNonEmpty) {
                ++stats.usedBlocks;
                wasNonEmpty = wasNonEmpty || block != tailBlock;
              }
              ++stats.ownedBlocksExplicit;
              block = block->next;
            } while (block != tailBlock);
          }
          auto index = prod->blockIndex.load(std::memory_order_relaxed);
          while (index != nullptr) {
            stats.explicitBlockIndexBytes +=
              sizeof(typename ExplicitProducer::BlockIndexHeader) +
              index->size * sizeof(typename ExplicitProducer::BlockIndexEntry);
            index = static_cast<typename ExplicitProducer::BlockIndexHeader*>(
              index->prev
            );
          }
        }
      }

      auto freeOnInitialPool =
        q->initialBlockPoolIndex.load(std::memory_order_relaxed) >=
            q->initialBlockPoolSize
          ? 0
          : q->initialBlockPoolSize -
              q->initialBlockPoolIndex.load(std::memory_order_relaxed);
      stats.allocatedBlocks += freeOnInitialPool;
      stats.freeBlocks += freeOnInitialPool;

      stats.blockClassBytes = sizeof(Block) * stats.allocatedBlocks;
      stats.queueClassBytes += sizeof(ConcurrentQueue);

      return stats;
    }
  };

  // For debugging only. Not thread-safe.
  MemStats getMemStats() { return MemStats::getFor(this); }

private:
  friend struct MemStats;
#endif

  //////////////////////////////////
  // Producer list manipulation
  //////////////////////////////////

  ProducerBase* recycle_or_create_producer() {
#ifdef MCDBGQ_NOLOCKFREE_IMPLICITPRODHASH
    debug::DebugLock lock(implicitProdMutex);
#endif
    // Try to re-use one first
    for (auto ptr = producerListTail.load(std::memory_order_acquire);
         ptr != nullptr; ptr = ptr->next_prod()) {
      if (ptr->inactive.load(std::memory_order_relaxed) &&
          ptr->isExplicit == false) {
        bool expected = true;
        if (ptr->inactive.compare_exchange_strong(
              expected, /* desired */ false, std::memory_order_acquire,
              std::memory_order_relaxed
            )) {
          // We caught one! It's been marked as activated, the caller can have
          // it
          return ptr;
        }
      }
    }

    return add_producer(create<ImplicitProducer>(this));
  }

  ProducerBase* add_producer(ProducerBase* producer) {
    // Handle failed memory allocation
    if (producer == nullptr) {
      return nullptr;
    }

    producerCount.fetch_add(1, std::memory_order_relaxed);

    // Add it to the lock-free list
    auto prevTail = producerListTail.load(std::memory_order_relaxed);
    do {
      producer->next = prevTail;
    } while (!producerListTail.compare_exchange_weak(
      prevTail, producer, std::memory_order_release, std::memory_order_relaxed
    ));

#ifdef MOODYCAMEL_QUEUE_INTERNAL_DEBUG
    if (producer->isExplicit) {
      auto prevTailExplicit = explicitProducers.load(std::memory_order_relaxed);
      do {
        static_cast<ExplicitProducer*>(producer)->nextExplicitProducer =
          prevTailExplicit;
      } while (!explicitProducers.compare_exchange_weak(
        prevTailExplicit, static_cast<ExplicitProducer*>(producer),
        std::memory_order_release, std::memory_order_relaxed
      ));
    } else {
      auto prevTailImplicit = implicitProducers.load(std::memory_order_relaxed);
      do {
        static_cast<ImplicitProducer*>(producer)->nextImplicitProducer =
          prevTailImplicit;
      } while (!implicitProducers.compare_exchange_weak(
        prevTailImplicit, static_cast<ImplicitProducer*>(producer),
        std::memory_order_release, std::memory_order_relaxed
      ));
    }
#endif

    return producer;
  }

  //////////////////////////////////
  // Implicit producer hash
  //////////////////////////////////

  struct ImplicitProducerKVP {
    std::atomic<details::thread_id_t> key;
    ImplicitProducer* value; // No need for atomicity since it's only read by
                             // the thread that sets it in the first place

    ImplicitProducerKVP() : value(nullptr) {}

    ImplicitProducerKVP(ImplicitProducerKVP&& other) = delete;

    inline ImplicitProducerKVP& operator=(ImplicitProducerKVP&& other) = delete;
  };

  struct ImplicitProducerHash {
    size_t capacity;
    ImplicitProducerKVP* entries;
    ImplicitProducerHash* prev;
  };

  inline void populate_initial_implicit_producer_hash() {
    if constexpr (INITIAL_IMPLICIT_PRODUCER_HASH_SIZE == 0) {
      return;
    } else {
      implicitProducerHashCount.store(0, std::memory_order_relaxed);
      auto hash = &initialImplicitProducerHash;
      hash->capacity = INITIAL_IMPLICIT_PRODUCER_HASH_SIZE;
      hash->entries = &initialImplicitProducerHashEntries[0];
      for (size_t i = 0; i != INITIAL_IMPLICIT_PRODUCER_HASH_SIZE; ++i) {
        initialImplicitProducerHashEntries[i].key.store(
          details::invalid_thread_id, std::memory_order_relaxed
        );
      }
      hash->prev = nullptr;
      implicitProducerHash.store(hash, std::memory_order_relaxed);
    }
  }

  // Only fails (returns nullptr) if memory allocation fails
  ImplicitProducer* get_or_add_implicit_producer() {
    // Note that since the data is essentially thread-local (key is thread ID),
    // there's a reduced need for fences (memory ordering is already consistent
    // for any individual thread), except for the current table itself.

    // Start by looking for the thread ID in the current and all previous hash
    // tables. If it's not found, it must not be in there yet, since this same
    // thread would have added it previously to one of the tables that we
    // traversed.

    // Code and algorithm adapted from
    // http://preshing.com/20130605/the-worlds-simplest-lock-free-hash-table

#ifdef MCDBGQ_NOLOCKFREE_IMPLICITPRODHASH
    debug::DebugLock lock(implicitProdMutex);
#endif

    auto id = details::thread_id();
    auto hashedId = details::hash_thread_id(id);

    auto mainHash = implicitProducerHash.load(std::memory_order_acquire);
    assert(
      mainHash != nullptr
    ); // silence clang-tidy and MSVC warnings (hash cannot be null)
    for (auto hash = mainHash; hash != nullptr; hash = hash->prev) {
      // Look for the id in this hash
      auto index = hashedId;
      while (true) { // Not an infinite loop because at least one slot is free
                     // in the hash table
        index &= hash->capacity - 1u;

        auto probedKey =
          hash->entries[index].key.load(std::memory_order_relaxed);
        if (probedKey == id) {
          // Found it! If we had to search several hashes deep, though, we
          // should lazily add it to the current main hash table to avoid the
          // extended search next time. Note there's guaranteed to be room in
          // the current hash table since every subsequent table implicitly
          // reserves space for all previous tables (there's only one
          // implicitProducerHashCount).
          auto value = hash->entries[index].value;
          if (hash != mainHash) {
            index = hashedId;
            while (true) {
              index &= mainHash->capacity - 1u;
              auto empty = details::invalid_thread_id;
              auto reusable = details::invalid_thread_id2;
              if (mainHash->entries[index].key.compare_exchange_strong(
                    empty, id, std::memory_order_seq_cst,
                    std::memory_order_relaxed
                  ) ||
                  mainHash->entries[index].key.compare_exchange_strong(
                    reusable, id, std::memory_order_seq_cst,
                    std::memory_order_relaxed
                  )) {
                mainHash->entries[index].value = value;
                break;
              }
              ++index;
            }
          }

          return value;
        }
        if (probedKey == details::invalid_thread_id) {
          break; // Not in this hash table
        }
        ++index;
      }
    }

    // Insert!
    auto newCount =
      1 + implicitProducerHashCount.fetch_add(1, std::memory_order_relaxed);
    while (true) {
      // NOLINTNEXTLINE(clang-analyzer-core.NullDereference)
      if (newCount >= (mainHash->capacity >> 1) &&
          !implicitProducerHashResizeInProgress.test_and_set(
            std::memory_order_acquire
          )) {
        // We've acquired the resize lock, try to allocate a bigger hash table.
        // Note the acquire fence synchronizes with the release fence at the end
        // of this block, and hence when we reload implicitProducerHash it must
        // be the most recent version (it only gets changed within this locked
        // block).
        mainHash = implicitProducerHash.load(std::memory_order_acquire);
        if (newCount >= (mainHash->capacity >> 1)) {
          size_t newCapacity = mainHash->capacity << 1;
          while (newCount >= (newCapacity >> 1)) {
            newCapacity <<= 1;
          }
          auto raw = static_cast<char*>((Traits::malloc)(
            sizeof(ImplicitProducerHash) +
            std::alignment_of<ImplicitProducerKVP>::value - 1 +
            sizeof(ImplicitProducerKVP) * newCapacity
          ));
          if (raw == nullptr) {
            // Allocation failed
            implicitProducerHashCount.fetch_sub(1, std::memory_order_relaxed);
            implicitProducerHashResizeInProgress.clear(std::memory_order_relaxed
            );
            return nullptr;
          }

          auto newHash = new (raw) ImplicitProducerHash;
          newHash->capacity = static_cast<size_t>(newCapacity);
          newHash->entries = reinterpret_cast<ImplicitProducerKVP*>(
            details::align_for<ImplicitProducerKVP>(
              raw + sizeof(ImplicitProducerHash)
            )
          );
          for (size_t i = 0; i != newCapacity; ++i) {
            new (newHash->entries + i) ImplicitProducerKVP;
            newHash->entries[i].key.store(
              details::invalid_thread_id, std::memory_order_relaxed
            );
          }
          newHash->prev = mainHash;
          implicitProducerHash.store(newHash, std::memory_order_release);
          implicitProducerHashResizeInProgress.clear(std::memory_order_release);
          mainHash = newHash;
        } else {
          implicitProducerHashResizeInProgress.clear(std::memory_order_release);
        }
      }

      // If it's < three-quarters full, add to the old one anyway so that we
      // don't have to wait for the next table to finish being allocated by
      // another thread (and if we just finished allocating above, the condition
      // will always be true)
      if (newCount < (mainHash->capacity >> 1) + (mainHash->capacity >> 2)) {
        auto producer =
          static_cast<ImplicitProducer*>(recycle_or_create_producer());
        if (producer == nullptr) {
          implicitProducerHashCount.fetch_sub(1, std::memory_order_relaxed);
          return nullptr;
        }

        producer->threadExitListener.callback =
          &ConcurrentQueue::implicit_producer_thread_exited_callback;
        producer->threadExitListener.userData = producer;
        details::ThreadExitNotifier::subscribe(&producer->threadExitListener);

        auto index = hashedId;
        while (true) {
          index &= mainHash->capacity - 1u;
          auto empty = details::invalid_thread_id;
          auto reusable = details::invalid_thread_id2;
          if (mainHash->entries[index].key.compare_exchange_strong(
                reusable, id, std::memory_order_seq_cst,
                std::memory_order_relaxed
              )) {
            implicitProducerHashCount.fetch_sub(
              1, std::memory_order_relaxed
            ); // already counted as a used slot
            mainHash->entries[index].value = producer;
            break;
          }
          if (mainHash->entries[index].key.compare_exchange_strong(
                empty, id, std::memory_order_seq_cst, std::memory_order_relaxed
              )) {
            mainHash->entries[index].value = producer;
            break;
          }
          ++index;
        }
        return producer;
      }

      // Hmm, the old hash is quite full and somebody else is busy allocating a
      // new one. We need to wait for the allocating thread to finish (if it
      // succeeds, we add, if not, we try to allocate ourselves).
      mainHash = implicitProducerHash.load(std::memory_order_acquire);
    }
  }

  void implicit_producer_thread_exited(ImplicitProducer* producer) {
    // Remove from hash
#ifdef MCDBGQ_NOLOCKFREE_IMPLICITPRODHASH
    debug::DebugLock lock(implicitProdMutex);
#endif
    auto hash = implicitProducerHash.load(std::memory_order_acquire);
    assert(hash != nullptr); // The thread exit listener is only registered if
                             // we were added to a hash in the first place
    auto id = details::thread_id();
    auto hashedId = details::hash_thread_id(id);
    details::thread_id_t probedKey;

    // We need to traverse all the hashes just in case other threads aren't on
    // the current one yet and are trying to add an entry thinking there's a
    // free slot (because they reused a producer)
    for (; hash != nullptr; hash = hash->prev) {
      auto index = hashedId;
      do {
        index &= hash->capacity - 1u;
        probedKey = id;
        if (hash->entries[index].key.compare_exchange_strong(
              probedKey, details::invalid_thread_id2, std::memory_order_seq_cst,
              std::memory_order_relaxed
            )) {
          break;
        }
        ++index;
      } while (probedKey != details::invalid_thread_id
      ); // Can happen if the hash has
         // changed but we weren't put back
         // in it yet, or if we weren't added
         // to this hash in the first place
    }

    // Mark the queue as being recyclable
    producer->inactive.store(true, std::memory_order_release);
  }

  static void implicit_producer_thread_exited_callback(void* userData) {
    auto producer = static_cast<ImplicitProducer*>(userData);
    auto queue = producer->parent;
    queue->implicit_producer_thread_exited(producer);
  }

  //////////////////////////////////
  // Utility functions
  //////////////////////////////////

  template <typename TAlign> static inline void* aligned_malloc(size_t size) {
    if constexpr (std::alignment_of<TAlign>::value <=
                  std::alignment_of<details::max_align_t>::value)
      return (Traits::malloc)(size);
    else {
      size_t alignment = std::alignment_of<TAlign>::value;
      void* raw = (Traits::malloc)(size + alignment - 1 + sizeof(void*));
      if (!raw)
        return nullptr;
      char* ptr = details::align_for<TAlign>(
        reinterpret_cast<char*>(raw) + sizeof(void*)
      );
      *(reinterpret_cast<void**>(ptr) - 1) = raw;
      return ptr;
    }
  }

  template <typename TAlign> static inline void aligned_free(void* ptr) {
    if constexpr (std::alignment_of<TAlign>::value <=
                  std::alignment_of<details::max_align_t>::value)
      return (Traits::free)(ptr);
    else
      (Traits::free)(ptr ? *(reinterpret_cast<void**>(ptr) - 1) : nullptr);
  }

  template <typename U> static inline U* create_array(size_t count) {
    assert(count > 0);
    U* p = static_cast<U*>(aligned_malloc<U>(sizeof(U) * count));
    if (p == nullptr)
      return nullptr;

    for (size_t i = 0; i != count; ++i)
      new (p + i) U();
    return p;
  }

  template <typename U> static inline void destroy_array(U* p, size_t count) {
    if (p != nullptr) {
      assert(count > 0);
      for (size_t i = count; i != 0;)
        (p + --i)->~U();
    }
    aligned_free<U>(p);
  }

  template <typename U> static inline U* create() {
    void* p = aligned_malloc<U>(sizeof(U));
    return p != nullptr ? new (p) U : nullptr;
  }

  template <typename U, typename A1> static inline U* create(A1&& a1) {
    void* p = aligned_malloc<U>(sizeof(U));
    return p != nullptr ? new (p) U(static_cast<A1&&>(a1)) : nullptr;
  }

  template <typename U> static inline void destroy(U* p) {
    if (p != nullptr)
      p->~U();
    aligned_free<U>(p);
  }

public:
  // this is not used by all classes, only by ex_cpu. so it is not managed by
  // this' destructor, but by init() / teardown() of ex_cpu
  // array of deqeueueProducerCount * ex_cpu::PRIORITY_COUNT elements
  ExplicitProducer* staticProducers;
  // Element 1 is a duplicate, thus this is the count of staticProducers + 1
  size_t dequeueProducerCount = 0;

private:
  std::atomic<ProducerBase*> producerListTail;
  std::atomic<std::uint32_t> producerCount;

  std::atomic<size_t> initialBlockPoolIndex;
  Block* initialBlockPool;
  size_t initialBlockPoolSize;

#ifndef MCDBGQ_USEDEBUGFREELIST
  FreeList<Block> freeList;
#else
  debug::DebugFreeList<Block> freeList;
#endif

  std::atomic<ImplicitProducerHash*> implicitProducerHash;
  std::atomic<size_t>
    implicitProducerHashCount; // Number of slots logically used
  ImplicitProducerHash initialImplicitProducerHash;
  std::array<ImplicitProducerKVP, INITIAL_IMPLICIT_PRODUCER_HASH_SIZE>
    initialImplicitProducerHashEntries;
  std::atomic_flag implicitProducerHashResizeInProgress;

  std::atomic<std::uint32_t> nextExplicitConsumerId;
  std::atomic<std::uint32_t> globalExplicitConsumerOffset;

#ifdef MCDBGQ_NOLOCKFREE_IMPLICITPRODHASH
  debug::DebugMutex implicitProdMutex;
#endif

#ifdef MOODYCAMEL_QUEUE_INTERNAL_DEBUG
  std::atomic<ExplicitProducer*> explicitProducers;
  std::atomic<ImplicitProducer*> implicitProducers;
#endif
};

} // namespace tmc::queue

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#if defined(__GNUC__) && !defined(__INTEL_COMPILER)
#pragma GCC diagnostic pop
#endif
