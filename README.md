# asf::queue_mutex

C++17 header-only mutex for a fair user-space `lock` / `try_lock`, This has an extra performance cost. `lock` takes the mutex in queue order. `try_lock` fails in order, and `lock` does not affect that order. The mutex allocates nothing and keeps no pre-sized table: the node for a call lives on the caller's stack, or in the one thread-local slot.

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

void lock_stack() {
  asf::queue_mutex::lock_node node;
  mutex.lock(node);
  mutex.unlock(node);
}

void lock_tls() {
  mutex.lock();
  mutex.unlock_lock();
  mutex.lock();
  mutex.unlock();
}
```

`try_lock(try_node&)` is released with `unlock(try_node&)`. The node stays alive until that call returns.

`lock()` and `try_lock()` take no node. Both use one `thread_local` slot per thread, shared by every `queue_mutex`. Three TLS unlocks:

- `unlock_try()` calls `unlock(try_node&)` on that slot.
- `unlock_lock()` calls `unlock(lock_node&)` on that slot.
- `unlock()` calls whichever of those the slot holds.

Calling `unlock_try()` or `unlock_lock()` for the other node is undefined.

```cpp
void try_node_unlock() {
  asf::queue_mutex::try_node attempt;
  if (mutex.try_lock(attempt)) {
    mutex.unlock(attempt);
  }
}

void try_tls_direct() {
  if (mutex.try_lock()) {
    mutex.unlock_try();
  }
}

void try_tls() {
  if (mutex.try_lock()) {
    mutex.unlock();
  }
}
```

Nodes are not copyable. The mutex is not recursive. The `try_lock` head polls `ASF_TRY_SPIN` times and then gives up. A later call waits until it becomes the head. The default is 256. Override it with `-DASF_TRY_SPIN=N`.

## Build

C++17 is required. From the repository root:

```text
cmake -S . -B build
cmake --build build
```

`test_queue_mutex` is built with `ASF_TRY_SPIN=0`. `bench_mutex` keeps the default budget of 256. GCC and Clang build both with `-Ofast -Wall -Wextra`. MinGW also links `-static`.

GitHub Actions builds both targets and runs the correctness test on Linux with GCC and Clang, on macOS, and on Windows.

The same flags without CMake:

```text
g++ -std=c++17 -Ofast -Wall -Wextra -pthread -static -I include -DASF_TRY_SPIN=0 -o test_queue_mutex.exe tests/test_queue_mutex.cpp
g++ -std=c++17 -Ofast -Wall -Wextra -pthread -static -I include -o bench_mutex.exe bench/bench_mutex.cpp
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

`meta` stays. It holds the tail of the `try_lock` queue, and it can carry a `lock` queue of its own. `try_lock` fails in the arrival order of its queue. A `lock` hanging under a `try_lock` is not in that queue. A node leaves its queue only from the head.

---

# asf::queue_mutex

C++17 头文件互斥量，用来做公平的用户态 `lock` / `try_lock`，这有额外的性能成本。`lock` 按队列顺序获得锁。`try_lock` 会按顺序失败，这个顺序不受 `lock` 影响。互斥量不做动态分配，也没有预先配制的空间：一次调用的节点放在调用方栈上，或者放在那个线程本地槽里。

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

void lock_stack() {
  asf::queue_mutex::lock_node node;
  mutex.lock(node);
  mutex.unlock(node);
}

void lock_tls() {
  mutex.lock();
  mutex.unlock_lock();
  mutex.lock();
  mutex.unlock();
}
```

`try_lock(try_node&)` 用 `unlock(try_node&)` 释放。节点一直活到这次调用返回。

`lock()` 和 `try_lock()` 都不接收节点。两者使用每个线程一个的 `thread_local` 槽，同一线程上所有 `queue_mutex` 共用。TLS 有三个解锁：

- `unlock_try()` 对该槽调用 `unlock(try_node&)`。
- `unlock_lock()` 对该槽调用 `unlock(lock_node&)`。
- `unlock()` 调用槽里当前持有的那一个。

对另一个节点调用 `unlock_try()` 或 `unlock_lock()` 是未定义行为。

```cpp
void try_node_unlock() {
  asf::queue_mutex::try_node attempt;
  if (mutex.try_lock(attempt)) {
    mutex.unlock(attempt);
  }
}

void try_tls_direct() {
  if (mutex.try_lock()) {
    mutex.unlock_try();
  }
}

void try_tls() {
  if (mutex.try_lock()) {
    mutex.unlock();
  }
}
```

节点不可复制。互斥量不递归。`try_lock` 的队头轮询 `ASF_TRY_SPIN` 次后放弃，后来的调用等到自己变成队头。默认 256，编译时用 `-DASF_TRY_SPIN=N` 覆盖。

## 编译

需要 C++17。在仓库根目录：

```text
cmake -S . -B build
cmake --build build
```

`test_queue_mutex` 使用 `ASF_TRY_SPIN=0`。`bench_mutex` 保持默认的 256 次自旋。GCC 和 Clang 对这两个目标使用 `-Ofast -Wall -Wextra`。MinGW 再链接 `-static`。

GitHub Actions 会编译这两个目标，并在 Linux（GCC、Clang）、macOS 和 Windows 上跑正确性测试。

不用 CMake 时是同一组选项：

```text
g++ -std=c++17 -Ofast -Wall -Wextra -pthread -static -I include -DASF_TRY_SPIN=0 -o test_queue_mutex.exe tests/test_queue_mutex.cpp
g++ -std=c++17 -Ofast -Wall -Wextra -pthread -static -I include -o bench_mutex.exe bench/bench_mutex.cpp
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

`meta` 一直在。它握着 `try_lock` 队列的尾，自己也可以带一条 `lock`。`try_lock` 按自己那条队列的到达顺序失败。挂在 `try_lock` 下面的 `lock` 不在这条队列里。节点只从自己队列的头离开。
