On Linux a naive single futex (as used by ready_task_cv.wait()) tends
to wake threads in a round-robin fashion.
It would be better to wake more recently used threads instead or wake
neighbor and peer threads first.

The first hack implementation always wakes low bits of sleeping thread bitset.
This is highly unfair but it does improve ping-pong performance substantially (~30x)

For ping-pong it would be better to queue the waiter to one of:
- this thread's queue (and wake nothing)
- adjacent waiter
- whence it came

however, we also need to balance work across nodes when large amounts of
work are submitted

this dovetails with the private work - perhaps an MPSC queue per thread, to
use both for private work / as an inbox. Although it would want to be LIFO
on itself, FIFO from others.

can we start tracking thread id when invoking any awaitable, calculate a
heuristic across all awaitable types, perform a graph adjacency
optimization, and do explicit migrations so that linked tasks are nearer
each other? ...this seems like a future project.

heuristics on tasks sounds even harder.
I'd like to get a hash or ID per coroutine frame/promise type, and
sample the fork size / runtime of each type's task graph to tune for
optimal concurrency at runtime.

coroutine handle addresses can be used IF the coroutine is long-running
otherwise it might be a new coro in the same address. how badly can
a wrong heuristic affect the application?

atomic address could be used, but better to use a single entry for each
higher-level data structure (the queue's address instead of each atomic within)


### Benchmarks
queue - 2 prod, 10 cons, 10M elements, block size 4096
measurements in us. 10 runs each
single futex:
64 futexes, wake low bitset thread:
varies from 500k  to 7.3M. median 1.25M. 1 outlier (>5M)
64 futexes, wake neighbors in iteration order:
varies from 700k to 6.5M. median 1.10M. 4 outliers (>5M)
64 futexes, wake neighbors in neighbor order:
// TODO
64 futexes, wake neighbors in reverse neighbor order:
// TODO