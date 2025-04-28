![x64-linux-gcc](https://github.com/tzcnt/TooManyCooks/actions/workflows/x64-linux-gcc.yml/badge.svg) ![x64-linux-clang](https://github.com/tzcnt/TooManyCooks/actions/workflows/x64-linux-clang.yml/badge.svg) ![x64-windows-clang-cl](https://github.com/tzcnt/TooManyCooks/actions/workflows/x64-windows-clang-cl.yml/badge.svg) ![arm64-macos-clang](https://github.com/tzcnt/TooManyCooks/actions/workflows/arm64-macos-clang.yml/badge.svg)

![AddressSanitizer](https://github.com/tzcnt/TooManyCooks/actions/workflows/x64-linux-clang-asan.yml/badge.svg) ![ThreadSanitizer](https://github.com/tzcnt/TooManyCooks/actions/workflows/x64-linux-clang-tsan.yml/badge.svg) ![UndefinedBehaviorSanitizer](https://github.com/tzcnt/TooManyCooks/actions/workflows/x64-linux-clang-ubsan.yml/badge.svg) [![codecov](https://codecov.io/gh/tzcnt/TooManyCooks/graph/badge.svg?token=UJ7XFJ72VK)](https://codecov.io/gh/tzcnt/TooManyCooks)


## TooManyCooks
TooManyCooks is a runtime and concurrency library for C++20 coroutines. Its goals:
- be the fastest general-purpose coroutine library available ([see the benchmarks](https://github.com/tzcnt/runtime-benchmarks))
- absolutely no-fuss API
- extensive feature set
- simple and clear path to migrate legacy applications
- simple and clear path to integrate with 3rd-party executors/event loops

It provides:
- a blazing fast lock-free work-stealing thread pool (`ex_cpu`) that supports both coroutines and regular functors
- automatic, hardware-optimized thread configuration via [hwloc](https://www.open-mpi.org/projects/hwloc/)
- network I/O, file I/O, and timers support by integration with Asio (via [tmc-asio](https://github.com/tzcnt/tmc-asio))
- a global executor instance so you can submit work from anywhere
- support for multiple task priority levels
- a suite of utility functions for fluently interacting with tasks, awaitables, and executors
- traits-based extensibility for 3rd party awaitables and executors

### Read the [Documentation](https://fleetcode.com/oss/tmc/docs), try out the [Examples](https://github.com/tzcnt/tmc-examples), and run the [Tests](https://github.com/tzcnt/tmc-examples/tree/main/tests).

### Building
TooManyCooks is a header-only library. You can either include the specific headers that you need in each file, or `#include "tmc/all_headers.hpp"`, which contains all of the other headers.

In exactly one file, you must `#define TMC_IMPL` before including the headers. This will generate the function definitions for the library into that file. The simplest way to accomplish this is to put it in your `main.cpp`. Creating a [standalone compilation file](https://github.com/tzcnt/tmc-examples/blob/main/tests/standalone_compilation.cpp) is also trivial.

### Quick Start

```cpp
#define TMC_IMPL
#include "tmc/all_headers.hpp"
int main() {
  return tmc::async_main(()[] -> tmc::task<int> {
    // Hello, async world!
    co_return 0;
  }());
}
```

### Configuration
TooManyCooks will work out of the box as a header-only library without any configuration.
However, some performance tuning options are available. See the documentation section [Build-Time Options](https://fleetcode.com/oss/tmc/docs/v0.1.0/build_flags.html) for more info.

### Roadmap
See the [issues tagged "enhancement"](https://github.com/tzcnt/TooManyCooks/issues?q=is%3Aissue%20state%3Aopen%20label%3Aenhancement) for future planned work. Please leave a :thumbsup: on any issues that are important to you. I will use this as a way to gauge community interest on what should be developed next.

### Supported Compilers
Linux:
- Clang 17 or newer
- GCC 14 or newer

Windows:
- Clang 17 or newer (via clang-cl.exe)
- ~~MSVC 19.42.34436~~

MSVC has an open bug with symmetric transfer and final awaiters that destroy the coroutine frame. The code will compile, but crashes at runtime. ([bug link](https://developercommunity.visualstudio.com/t/Incorrect-code-generation-for-symmetric/1659260?scope=follow&viewtype=all))

### Supported Hardware
- x86 (32- or 64-bit)
- AArch64

TooManyCooks has been tested on the following physical devices:
- Intel i7 4770k
- AMD Ryzen 5950X
- AMD EPYC 7742
- Rockchip RK3588S (in a Khadas Edge2)
