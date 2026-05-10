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
  // The return type of a single task would be `int`.
  // Here, we run 2 tasks in parallel and retrieve the results in a `std::tuple<int, int>`.
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
TooManyCooks is a header-only library. Just add `/include` to your include path.
Then `#include "tmc/all_headers.hpp"` for the entire library, or choose specific headers that you need.

This repo offers a CMakeLists.txt with an INTERFACE target that adds this include directory, which can be consumed like so:

```
add_subdirectory(${TooManyCooks_DIR} CONFIG REQUIRED)
target_link_libraries(main PRIVATE TooManyCooks::TooManyCooks)
```

TMC offers some compile-time configuration options via preprocessor definitions ([documented here](https://www.fleetcode.com/oss/tmc/docs/dev/build_flags.html)). The CMake target exposes CMake options that [map directly to these preprocessor definitions](https://github.com/tzcnt/TooManyCooks/blob/main/cmake/tmc-options.cmake) and ensures they are propagated to all library consumers.

The use of CMake is not required; the library can also be configured by setting the preprocessor definitions manually. If done this way, you must set these definitions identically across all files that consume any TMC headers.

For a minimal consumer project template, see [tmc-hello-world](https://github.com/tzcnt/tmc-hello-world).

### Optional Dependencies
- [Portable Hardware Locality Library (hwloc)](https://www.open-mpi.org/projects/hwloc/)
  - Defaults to required when using the CMake target. You can disable this dependency by setting the CMake option `TMC_USE_HWLOC=OFF`.
  - On Linux or MacOS, you can install this from your system package manager.
  - On Windows, the examples repo demonstrates how to [link](https://github.com/tzcnt/tmc-examples/blob/f1d0dbeae41501136e5ac2d0fc3f2338c4beadc9/CMakeLists.txt#L49) and [package](https://github.com/tzcnt/tmc-examples/blob/f1d0dbeae41501136e5ac2d0fc3f2338c4beadc9/CMakeLists.txt#L217) this with your application.
- Asio or Boost.Asio
  - Required if you want to use any of the functionality in the `include/tmc/asio/` folder.
  - These are header-only libraries, so you just need to add them to your include path when building your application.
  - The default is to use standalone Asio. Set `TMC_USE_BOOST_ASIO=ON` to use Boost.Asio.

### Package Manager Support

#### Conan
`conanfile.py` is provided in this repo which exposes all of the configuration options and resolves the optional dependencies.
Export this repository's Conan recipe into your local Conan package cache from the repository root:
```sh
conan export .
```
Then, from a consumer project's `conanfile.py`, add the project and configure any desired options:
```python
requires = "toomanycooks/1.5.0"

default_options = {
    "toomanycooks/*:trivial_task": True,
    "toomanycooks/*:nodiscard_await": True,
}
```

#### vcpkg
TooManyCooks can be consumed with vcpkg manifest mode from this repository's overlay port.
In the consumer repository, add a `vcpkg-configuration.json` that points to the overlay port:
```json
{
  "overlay-ports": [
    "submodules/TooManyCooks/ports"
  ]
}
```
Then add a `vcpkg.json` dependency with any desired features, e.g. TMC_TRIVIAL_TASK and TMC_NODISCARD_AWAIT:
```json
{
  "dependencies": [
    {
      "name": "toomanycooks",
      "features": [
        "trivial-task",
        "nodiscard-await"
      ]
    }
  ]
}
```

See the vcpkg [usage](https://github.com/tzcnt/TooManyCooks/blob/main/ports/toomanycooks/usage) file for more info.

### Roadmap
See the [issues tagged "enhancement"](https://github.com/tzcnt/TooManyCooks/issues?q=is%3Aissue%20state%3Aopen%20label%3Aenhancement) for future planned work. Please leave a :thumbsup: on any issues that are important to you. I will use this as a way to gauge community interest on what should be developed next.

### Supported Compilers
GCC and Clang are supported, but Clang is the recommended compiler, as it has the best coroutine codegen and the most functional HALO implementation.

Linux:
- Clang 17 or newer
- GCC 14 or newer

Windows:
- Clang 17 or newer (via clang-cl.exe)

MSVC is not supported due to [this compiler bug](https://developercommunity.visualstudio.com/t/MSVC-incorrectly-caches-thread_local-var/11041371) which causes a critical miscompilation. MSVC builds may work in Debug mode but crash on Release builds.

MacOS:
- Apple Clang based on Clang 17 or newer with `-fexperimental-library`

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
