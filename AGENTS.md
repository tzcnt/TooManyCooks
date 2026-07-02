# TooManyCooks — agent guide

TooManyCooks (TMC) is a header-only C++20 coroutine runtime and concurrency
library. Everything lives in the `tmc::` namespace. Include `"tmc/all_headers.hpp"` for the whole library, or the individual headers you need (e.g. `"tmc/spawn.hpp"`).

## Core Concepts
- `tmc::task` is the library coroutine type. It is a cold coroutine (initial_suspend = suspend_never).
- Tasks run on executors and have a priority level. The current executor/priority are tracked via thread-local variables.
- Child / spawned tasks inherit their parents executor/priority by default. If the child's executor/priority are customized, those customizations are transitive to the child's children. The inheritance flows downward only — parents are not affected by changes to their children's executor/priority. In general, whenever a task awaits an awaitable, it always resumes back on its original executor/priority afterward.

## Awaitable Usage Rules
- Most `tmc::` awaitables are single-use and must be `co_await`ed as a temporary or explicitly cast to rvalue before `co_await`ing. Awaitables that are designed to be awaited multiple times (e.g. `tmc::mutex`) must be awaited as an lvalue. Using the wrong value category for an awaitable will cause a compilation error.
- Rvalue awaitables (including `tmc::task`) are linear types and *must* be awaited if not explicitly `detach()`ed. Constructing an awaitable that is not awaited will cause a leak.
- Forked awaitables hold a pointer back to the awaitable object returned by `fork()`. They must be joined by `co_await` on that object before it goes out of scope. Failure to do so will cause a use-after-free. Usually this means they must be awaited within within the coroutine where they were created.  An escape hatch for this is to pass a persistent awaitable object (`fork_group` or `mux`) with an external lifetime by reference into a coroutine. You can `fork()` into that object and then return, as long as it is eventually awaited before it goes out of scope. You can also use this to `fork()` tasks from a function that is not a coroutine - only the eventual awaiter must be a coroutine.

## Footgun: capturing coroutine lambdas

**Rule: a lambda whose body is a coroutine (uses `co_await`/`co_return`) must not
capture anything it needs at run time unless the closure object is guaranteed to
outlive the task.**

A capturing lambda that is itself a coroutine stores its captures in the *closure
object*. The coroutine frame keeps only a `this` pointer back to that closure. If
the closure dies before the task runs, the task reads freed memory — a
use-after-free that often presents as a flaky hang, garbage result, or assert.

Invoking such a lambda produces a **temporary closure** whose lifetime ends at the
end of the full-expression. So whether it is safe depends entirely on *when the
task runs relative to that closure temporary*.

### Safe

```cpp
int x = 42;

// 1. Invoke AND await in the same full-expression. The closure temporary lives
//    to the end of the statement, which covers the whole await.
co_await tmc::spawn([&x]() -> tmc::task<int> { co_return x; }());

// 2. Non-capturing coroutine; pass references as PARAMETERS. Parameters are
//    stored in the coroutine frame, so this is safe even when deferred.
auto t = [](int& x) -> tmc::task<int> { co_return x; }(x);
// ... arbitrary code ...
co_await tmc::spawn(std::move(t));                 // OK

// 3. Name the closure as an lvalue first, so it outlives the produced task.
auto fn = [&x]() -> tmc::task<int> { co_return x; };
auto t2 = fn();
co_await tmc::spawn(std::move(t2));                // OK — fn still alive
```

### Unsafe

```cpp
int x = 42;

// The closure is a TEMPORARY, destroyed at the `;`. The task is deferred.
auto t = [&x]() -> tmc::task<int> { co_return x; }();
// ... `t`'s closure is already gone here ...
co_await tmc::spawn(std::move(t));                 // UAF: reads x via dangling `this`
```

The same trap applies whenever the produced task is **deferred** — stored in a
variable, `std::array`/`std::vector`, or built into a `spawn_many` / `spawn_tuple`
/ `mux_*` group that is awaited/`fork()`ed in a *later* statement. Prefer form 2
(pass refs as parameters) for anything that isn't awaited on the spot.

## Awaitable Wrappers

These can all wrap one or more awaitables to provide custom dispatch behavior. They can all be customized before initiation (see next section).

| Awaitable | Use when |
|---|---|
| `spawn` | Customize a single awaitable. |
| `spawn_many` | Run N awaitables of the **same** type; results in a `std::array`/`std::vector`. |
| `spawn_tuple` | Run several awaitables of **different** types; results in a `std::tuple`. |
| `spawn_func` | Wrap a plain function/functor (not a coroutine) as a task. |
| `spawn_func_many` | Run N plain functors. |
| `spawn_group` | Imperatively build a group, then initiate all on `co_await` (lazy). Movable — can be returned/passed around. |
| `fork_group` | Imperatively build a group, initiating **each awaitable immediately** (eager); join later. Not movable. |
| `mux_many` | Homogeneous multiplexer: results delivered **as each becomes ready**; `co_await` returns the index of one ready slot. Slots are reusable - you can re-arm a new awaitable into a slot after its result has been consumed. Up to 63 slots (31 on 32-bit). |
| `mux_tuple` | Heterogeneous multiplexer: same as `mux_many` but slots may have different result types. |
| `select` | Await several awaitables, return the result of the **first** to complete, and cancel the rest. |

`spawn_many` / `spawn_func_many` take iterators of tasks. A simple, lightweight tool to transform a sequence into an iterator these will accept is `tmc::iter_adapter` from `tmc/utils.hpp`. For more complex transformations, use the <ranges> library - but if it is not used elsewhere in the project, you should consult the user first, as <ranges> is a heavyweight include. If the user rejects the use of <ranges>, `spawn_group` / `fork_group` imperative construction avoids iterators entirely.

### Customizing and initiating

Most awaitable wrappers above expose fluent customizers (return `*this`, chainable). Some awaitables also provide them directly:

- `.run_on(executor)` — where the work runs.
- `.resume_on(executor)` — where the parent resumes afterward.
- `.with_priority(p)` — priority level for the work.

After customizing, initiate the work with **exactly one** of:

- `co_await` — run and await the result inline.
- `.fork()` — start eagerly now, await the returned handle later.
- `.detach()` — fire-and-forget; no result is retrieved.

### HALO (`*_clang()` variants) — Clang only

TMC provides HALO (Heap Allocation Elision Optimization) entry points that use Clang-specific
attributes to elide the child coroutine's frame allocation: `tmc::spawn_clang`,
`tmc::fork_clang`, `tmc::fork_tuple_clang`, `mux.fork_clang(...)`,
`sg.add_clang(...)`, `fg.fork_clang(...)`. Each **must be `co_await`ed
immediately** in the same statement for elision to fire. The `fork_clang` functions
return a dummy awaitable that you must await immediately for elision. The return
value of that `co_await` expression is the real forked task handle, which you await
later to join the forked task.

**Before suggesting a `*_clang()` API, confirm the user's build environment
actually targets Clang** (compiler flags, CMake toolchain, CI config).
On other compilers they offer no benefit — use the plain variants.

## Control structures

Async equivalents of familiar primitives; behavior matches the name:

`mutex`, `semaphore`, `rw_lock`, `barrier`, `latch`, `manual_reset_event`,
`auto_reset_event`, `atomic_condvar`.

## Queues

- `qu_spsc_bounded`, `qu_spsc_unbounded` — single-producer, single-consumer.
- `qu_mpsc_bounded`, `qu_mpsc_unbounded` — multi-producer, single-consumer.
- `channel` — MPMC. Create with `tmc::make_channel<T>()`, which returns a
  `chan_tok` — a hazard-pointer + shared-ownership handle to the channel. Access
  the channel through the `chan_tok`; copy it (`new_token()`) to hand additional
  producers/consumers their own token.
