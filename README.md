# asf::queue_mutex

C++17 header-only mutex for a fair user-space `lock` / `try_lock`. Fairness has an extra performance cost. `lock` takes the mutex in queue order. `try_lock` fails in its own queue order, and a `lock` does not take a turn in that queue. The mutex allocates nothing: the node for a contended call lives on the caller's stack and is gone when the call returns.

`unlock` only clears the owner. A contended `lock` hands its queue role to the successor before it returns into the critical section.

## Layout

- `include/asf/queue_mutex.hpp` — implementation
- `tests/test_queue_mutex.cpp` — correctness test
- `bench/bench_mutex.cpp` — comparison with `std::mutex`
- `bench/BENCHMARK.md` — measurements from 2026-11-17 on an i7-14700
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
  if (mutex.try_lock()) {
    mutex.unlock();
  }
}
```

The mutex is not recursive. An uncontended `lock` is one CAS and builds no node. A contended `lock` waits on a stack node. After `ASF_LOCK_SPIN` relaxes it parks in the OS. The default is 512. On Windows the park call has its own pause loop, and `ASF_LOCK_PARK_SPIN` (256) is deducted from that budget. Elsewhere the deduction is 0.

The `try_lock` head polls `ASF_TRY_SPIN` times and then gives up. A later call waits until it becomes the head, so the budget is part of the failure order. A waiter that reaches the same budget marks itself timed out. One waker releases that run and stops at the first node that has not timed out. The default is 256. Override either budget with `-DASF_TRY_SPIN=N` or `-DASF_LOCK_SPIN=N`.

## Build

C++17 is required. From the repository root:

```text
cmake -S . -B build
cmake --build build
```

`test_queue_mutex` is built with `ASF_TRY_SPIN=0`, `ASF_LOCK_SPIN=0`, and `ASF_COUNT_PARK`. `bench_mutex` keeps the defaults, 256 try spins and 512 lock spins. GCC and Clang build both with `-Ofast -Wall -Wextra`. MinGW also links `-static`.

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

`meta` holds the owner, the `try_lock` tail, and the claim index. `try_lock` nodes form that queue. `lock` nodes hang off the current `try_lock` tail, or off the lock tail once that subchain is closed. A `lock` under a `try_lock` is not in the `try_lock` failure order. A node leaves its queue only from the head.

---

# asf::queue_mutex

C++17 头文件互斥量，用来做公平的用户态 `lock` / `try_lock`。公平有额外的性能成本。`lock` 按队列顺序获得锁。`try_lock` 按自己的队列顺序失败，`lock` 不占用这个顺序里的位置。互斥量不做动态分配：竞争调用的节点在调用方栈上，调用返回后就没有了。

`unlock` 只清掉持有者。竞争的 `lock` 在返回进临界区之前，已经把队列角色交给了后继。

## 目录

- `include/asf/queue_mutex.hpp`：实现
- `tests/test_queue_mutex.cpp`：正确性测试
- `bench/bench_mutex.cpp`：和 `std::mutex` 的对比
- `bench/BENCHMARK.md`：2026-11-17 在 i7-14700 上的测量
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
  if (mutex.try_lock()) {
    mutex.unlock();
  }
}
```

互斥量不递归。无竞争的 `lock` 是一次 CAS，不建节点。有竞争的 `lock` 在栈节点上等待，`ASF_LOCK_SPIN` 次放松后转入操作系统停车。默认 512。Windows 的停车调用自带一段 pause，`ASF_LOCK_PARK_SPIN`（256）会从这份预算里扣掉。其他平台扣 0。

`try_lock` 的队头轮询 `ASF_TRY_SPIN` 次后放弃。后来的调用等到自己变成队头，这份预算属于失败顺序。等待者达到同一预算后把自己标成超时。一个唤醒者连续放行这段超时节点，遇到第一个还没超时的就停。默认 256。编译时用 `-DASF_TRY_SPIN=N` 或 `-DASF_LOCK_SPIN=N` 覆盖。

## 编译

需要 C++17。在仓库根目录：

```text
cmake -S . -B build
cmake --build build
```

`test_queue_mutex` 使用 `ASF_TRY_SPIN=0`、`ASF_LOCK_SPIN=0` 和 `ASF_COUNT_PARK`。`bench_mutex` 保持默认，try 自旋 256 次，lock 自旋 512 次。GCC 和 Clang 对这两个目标使用 `-Ofast -Wall -Wextra`。MinGW 再链接 `-static`。

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

`meta` 握着持有者、`try_lock` 队列的尾，以及领取用的 index。`try_lock` 节点排成这条队列。`lock` 挂在当前 `try_lock` 的尾上，子链关闭后改挂到 lock 队列的尾上。挂在 `try_lock` 下面的 `lock` 不在 `try_lock` 的失败顺序里。节点只从自己队列的头离开。
