# queue_mutex and std::mutex

Measured on 2026-11-17. The machine is an Intel Core i7-14700 (20 cores, 28 logical processors). The compiler is MinGW-W64 g++ 16.2.0.

The correctness test and the benchmark share the same optimization flags:

```text
g++ -std=c++17 -Ofast -Wall -Wextra -pthread -static -I include -DASF_TRY_SPIN=0 -o test_queue_mutex.exe tests/test_queue_mutex.cpp
g++ -std=c++17 -Ofast -Wall -Wextra -pthread -static -I include -o bench_mutex.exe bench/bench_mutex.cpp
```

## Method

One mutex. Several threads repeatedly lock, enter the critical section, and unlock. Each cell runs for about 1 second. The table records that single sample.

`Mop/s` is the aggregate throughput of every thread. `ns/op` is its reciprocal, the average time per completed operation across all threads.

Two critical sections:

- `inc`: add 1 to a thread-private integer.
- `burn64`: 64 `volatile` additions. `-Ofast` cannot delete those accesses.

Three lock implementations:

- `std::mutex`: `lock` / `unlock`.
- `queue_mutex stack`: a stack `lock_node` on every operation, then `lock(node)` / `unlock(node)`.
- `queue_mutex tls`: `lock()` / `unlock()` with no node. The node lives in the thread-local slot, and each `lock()` constructs it in place.

## Results

| Threads | Implementation | inc ns/op | inc Mop/s | burn64 ns/op | burn64 Mop/s |
| ---: | --- | ---: | ---: | ---: | ---: |
| 1 | std::mutex | 13.1 | 76.43 | 84.2 | 11.87 |
| 1 | queue_mutex stack | 25.2 | 39.65 | 75.3 | 13.29 |
| 1 | queue_mutex tls | 32.2 | 31.09 | 76.0 | 13.16 |
| 2 | std::mutex | 30.2 | 33.14 | 151.6 | 6.60 |
| 2 | queue_mutex stack | 95.3 | 10.49 | 161.3 | 6.20 |
| 2 | queue_mutex tls | 89.8 | 11.14 | 163.1 | 6.13 |
| 4 | std::mutex | 29.5 | 33.86 | 134.0 | 7.46 |
| 4 | queue_mutex stack | 96.2 | 10.40 | 175.5 | 5.70 |
| 4 | queue_mutex tls | 97.4 | 10.27 | 169.0 | 5.92 |
| 8 | std::mutex | 48.6 | 20.58 | 162.1 | 6.17 |
| 8 | queue_mutex stack | 107.1 | 9.33 | 195.3 | 5.12 |
| 8 | queue_mutex tls | 113.3 | 8.83 | 197.5 | 5.06 |
| 16 | std::mutex | 62.7 | 15.94 | 203.2 | 4.92 |
| 16 | queue_mutex stack | 123.7 | 8.08 | 199.7 | 5.01 |
| 16 | queue_mutex tls | 125.5 | 7.97 | 201.6 | 4.96 |

With one integer increment in the critical section, `std::mutex` is faster. One thread takes about 13 ns. The stack-node interface takes about 25 ns, and the TLS interface about 32 ns. At 16 threads the standard library takes about 63 ns, and this mutex about 124–126 ns.

With 64 `volatile` additions, the critical section covers most of the lock overhead. At one thread the stack-node interface takes 75 ns and the standard library 84 ns. At 16 threads both sit around 200 ns.

## Correctness test

The correctness test is built with `-DASF_TRY_SPIN=0`. The critical section and the gap between operations are each one `volatile` increment, a few nanoseconds. The performance table above keeps the default budget of 256 spins.

`test_queue_mutex.exe` passed with exit code 0.

- stress: 15000 `try_lock` calls, 7880 failures, about 53%.
- 60 seconds with 64 threads: 15957188 `try_lock` calls, 10151638 failures, about 64%. Threads entered the critical section 15379997 times, about 256,000 times per second.
- The plain counter and the relaxed atomic counter match. Depth is 0 at the end.

---

# queue_mutex 与 std::mutex

测量日期 2026-11-17。机器是 Intel Core i7-14700（20 核、28 逻辑处理器）。编译器是 MinGW-W64 g++ 16.2.0。

功能和性能都用同一组优化选项：

```text
g++ -std=c++17 -Ofast -Wall -Wextra -pthread -static -I include -DASF_TRY_SPIN=0 -o test_queue_mutex.exe tests/test_queue_mutex.cpp
g++ -std=c++17 -Ofast -Wall -Wextra -pthread -static -I include -o bench_mutex.exe bench/bench_mutex.cpp
```

## 测法

一个互斥量，若干线程同时反复加锁、进入临界区、解锁。每个格子跑约 1 秒，表里记这一次采样。

`Mop/s` 是全部线程加起来的吞吐。`ns/op` 是它的倒数，也就是全部线程完成一次操作的平均时间。

两种临界区：

- `inc`：临界区里对线程私有整数加 1。
- `burn64`：临界区里做 64 次 `volatile` 累加。`-Ofast` 消不掉这些访问。

三种加锁方式：

- `std::mutex`：`lock` / `unlock`。
- `queue_mutex stack`：每次在栈上放一个 `lock_node`，调用 `lock(node)` / `unlock(node)`。
- `queue_mutex tls`：调用不带节点的 `lock()` / `unlock()`。节点放在线程本地槽里，每次 `lock()` 都会就地构造。

## 结果

| 线程 | 实现 | inc ns/op | inc Mop/s | burn64 ns/op | burn64 Mop/s |
| ---: | --- | ---: | ---: | ---: | ---: |
| 1 | std::mutex | 13.1 | 76.43 | 84.2 | 11.87 |
| 1 | queue_mutex stack | 25.2 | 39.65 | 75.3 | 13.29 |
| 1 | queue_mutex tls | 32.2 | 31.09 | 76.0 | 13.16 |
| 2 | std::mutex | 30.2 | 33.14 | 151.6 | 6.60 |
| 2 | queue_mutex stack | 95.3 | 10.49 | 161.3 | 6.20 |
| 2 | queue_mutex tls | 89.8 | 11.14 | 163.1 | 6.13 |
| 4 | std::mutex | 29.5 | 33.86 | 134.0 | 7.46 |
| 4 | queue_mutex stack | 96.2 | 10.40 | 175.5 | 5.70 |
| 4 | queue_mutex tls | 97.4 | 10.27 | 169.0 | 5.92 |
| 8 | std::mutex | 48.6 | 20.58 | 162.1 | 6.17 |
| 8 | queue_mutex stack | 107.1 | 9.33 | 195.3 | 5.12 |
| 8 | queue_mutex tls | 113.3 | 8.83 | 197.5 | 5.06 |
| 16 | std::mutex | 62.7 | 15.94 | 203.2 | 4.92 |
| 16 | queue_mutex stack | 123.7 | 8.08 | 199.7 | 5.01 |
| 16 | queue_mutex tls | 125.5 | 7.97 | 201.6 | 4.96 |

临界区只有一次整数加法时，`std::mutex` 更快。单线程大约 13 ns，栈节点接口大约 25 ns，TLS 接口大约 32 ns。16 线程时标准库大约 63 ns，本实现大约 124–126 ns。

临界区改成 64 次 `volatile` 累加后，临界区盖住了锁开销的大部分。单线程时栈节点接口是 75 ns，标准库是 84 ns。16 线程时两者都在 200 ns 左右。

## 功能测试

正确性测试用 `-DASF_TRY_SPIN=0` 编译。临界区里和两次操作之间各做一次 `volatile` 加法，停在几纳秒。上面的性能表仍是默认 256 次自旋。

`test_queue_mutex.exe` 通过，退出码 0。

- stress：15000 次 `try_lock`，失败 7880 次，约 53%。
- 60 秒、64 线程混跑：15957188 次 `try_lock`，失败 10151638 次，约 64%。成功进入临界区 15379997 次，大约每秒 25.6 万次。
- 非原子计数与 relaxed 原子计数一致。结束时深度为 0。
