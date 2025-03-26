![x64-linux-gcc](https://github.com/tzcnt/TooManyCooks/actions/workflows/x64-linux-gcc.yml/badge.svg) ![x64-linux-clang](https://github.com/tzcnt/TooManyCooks/actions/workflows/x64-linux-clang.yml/badge.svg) ![x64-windows-clang-cl](https://github.com/tzcnt/TooManyCooks/actions/workflows/x64-windows-clang-cl.yml/badge.svg) ![arm64-macos-clang](https://github.com/tzcnt/TooManyCooks/actions/workflows/arm64-macos-clang.yml/badge.svg)

## TooManyCooks
TooManyCooks is a runtime and concurrency library for C++20 coroutines. Its goals:
- be the fastest general-purpose coroutine library available
- absolutely no-fuss API
- extensive feature set
- simple and clear path to migrate legacy applications
- simple and clear path to integrate with 3rd-party executors/event loops

It provides:
- a blazing fast lock-free work-stealing thread pool (`ex_cpu`) that supports both coroutines and regular functors
- automatic, hardware-optimized thread configuration via [hwloc](https://www.open-mpi.org/projects/hwloc/)
- network I/O, file I/O, and timers support by integration with Asio (via [tmc-asio](https://github.com/tzcnt/tmc-asio))
- a global executor instance so you can submit work from anywhere
- support for multiple priority levels
- a suite of utility functions for fluently interacting with tasks, awaitables, and executors
- traits-based extensibility for 3rd party integrations

### Documentation
https://fleetcode.com/oss/tmc/docs

### Examples
https://github.com/tzcnt/tmc-examples

In order to keep this repository bloat-free, the examples are in a separate repository. The examples CMake config will automatically download this, and other TMC ecosystem projects, as a dependency. 

### Usage
TooManyCooks is a header-only library. You can either include the specific headers that you need in each file, or `#include "tmc/all_headers.hpp"`, which contains all of the other headers.

In order to reduce compile times, some files have separated declarations and definitions. The definitions will appear in whichever compilation unit defines `TMC_IMPL`. Since each function must be defined exactly once, you must include this definition in exactly one compilation unit. The simplest way to accomplish this is to put it in your `main.cpp`:
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
- async barrier / condvar / semaphore
- [[[clang::coro_await_elidable]]](https://github.com/llvm/llvm-project/pull/99282) / [[[clang::coro_await_elidable_argument]]](https://github.com/llvm/llvm-project/pull/108474)
- compilation time improvements
- performance tuning for:
  - hybrid architectures (Apple M / Intel Hybrid Core)
  - Docker
  - explicit NUMA / pre-fork worker model 

### Supported Compilers
Linux:
- Clang 17 or newer
- GCC 14 or newer

Windows:
- Clang 17 or newer (via clang-cl.exe)
- ~~MSVC 19.42.34436~~

MSVC has an open bug with symmetric transfer and final awaiters that destroy the coroutine frame. The code will compile but crashes at runtime. ([bug link](https://developercommunity.visualstudio.com/t/Incorrect-code-generation-for-symmetric/1659260?scope=follow&viewtype=all))

### Supported Hardware
- x86 (32- or 64-bit)
- AArch64

TooManyCooks has been tested on the following physical devices:
- Intel i7 4770k
- AMD Ryzen 5950X
- AMD EPYC 7742
- Rockchip RK3588S (in a Khadas Edge2)
