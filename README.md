![x64-linux-gcc](https://github.com/tzcnt/TooManyCooks/actions/workflows/x64-linux-gcc.yml/badge.svg) ![x64-linux-clang](https://github.com/tzcnt/TooManyCooks/actions/workflows/x64-linux-clang.yml/badge.svg) ![x64-windows-clang-cl](https://github.com/tzcnt/TooManyCooks/actions/workflows/x64-windows-clang-cl.yml/badge.svg) ![arm64-macos-clang](https://github.com/tzcnt/TooManyCooks/actions/workflows/arm64-macos-clang.yml/badge.svg)

![AddressSanitizer](https://github.com/tzcnt/TooManyCooks/actions/workflows/x64-linux-clang-asan.yml/badge.svg) ![ThreadSanitizer](https://github.com/tzcnt/TooManyCooks/actions/workflows/x64-linux-clang-tsan.yml/badge.svg) ![UndefinedBehaviorSanitizer](https://github.com/tzcnt/TooManyCooks/actions/workflows/x64-linux-clang-ubsan.yml/badge.svg) [![codecov](https://codecov.io/gh/tzcnt/TooManyCooks/graph/badge.svg?token=UJ7XFJ72VK)](https://codecov.io/gh/tzcnt/TooManyCooks)

## TooManyCooks
TooManyCooks is a runtime and concurrency library for C++20 coroutines. Its goals:
- be the fastest general-purpose coroutine library available (see the :chart_with_upwards_trend: [benchmarks](https://github.com/tzcnt/runtime-benchmarks))
- clean API with minimal noise
- extensive feature set
- simple and clear path to migrate legacy applications
- simple and clear path to integrate with 3rd-party executors/event loops

It provides:
- a blazing fast lock-free work-stealing thread pool (`ex_cpu`) that supports both coroutines and regular functors
- automatic, hardware-optimized thread configuration via [hwloc](https://www.open-mpi.org/projects/hwloc/)
- network I/O, file I/O, and timers support by integration with Asio (via :octocat: [tmc-asio](https://github.com/tzcnt/tmc-asio))
- support for multiple task priority levels
- a suite of utility functions for fluently interacting with tasks, awaitables, and executors
- a suite of async data and control structures
- a global executor instance so you can submit work from anywhere
- traits-based extensibility for 3rd party awaitables and executors

### Quick Links
| :page_facing_up: [Documentation](https://fleetcode.com/oss/tmc/docs) | :bulb: [Examples](https://github.com/tzcnt/tmc-examples) | :chart_with_upwards_trend: [Benchmarks](https://github.com/tzcnt/runtime-benchmarks) | :white_check_mark: [Tests](https://github.com/tzcnt/tmc-examples/tree/main/tests) |
|---|---|---|---|


### Building
TooManyCooks is a header-only library. Adding it to your project is simple:
1. Download the library and add `/include` to your include path.
2. Add `#define TMC_IMPL` and `#include "tmc/all_headers.hpp"` to exactly one file in your project.

For a minimal project template, see :octocat: [tmc-hello-world](https://github.com/tzcnt/tmc-hello-world).

### Quick Start

```cpp
#define TMC_IMPL
#include "tmc/all_headers.hpp"
int main() {
  return tmc::async_main([]() -> tmc::task<int> {
    // Hello, async world!
    co_return 0;
  }());
}
```

For more info, check out the :bulb: [examples](https://github.com/tzcnt/tmc-examples) or dive into the :page_facing_up: [documentation](https://fleetcode.com/oss/tmc/docs).

### Configuration
TooManyCooks will work out of the box as a header-only library without any configuration.
However, some performance tuning options are available. See the documentation section [Build-Time Options](https://fleetcode.com/oss/tmc/docs/latest/build_flags.html) for more info.

### Release Strategy
Stable / LTS releases offer a stable API and continue to receive bugfixes for an extended period of time. The latest LTS release is [v1.0.0](https://github.com/tzcnt/TooManyCooks/releases/tag/v1.0.0).

Unstable releases get the latest and greatest performance and feature enhancements. The latest unstable release is [v1.2.0](https://github.com/tzcnt/TooManyCooks/releases/tag/v1.2.0).

### Roadmap
See the [issues tagged "enhancement"](https://github.com/tzcnt/TooManyCooks/issues?q=is%3Aissue%20state%3Aopen%20label%3Aenhancement) for future planned work. Please leave a :thumbsup: on any issues that are important to you. I will use this as a way to gauge community interest on what should be developed next.

### Supported Compilers
Linux:
- Clang 17 or newer
- GCC 14 or newer

Windows:
- Clang 17 or newer (via clang-cl.exe)
- MSVC Build Tools v145 (Visual Studio 2026 Insiders) or newer (older versions are affected by [this bug](https://developercommunity.visualstudio.com/t/Incorrect-code-generation-for-symmetric/1659260?scope=follow&viewtype=all))

MacOS:
- Apple Clang based on Clang 17 or newer with -fexperimental-library

### Supported Hardware
- x86 (32- or 64-bit)
- AArch64

TooManyCooks has been tested on the following physical devices:
- Intel i7 4770k
- AMD Ryzen 5950X
- AMD EPYC 7742
- Apple M2
- Rockchip RK3588S (in a Khadas Edge2)
