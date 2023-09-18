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
- building blocks:
  - `task<result_t>` is TMC's native coroutine type
  - tasks can spawn child tasks, which may be:
    - awaited immediately
    - eagerly spawned, and lazily awaited
    - eagerly spawned, and not awaited (detached)
  - the prior submit/await operations can also all be done in bulk
  - `ex_braid` is an async mutex / serializing executor
  - `post_waitable()` for an external thread to wait (block) on thread pool work
- convenience functions:
  - `yield()` / `yield_if_requested()` to implement fiber-like cooperative multitasking
  - `resume_on()` to move the coroutine to a different executor, as either a free function or an awaitable customization
  - `async_main()` quickstart function

Integrations with other libraries:
- Asio (via [tmc-asio](https://github.com/tzcnt/tmc-asio)) - provides network I/O, file I/O, and timers

### Usage
TooManyCooks is a header-only library. You can either include the specific headers that you need in each file, or `#include "tmc/all_headers.hpp"`, which contains all of the other headers.

In order to reduce compile times, some files have separated declarations and definitions. The definitions will appear in whichever compilation unit defines `TMC_IMPL`. Since each function must be defined exactly once, you must include this definition in exactly one compilation unit. The simplest way to accomplish this is to put it in your `main.cpp`, and also to initialize the global executor there:
```cpp
#define TMC_IMPL
#include "tmc/all_headers.hpp"
int main() {
  tmc::cpu_executor().init();
  return tmc::async_main(()[] -> tmc::task<int> {
    // Hello, world!
    co_return 0;
  }());
}
```

### Examples
https://github.com/tzcnt/tmc-examples

In order to keep this repository bloat-free, the examples are in a separate repository. The examples CMake config will automatically download this, and other TMC ecosystem projects, as a dependency. 

### TODO
  - documentation
  - cancellation
  - simultaneously await multiple awaitables with different types
  - algorithms that depend on the prior 2 (select)

Planned integrations:
- CUDA ([tmc-cuda](https://github.com/tzcnt/tmc-cuda)) - a CUDA Graph can be made into an awaitable by adding a callback to the end of the graph with [cudaGraphAddHostNode](https://docs.nvidia.com/cuda/cuda-runtime-api/group__CUDART__GRAPH.html#group__CUDART__GRAPH_1g30e16d2715f09683f0aa8ac2b870cf71) which will resume the awaiting coroutine
- gRPC ([tmc-grpc](https://github.com/tzcnt/tmc-grpc)) - via the callback interface if it is sufficiently stable / well documented. otherwise via the completion queue thread
- blosc2 ([tmc-blosc2](https://github.com/tzcnt/tmc-blosc2)) - port to C++. use ex_asio + io_uring for file I/O, and ex_cpu to replace the inbuilt pthreads. break down operations into smaller vertical slices to exploit dynamic parallelism.
