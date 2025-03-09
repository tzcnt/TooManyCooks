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

Ideas:
Transition from working -> spinning earlier (as soon as this thread's queue is empty)
- How does this work with lower priority / this thread's queue - worry about this later
When spinning, skip looking for work in non-local queues that are associated with another spinning thread
- This will negatively affect high parallelism benchmarks
- Final (full) spin after marking self as sleeping
Consumer Autoscaling - Use a hook to create consumers dynamically as needed
- Need time-based heuristics in queue to determine when to do this
- Allow constructor to set a MaxConcurrency
- This could also take the form of a signal to be handled by the caller (error code SUGGEST_UPSCALE / SUGGEST_DOWNSCALE)
Update woken thread's last-consumed-from index
- Along with ThreadHint of prior consumer thread
Private Work Queues
- Push to ThreadHint from prior consumer thread
Wake waiting threads directly and have them check a separate queue from this queue?
- Threads wait on multiple futexes (futex_waitv)
Don't wake consumers in FIFO order. Wake in LIFO order instead.
- Separate waiting consumer stack from ready-items queue.
- This also effectively "downscales" the number of consumers.
- Probs not the issue - which is too many spinning threads causing migration on wakeups.
Push ready consumers into a queue and use a try_lock to wake them (similar to ex_braid).
- 1 producer can wake multiple consumers in a row while holding the lock
- ready-consumer-queue must be lock free pushable (MPSC)


When using a very high number of spins in the queue (consumers never suspend) w/ 2/10/64 and 1M elements:
Producers in same L3: 40ms
Producers in diff L3: 200ms

This comprises a lower bound on the performance / differential that can be achieved.
Perhaps Producer-Producer Locality is the most important metric to optimize?


### Benchmarks
queue - 2 prod, 10 cons, 10M elements, block size 4096
measurements in us. 10 runs each
single futex:
196M (1 run, did not wait for remaining runs to finish)
64 futexes, wake low bitset thread:
varies from 500k  to 7.3M. median 1.25M. 1 outlier (>5M)
64 futexes, wake neighbors in iteration order:
varies from 700k to 6.5M. median 1.10M. 4 outliers (>5M)
64 futexes, wake neighbors in neighbor order:
// TODO
64 futexes, wake neighbors in reverse neighbor order:
// TODO

64 futexes, only wake source thread (by hint); if already awake, do nothing:
varies from 1M to 1.25M, median 1.10M. No outliers
- but slightly slower on 32prod, 32cons either due to threads not waking or no flexibility resulting in cross-CCD sharing repeatedly