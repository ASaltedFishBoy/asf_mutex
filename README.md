# asf::queue_mutex

C++17 header-only mutex for a fair user-space `lock` / `fair_try_lock`. Fairness has an extra performance cost. `lock` takes the mutex in queue order. `fair_try_lock` fails in its own queue order, and a `lock` does not take a turn in that queue. A plain `try_lock` with unfair semantics — no queue, no order, may fail spuriously — is also available; while the mutex is merely held it retries within the shared `ASF_LOCK_SPIN` count, and it fails at once once anything queues. The mutex allocates nothing: the node for a contended call lives on the caller's stack and is gone when the call returns.

`lock` records what its `unlock` should do in a meta word at acquire: the successor to grant, or nothing when the queue drained. `unlock` reads that record back — it is the holder's own store — and either grants the successor, the grant bit alone carrying ownership, or frees the owner word.

## Layout

- `include/asf/queue_mutex.hpp` — implementation
- `tests/test_queue_mutex.cpp` — correctness test
- `bench/bench_mutex.cpp` — comparison with `std::mutex`
- `bench/BENCHMARK.md` — measurements from 2026-11-18 on an i7-14700
- `CMakeLists.txt` — header-only target `asf::queue_mutex`, plus the test and benchmark

## Usage

```cpp
#include "asf/queue_mutex.hpp"

asf::queue_mutex mutex;

void exclusive() {
  mutex.lock();
  mutex.unlock();
}

void maybe() {
  if (mutex.try_lock()) {  // unfair, no order, bounded retries
    mutex.unlock();
  }
}

void maybe_fair() {
  if (mutex.fair_try_lock()) {  // fails in its own queue order
    mutex.unlock();
  }
}
```

The mutex is not recursive. An uncontended `lock` retries its one-CAS fast lane — and a plain `try_lock` its conditions — within the one shared spin count, but only while the mutex is merely held: the lane fails at once once a try spine or a lock queue exists, so contended arrivals reach the queue with a full budget. A contended `lock` waits on a stack node and continues with what is left of the count. After `ASF_LOCK_SPIN` relaxes in total — default 2048, about 85 µs, sized to outlast one scheduling jitter, since a jitter longer than the budget ignites a self-sustaining park convoy — it yields the core up to 12 times, respinning if a handoff completes, and only then parks in the OS. On Windows the park call has its own pause loop, and `ASF_LOCK_PARK_SPIN` (256) is deducted from that budget. Elsewhere the deduction is 0. When a grant finds its target parked, `unlock` also marks up to 8 further parked waiters head, one per 2048 tick counts of the last observed grant gap (~1 µs on a 2 GHz part; counts, not ns, so the sizing follows the machine's own rhythm), pipelining wake latency across upcoming turns — waiters still spinning already watch their own word and are skipped, since a mark would push them onto the `state_` poll with no park exit; this keeps throughput flat at and above the logical processor count.

The `fair_try_lock` head polls `ASF_TRY_SPIN` times and then gives up. A later call waits until it becomes the head, so the budget is part of the failure order. A waiter that reaches the same budget marks itself timed out. One waker releases that run and stops at the first node that has not timed out. The default is 256. Override either budget with `-DASF_TRY_SPIN=N` or `-DASF_LOCK_SPIN=N`.

## Build

C++17 is required. From the repository root:

```text
cmake -S . -B build
cmake --build build
```

`test_queue_mutex` is built with `ASF_TRY_SPIN=0`, `ASF_LOCK_SPIN=0`, and `ASF_COUNT_PARK`. `bench_mutex` keeps the defaults, 256 try spins and 2048 lock spins. GCC and Clang build both with `-Ofast -Wall -Wextra`. MinGW also links `-static`.

GitHub Actions builds both targets and runs the correctness test on Linux with GCC and Clang, on macOS, and on Windows.

The same flags without CMake:

```text
g++ -std=c++17 -Ofast -Wall -Wextra -pthread -I include -DASF_TRY_SPIN=0 -DASF_LOCK_SPIN=0 -DASF_COUNT_PARK -o test_queue_mutex tests/test_queue_mutex.cpp
g++ -std=c++17 -Ofast -Wall -Wextra -pthread -I include -o bench_mutex bench/bench_mutex.cpp
```

The correctness test's critical section is one `volatile` increment.

## License

[Boost Software License 1.0](LICENSE)

## Structure

```text
                                +------+    +----------+
                                | meta |--->|   lock   |
                                +--+---+    +----+-----+
                                   |             |
                                   v             v
+----------+   +----------+   +----+-----+  +----+-----+
| try_lock |-->| try_lock |-->| try_lock |  |   lock   |
+----+-----+   +----+-----+   +----------+  +----------+
     |              |
     v              v
+----+-----+   +----+-----+
|   lock   |   |   lock   |
+----------+   +----------+
```

`meta` is three cache-line-separated words: the owner together with the `try_lock` tail and the claim index, the `lock` tail, and the grant slot the holder writes at acquire. `fair_try_lock` nodes form that queue. `lock` nodes hang off the current try tail, or off the lock tail once that subchain is closed. A `lock` under a `fair_try_lock` is not in the try failure order. A node leaves its queue only from the head.

---

# asf::queue_mutex

C++17 头文件互斥量，用来做公平的用户态 `lock` / `fair_try_lock`。公平有额外的性能成本。`lock` 按队列顺序获得锁。`fair_try_lock` 按自己的队列顺序失败，`lock` 不占用这个顺序里的位置。另提供非公平语义的 `try_lock`：不排队、不占顺序、可能虚假失败；锁仅仅被持有时会在共享的 `ASF_LOCK_SPIN` 计数内重试，一旦有任何排队就立即失败。互斥量不做动态分配：竞争调用的节点在调用方栈上，调用返回后就没有了。

`lock` 在获取时把 `unlock` 要做的事记进一个 meta 字：要授权的后继，或者队列已排空时的空记录。`unlock` 读回这份记录——那本来就是持有者自己存的——然后要么授权后继（授权位本身就带着所有权），要么释放持有者位。

## 目录

- `include/asf/queue_mutex.hpp`：实现
- `tests/test_queue_mutex.cpp`：正确性测试
- `bench/bench_mutex.cpp`：和 `std::mutex` 的对比
- `bench/BENCHMARK.md`：2026-11-18 在 i7-14700 上的测量
- `CMakeLists.txt`：头文件目标 `asf::queue_mutex`，以及测试和 benchmark

## 用法

```cpp
#include "asf/queue_mutex.hpp"

asf::queue_mutex mutex;

void exclusive() {
  mutex.lock();
  mutex.unlock();
}

void maybe() {
  if (mutex.try_lock()) {  // 非公平，不占顺序，有界重试
    mutex.unlock();
  }
}

void maybe_fair() {
  if (mutex.fair_try_lock()) {  // 按自己的队列顺序失败
    mutex.unlock();
  }
}
```

互斥量不递归。无竞争的 `lock` 在快车道上重试一次 CAS——普通 `try_lock` 重试它的条件组——都花同一份共享自旋计数，但只在锁仅仅被持有时：一旦有 try 脊柱或 lock 队列存在，快车道立即失败，竞争的到达者带着满预算入队。有竞争的 `lock` 在栈节点上等待，接着用这份计数剩下的部分。总计 `ASF_LOCK_SPIN` 次放松后——默认 2048，约 85 µs，按扛住一次调度抖动来定，因为比预算长的抖动会点燃自我维持的停车车队——先让核最多 12 次，期间若有交接完成就重新自旋，然后才转入操作系统停车。Windows 的停车调用自带一段 pause，`ASF_LOCK_PARK_SPIN`（256）会从这份预算里扣掉。其他平台扣 0。授权发现目标在停车时，`unlock` 还会把队列里更多已停车的等待者标成待命队头，按上次实测授权间隔每 2048 个计数一个、最多 8 个（2 GHz 机器上约 1 µs；用计数而不是 ns，让定标跟随机器自身节奏），把唤醒延迟摊到未来几棒上流水化——仍在自旋的节点本就在盯自己的字，直接跳过，因为标记只会把它推上没有停车退路的 `state_` 轮询；这让吞吐在达到和超过逻辑核数时保持平坦。

`fair_try_lock` 的队头轮询 `ASF_TRY_SPIN` 次后放弃。后来的调用等到自己变成队头，这份预算属于失败顺序。等待者达到同一预算后把自己标成超时。一个唤醒者连续放行这段超时节点，遇到第一个还没超时的就停。默认 256。编译时用 `-DASF_TRY_SPIN=N` 或 `-DASF_LOCK_SPIN=N` 覆盖。

## 编译

需要 C++17。在仓库根目录：

```text
cmake -S . -B build
cmake --build build
```

`test_queue_mutex` 使用 `ASF_TRY_SPIN=0`、`ASF_LOCK_SPIN=0` 和 `ASF_COUNT_PARK`。`bench_mutex` 保持默认，try 自旋 256 次，lock 自旋 2048 次。GCC 和 Clang 对这两个目标使用 `-Ofast -Wall -Wextra`。MinGW 再链接 `-static`。

GitHub Actions 会编译这两个目标，并在 Linux（GCC、Clang）、macOS 和 Windows 上跑正确性测试。

不用 CMake 时是同一组选项：

```text
g++ -std=c++17 -Ofast -Wall -Wextra -pthread -I include -DASF_TRY_SPIN=0 -DASF_LOCK_SPIN=0 -DASF_COUNT_PARK -o test_queue_mutex tests/test_queue_mutex.cpp
g++ -std=c++17 -Ofast -Wall -Wextra -pthread -I include -o bench_mutex bench/bench_mutex.cpp
```

正确性测试的临界区只有一次 `volatile` 加法。

## 许可

[Boost Software License 1.0](LICENSE)

## 结构

```text
                                +------+    +----------+
                                | meta |--->|   lock   |
                                +--+---+    +----+-----+
                                   |             |
                                   v             v
+----------+   +----------+   +----+-----+  +----+-----+
| try_lock |-->| try_lock |-->| try_lock |  |   lock   |
+----+-----+   +----+-----+   +----------+  +----------+
     |              |
     v              v
+----+-----+   +----+-----+
|   lock   |   |   lock   |
+----------+   +----------+
```

`meta` 是三个分处不同缓存行的字：持有者与 `try_lock` 队列尾、领取用的 index 同在一个字里，另两个分别是 `lock` 队列尾和持有者在获取时写入的授权槽。`fair_try_lock` 节点排成这条队列。`lock` 挂在当前 try 队列的尾上，子链关闭后改挂到 lock 队列的尾上。挂在 `fair_try_lock` 下面的 `lock` 不在 try 的失败顺序里。节点只从自己队列的头离开。
