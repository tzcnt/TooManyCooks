![x64-linux-gcc](https://github.com/tzcnt/TooManyCooks/actions/workflows/x64-linux-gcc.yml/badge.svg) ![x64-linux-clang](https://github.com/tzcnt/TooManyCooks/actions/workflows/x64-linux-clang.yml/badge.svg) ![x64-windows-clang-cl](https://github.com/tzcnt/TooManyCooks/actions/workflows/x64-windows-clang-cl.yml/badge.svg) ![arm64-macos-clang](https://github.com/tzcnt/TooManyCooks/actions/workflows/arm64-macos-clang.yml/badge.svg)

## TooManyCooks
TooManyCooks is a runtime for C++20 coroutines. Its objectives are:
- seamless intermingling of cpu-bound, i/o bound, and heterogeneous (GPU/TPU/NPU/etc...) execution in the same code path
- maximum performance
- minimum boilerplate and clean interface
- simple upgrade path for existing libraries

It provides:
- a blazing fast lock-free work-stealing thread pool (`ex_cpu`) that supports both coroutines and regular functors
- automatic, hardware-optimized thread configuration via [hwloc](https://www.open-mpi.org/projects/hwloc/)
- a global executor instance so you can submit work from anywhere
- support for multiple priority levels
- network I/O, file I/O, and timers support by integration with Asio (via [tmc-asio](https://github.com/tzcnt/tmc-asio))
- a suite of utility functions for fluently interacting with tasks, awaitables, and executors

### Documentation
https://fleetcode.com/oss/tmc/docs

As this is library approaches its first major release, this documentation is still under active development... but it's nearly complete.

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
However, some performance tuning options are available. See the documentation section "Recommended Build Flags" for more info.

### TODO
  - finalize the documentation
  - CI automated tests across compilers and platforms
  - v0.1 release!

Planned integrations:
- CUDA ([tmc-cuda](https://github.com/tzcnt/tmc-cuda)) - a CUDA Graph can be made into an awaitable by adding a callback to the end of the graph with [cudaGraphAddHostNode](https://docs.nvidia.com/cuda/cuda-runtime-api/group__CUDART__GRAPH.html#group__CUDART__GRAPH_1g30e16d2715f09683f0aa8ac2b870cf71) which will resume the awaiting coroutine
- gRPC ([tmc-grpc](https://github.com/tzcnt/tmc-grpc)) - via the callback interface if it is sufficiently stable / well documented. otherwise via the completion queue thread
- blosc2 ([tmc-blosc2](https://github.com/tzcnt/tmc-blosc2)) - port to C++. use [tmc-asio](https://github.com/tzcnt/tmc-asio) + io_uring for file I/O, and ex_cpu to replace the inbuilt pthreads. break down operations into smaller vertical slices to exploit dynamic parallelism.

### Supported Compilers
Linux:
- Clang 17 or newer
- GCC 14 or newer

Windows:
- Clang 17 or newer (via clang-cl.exe)
- ~~MSVC 19.42.34436~~

MSVC has an open bug with symmetric transfer and final awaiters that destroy the coroutine frame. The code will compile but crashes at runtime. This bug has been open since 2022 and they just can't seem to fix it 🤔. ([bug link](https://developercommunity.visualstudio.com/t/Incorrect-code-generation-for-symmetric/1659260?scope=follow&viewtype=all))

### Supported Hardware
- x86_64 with support for POPCNT / TZCNT
- AArch64

TooManyCooks has been tested on the following physical devices:
- Intel i7 4770k
- AMD Ryzen 5950X
- AMD EPYC 7742
- Rockchip RK3588S (in a Khadas Edge2)

### Untested Hardware
TooManyCooks has not been tested on an M1+ Mac, or any Intel Hybrid (12th gen Core or newer) architecture - yet. I have recently purchased both of these machines, so this will be forthcoming.
