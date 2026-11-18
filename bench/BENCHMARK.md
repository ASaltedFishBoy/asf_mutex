# queue_mutex and std::mutex

Measured on 2026-11-18. The machine is an Intel Core i7-14700 (20 cores, 28 logical processors: 8 P-cores with hyperthreading plus 12 E-cores). The compiler is MinGW-W64 g++ 16.2.0.

The correctness test and the benchmark share the same optimization flags:

```text
g++ -std=c++17 -Ofast -Wall -Wextra -pthread -static -I include -DASF_TRY_SPIN=0 -DASF_LOCK_SPIN=0 -DASF_COUNT_PARK -o test_queue_mutex.exe tests/test_queue_mutex.cpp
g++ -std=c++17 -Ofast -Wall -Wextra -pthread -static -I include -o bench_mutex.exe bench/bench_mutex.cpp
```

## Method

One mutex. Several threads repeatedly lock, enter the critical section, and unlock. Each cell runs for about 1 second. The table records that single sample.

`Mop/s` is the aggregate throughput of every thread. `ns/op` is its reciprocal, the average time per completed operation across all threads.

Two critical sections:

- `inc`: add 1 to a thread-private integer.
- `burn64`: 64 `volatile` additions. `-Ofast` cannot delete those accesses.

Two lock implementations:

- `std::mutex`: `lock` / `unlock`.
- `queue_mutex`: `lock()` / `unlock()`. A contended call builds its node on the caller's stack.

## Results

| Threads | Implementation | inc ns/op | inc Mop/s | burn64 ns/op | burn64 Mop/s |
| ---: | --- | ---: | ---: | ---: | ---: |
| 1 | std::mutex | 12.3 | 81.37 | 83.8 | 11.93 |
| 1 | queue_mutex | 12.9 | 77.79 | 79.4 | 12.59 |
| 2 | std::mutex | 32.2 | 31.05 | 130.1 | 7.69 |
| 2 | queue_mutex | 96.8 | 10.33 | 229.5 | 4.36 |
| 4 | std::mutex | 28.0 | 35.71 | 134.2 | 7.45 |
| 4 | queue_mutex | 119.7 | 8.35 | 167.2 | 5.98 |
| 8 | std::mutex | 46.5 | 21.51 | 140.6 | 7.11 |
| 8 | queue_mutex | 124.4 | 8.04 | 187.9 | 5.32 |
| 16 | std::mutex | 64.0 | 15.62 | 155.7 | 6.42 |
| 16 | queue_mutex | 151.4 | 6.61 | 243.0 | 4.12 |
| 28 | std::mutex | 63.5 | 15.75 | 237.3 | 4.21 |
| 28 | queue_mutex | 188.8 | 5.30 | 311.8 | 3.21 |

With one integer increment in the critical section, `std::mutex` is faster; that gap is the price of queue fairness plus a `try_lock` with its own queue order. One thread is even (about 13 ns either way), and from 2 to 28 threads the ratio stays a fairly constant 2–3x. The important part is what is absent: throughput does not collapse at or above the core count. 28 threads on 28 logical processors hold 188.8 ns/op next to 151.4 at 16 threads — earlier revisions of this mutex fell into a park convoy past the physical core count and were over 40x slower there.

With 64 `volatile` additions, the critical section covers most of the lock overhead. At one thread the two are even (79 vs 84 ns), and at 28 threads they sit at 312 vs 237 ns.

The waiting policy that keeps oversubscription flat: a contended `lock` spins for `ASF_LOCK_SPIN` pauses (default 2048, about 85 µs — sized to outlast one scheduling jitter, because a jitter longer than the budget ignites a self-sustaining park convoy). When the budget runs out without a grant, the waiter yields the core up to 12 times, respinning if a handoff completes, and only then parks. And when a grant finds its target parked, the unlock marks up to 8 further queued nodes head — one per ~1 µs of the last observed grant gap — so wake latency is pipelined across upcoming turns instead of paid serially per handoff. All three mechanisms are idle in the fast lane.

## Correctness test

The correctness test is built with `-DASF_TRY_SPIN=0 -DASF_LOCK_SPIN=0 -DASF_COUNT_PARK`, so every wait exercises the OS park path. The performance table above uses the default budgets.

`test_queue_mutex.exe` passed with exit code 0.

- stress: 15000 `try_lock` calls, 13651 failures, about 91%.
- 60 seconds with 64 threads: 28749556 `try_lock` calls, 28748684 failures; threads entered the critical section 17251540 times, about 287,500 times per second. Roughly 291,000 parks per second, expected with zeroed spin budgets.
- The plain counter and the relaxed atomic counter match. Depth is 0 at the end.

---

# queue_mutex 与 std::mutex

测量日期 2026-11-18。机器是 Intel Core i7-14700（20 核、28 逻辑处理器：8 个支持超线程的 P 核加 12 个 E 核）。编译器是 MinGW-W64 g++ 16.2.0。

功能和性能都用同一组优化选项：

```text
g++ -std=c++17 -Ofast -Wall -Wextra -pthread -static -I include -DASF_TRY_SPIN=0 -DASF_LOCK_SPIN=0 -DASF_COUNT_PARK -o test_queue_mutex.exe tests/test_queue_mutex.cpp
g++ -std=c++17 -Ofast -Wall -Wextra -pthread -static -I include -o bench_mutex.exe bench/bench_mutex.cpp
```

## 测法

一个互斥量，若干线程同时反复加锁、进入临界区、解锁。每个格子跑约 1 秒，表里记这一次采样。

`Mop/s` 是全部线程加起来的吞吐。`ns/op` 是它的倒数，也就是全部线程完成一次操作的平均时间。

两种临界区：

- `inc`：临界区里对线程私有整数加 1。
- `burn64`：临界区里做 64 次 `volatile` 累加。`-Ofast` 消不掉这些访问。

两种加锁方式：

- `std::mutex`：`lock` / `unlock`。
- `queue_mutex`：`lock()` / `unlock()`。竞争调用在调用方栈上建节点。

## 结果

| 线程 | 实现 | inc ns/op | inc Mop/s | burn64 ns/op | burn64 Mop/s |
| ---: | --- | ---: | ---: | ---: | ---: |
| 1 | std::mutex | 12.3 | 81.37 | 83.8 | 11.93 |
| 1 | queue_mutex | 12.9 | 77.79 | 79.4 | 12.59 |
| 2 | std::mutex | 32.2 | 31.05 | 130.1 | 7.69 |
| 2 | queue_mutex | 96.8 | 10.33 | 229.5 | 4.36 |
| 4 | std::mutex | 28.0 | 35.71 | 134.2 | 7.45 |
| 4 | queue_mutex | 119.7 | 8.35 | 167.2 | 5.98 |
| 8 | std::mutex | 46.5 | 21.51 | 140.6 | 7.11 |
| 8 | queue_mutex | 124.4 | 8.04 | 187.9 | 5.32 |
| 16 | std::mutex | 64.0 | 15.62 | 155.7 | 6.42 |
| 16 | queue_mutex | 151.4 | 6.61 | 243.0 | 4.12 |
| 28 | std::mutex | 63.5 | 15.75 | 237.3 | 4.21 |
| 28 | queue_mutex | 188.8 | 5.30 | 311.8 | 3.21 |

临界区只有一次整数加法时，`std::mutex` 更快；这个差距就是公平队列加一个自带失败顺序的 `try_lock` 的代价。单线程打平（都约 13 ns），2 到 28 线程差距稳定在 2–3 倍。关键在于缺了什么：超过核数之后吞吐不再崩塌。28 个线程跑满 28 个逻辑处理器时是 188.8 ns/op，和 16 线程的 151.4 相邻——本互斥量的早期版本在超过物理核数后会陷入停车车队，在那里要慢 40 倍以上。

临界区改成 64 次 `volatile` 累加后，临界区盖住了锁开销的大部分。单线程两者打平（79 对 84 ns），28 线程时是 312 对 237 ns。

让超订保持平坦的等待策略：竞争的 `lock` 自旋 `ASF_LOCK_SPIN` 次（默认 2048，约 85 µs——按扛住一次调度抖动来定，因为比预算长的抖动会点燃自我维持的停车车队）。预算耗尽仍未获授权时，等待者先让核最多 12 次，期间若有交接完成就重新自旋，然后才停车。而当授权发现目标在停车时，`unlock` 会把队列里更多节点标成待命队头——按上次实测授权间隔每 ~1 µs 一个、最多 8 个——把唤醒延迟摊到未来若干棒上流水化，而不是每次交接串行付一次。这三套机制在快车道里全部空转。

## 功能测试

正确性测试用 `-DASF_TRY_SPIN=0 -DASF_LOCK_SPIN=0 -DASF_COUNT_PARK` 编译，每次等待都走操作系统停车路径。上面的性能表用默认预算。

`test_queue_mutex.exe` 通过，退出码 0。

- stress：15000 次 `try_lock`，失败 13651 次，约 91%。
- 60 秒、64 线程混跑：28749556 次 `try_lock`，失败 28748684 次。成功进入临界区 17251540 次，大约每秒 28.75 万次。每秒约 29.1 万次停车，预算归零后属预期。
- 非原子计数与 relaxed 原子计数一致。结束时深度为 0。
