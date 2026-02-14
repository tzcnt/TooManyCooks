![x64-linux-gcc](https://github.com/tzcnt/TooManyCooks/actions/workflows/x64-linux-gcc.yml/badge.svg) ![x64-linux-clang](https://github.com/tzcnt/TooManyCooks/actions/workflows/x64-linux-clang.yml/badge.svg) ![x64-windows-clang-cl](https://github.com/tzcnt/TooManyCooks/actions/workflows/x64-windows-clang-cl.yml/badge.svg) ![arm64-macos-clang](https://github.com/tzcnt/TooManyCooks/actions/workflows/arm64-macos-clang.yml/badge.svg)

![AddressSanitizer](https://github.com/tzcnt/TooManyCooks/actions/workflows/x64-linux-clang-asan.yml/badge.svg) ![ThreadSanitizer](https://github.com/tzcnt/TooManyCooks/actions/workflows/x64-linux-clang-tsan.yml/badge.svg) ![UndefinedBehaviorSanitizer](https://github.com/tzcnt/TooManyCooks/actions/workflows/x64-linux-clang-ubsan.yml/badge.svg) [![codecov](https://codecov.io/gh/tzcnt/TooManyCooks/graph/badge.svg?token=UJ7XFJ72VK)](https://codecov.io/gh/tzcnt/TooManyCooks)

## TooManyCooks
TooManyCooks is a runtime and concurrency library for C++20 coroutines. Its goals:
- be the fastest general-purpose coroutine library available (see the [benchmarks](https://github.com/tzcnt/runtime-benchmarks))
- clean API with minimal noise
- extensive feature set
- simple and clear path to migrate legacy applications
- simple and clear path to integrate with 3rd-party executors/event loops

It provides:
- a blazing fast, lock-free, work-stealing, continuation-stealing thread pool (`ex_cpu`)
- advanced hardware detection and thread configuration via [hwloc](https://www.open-mpi.org/projects/hwloc/)
- network I/O, file I/O, and timers support by integration with Asio (via [tmc-asio](https://github.com/tzcnt/TooManyCooks/tree/main/include/tmc/asio))
- support for multiple task priority levels
- support for both coroutines and regular functors in most APIs
- a suite of utility functions for fluently interacting with tasks, awaitables, and executors
- a suite of async data and control structures
- a global executor instance so you can submit work from anywhere
- traits-based extensibility for 3rd party awaitables and executors

### Quick Links
| :page_facing_up: [Documentation](https://fleetcode.com/oss/tmc/docs) | :bulb: [Examples](https://github.com/tzcnt/tmc-examples) | :chart_with_upwards_trend: [Benchmarks](https://github.com/tzcnt/runtime-benchmarks) | :white_check_mark: [Tests](https://github.com/tzcnt/tmc-examples/tree/main/tests) |
|---|---|---|---|

### A Brief Example
```cpp
// A complete implementation of the parallel recursive fibonacci benchmark
#define TMC_IMPL
#include "tmc/all_headers.hpp"
#include <iostream>

tmc::task<int> fib(int n) {
  if (n < 2) {
    co_return n;
  }
  // Fork 2 child tasks in parallel and await both results.
  // The return type of a single task would be just `int`.
  // Here, we retrieve both results together in a `std::tuple<int, int>`.
  auto [x, y] = co_await tmc::spawn_tuple(fib(n - 1), fib(n - 2));
  co_return x + y;
}

int main() {
  // Manually construct an executor and block on a root task.
  // `tmc::async_main()` could be used instead to simplify this.
  tmc::cpu_executor().init();

  // Synchronous (blocking) APIs return a std::future.
  int result = tmc::post_waitable(tmc::cpu_executor(), fib(30)).get();
  std::cout << result << std::endl;
}
```

### Building
TooManyCooks is a header-only library. You only need to add `/include` to your include path.
Adding `#include tmc/all_headers.hpp` gives access to the entire library, or you can choose specific headers that you need.

It also offers the option to create a [standalone compilation file](https://github.com/tzcnt/tmc-examples/blob/main/examples/standalone_compilation.cpp) to reduce redundant compilation times.

For a minimal project template, see [tmc-hello-world](https://github.com/tzcnt/tmc-hello-world).

Versions prior to v1.5 (the current dev version) require the creation of a standalone compilation file. For the latest stable release (v1.4) see the [older version of the README](https://github.com/tzcnt/TooManyCooks/tree/v1.4.0?tab=readme-ov-file#building) for build instructions.

### Configuration
TooManyCooks will work out of the box as a header-only library without any configuration.
However, some configuration options are available. See the documentation section [Build-Time Options](https://fleetcode.com/oss/tmc/docs/latest/build_flags.html) for more info.

### Roadmap
See the [issues tagged "enhancement"](https://github.com/tzcnt/TooManyCooks/issues?q=is%3Aissue%20state%3Aopen%20label%3Aenhancement) for future planned work. Please leave a :thumbsup: on any issues that are important to you. I will use this as a way to gauge community interest on what should be developed next.

### Supported Compilers
All 3 major compilers are fully supported, but Clang is the recommended compiler, as it has the best coroutine codegen and the most functional HALO implementation.

Linux:
- Clang 17 or newer
- GCC 14 or newer

Windows:
- Clang 17 or newer (via clang-cl.exe)

MSVC is not supported due to [this compiler bug](https://developercommunity.visualstudio.com/t/MSVC-incorrectly-caches-thread_local-var/11041371) which causes a critical miscompilation. MSVC builds may work in Debug mode but crash on Release builds.

MacOS:
- Apple Clang based on Clang 17 or newer with -fexperimental-library

### Supported Hardware
- x86 (32- or 64-bit)
- AArch64

TooManyCooks is regularly tested on the following physical devices:
- Intel i7 4770K
- Intel i5 13600K
- AMD Ryzen 5950X
- AMD EPYC 7742
- Apple M2
- Rockchip RK3588S (in a Khadas Edge2)
