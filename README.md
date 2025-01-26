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
https://fleetcode.com/oss/tmc/docs/v0.1.0/

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
TooManyCooks supports the following configuration parameters, supplied as preprocessor definitions:
- `TMC_USE_HWLOC` (default unset) enables hwloc integration, allowing TMC to automatically create optimized thread layouts and work-stealing groups. This requires that you add the directory containing `hwloc.h` to your include path, and the `hwloc` library path to you your linker path. It is highly recommended to use this.
- `TMC_PRIORITY_COUNT=` (default unset) allows you to set the number of priority levels at compile-time, rather than at runtime. The main use case for this is to set the value to 1, which will remove all priority-specific code, making things slightly faster.
- `TMC_WORK_ITEM=` (default `CORO`) controls the type used to store work items in the work stealing queue. Any type can store both a coroutine or a functor, but the performance characteristics are different. There are 4 options:

| Value | Type | sizeof(type) | Comments |
| --- | --- | --- | --- |
| CORO | std::coroutine_handle<> | 8 | Functors will be wrapped in a coroutine trampoline. |
| FUNC | std::function<void()> | 32 | Coroutines will be stored inline using small buffer optimization. This has substantially worse performance than coro_functor when used for coroutines. |
| FUNCORO | tmc::coro_functor | 16 | Stores either a coroutine or a functor using pointer tagging. Does not support small-object optimization. Supports move-only functors, or references to functors. Typed deleter is implemented with a shim. |
| FUNCORO32 | tmc::coro_functor32 | 32 | Stores either a coroutine or a functor using pointer tagging. Does not support small-object optimization. Supports move-only functors, or references to functors. |

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

Clang 16 will compile TMC, and things mostly work; however, there a number of subtle coroutine code generation issues, such as https://github.com/llvm/llvm-project/issues/63022, which were only fixed in Clang 17.

MSVC on Windows currently compiles TMC, but crashes at runtime, likely due to [this code generation bug](https://developercommunity.visualstudio.com/t/Incorrect-code-generation-for-symmetric/1659260?scope=follow).

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
