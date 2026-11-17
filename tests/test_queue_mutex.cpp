#include "asf/queue_mutex.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

namespace {

std::atomic<int> g_failures{0};

struct SuccessCounters {
  alignas(64) std::int64_t plain = 0;
  alignas(64) std::atomic<std::int64_t> relaxed{0};
};
SuccessCounters g_success;
static_assert(offsetof(SuccessCounters, relaxed) >= 64, "counters share a cache line");
std::int64_t g_success_checked = 0;

void note_success() {
  ++g_success.plain;
  g_success.relaxed.fetch_add(1, std::memory_order_relaxed);
}

void expect_success_counters(std::int64_t new_entries, const char* what) {
  const std::int64_t plain = g_success.plain;
  const std::int64_t relaxed = g_success.relaxed.load(std::memory_order_relaxed);
  const std::int64_t expected = g_success_checked + new_entries;
  const auto line = [](const void* address) {
    return reinterpret_cast<std::uintptr_t>(address) / 64u;
  };
  if (line(&g_success.plain) == line(&g_success.relaxed) || plain != expected ||
      relaxed != expected) {
    std::fprintf(stderr, "FAIL %s plain %lld relaxed %lld expected %lld\n", what,
                 static_cast<long long>(plain), static_cast<long long>(relaxed),
                 static_cast<long long>(expected));
    g_failures.fetch_add(1, std::memory_order_relaxed);
  }
  g_success_checked = expected;
}

void expect(bool cond, const char* what) {
  if (!cond) {
    std::fprintf(stderr, "FAIL %s\n", what);
    g_failures.fetch_add(1, std::memory_order_relaxed);
  }
}

void test_uncontended() {
  asf::queue_mutex mutex;
  mutex.lock();
  mutex.unlock();
  if (mutex.try_lock()) {
    mutex.unlock();
  } else {
    expect(false, "uncontended try_lock");
  }
  mutex.lock();
  mutex.unlock();
}

void test_try_fails_while_held() {
  asf::queue_mutex mutex;
  mutex.lock();
  expect(!mutex.try_lock(), "try_lock while lock held");
  mutex.unlock();
  if (mutex.try_lock()) {
    mutex.unlock();
  } else {
    expect(false, "try_lock after release");
  }
}

void test_lock_waits_for_try() {
  asf::queue_mutex mutex;
  std::atomic<int> phase{0};
  std::atomic<int> depth{0};

  std::thread holder([&] {
    if (!mutex.try_lock()) {
      expect(false, "holder try_lock");
      phase.store(1, std::memory_order_relaxed);
      return;
    }
    phase.store(1, std::memory_order_relaxed);
    while (phase.load(std::memory_order_relaxed) < 2) {
      std::this_thread::yield();
    }
    const int seen = depth.load(std::memory_order_relaxed);
    expect(seen == 0, "lock entered before try unlock");
    mutex.unlock();
  });

  while (phase.load(std::memory_order_relaxed) < 1) {
    std::this_thread::yield();
  }
  std::thread waiter([&] {
    mutex.lock();
    depth.store(1, std::memory_order_relaxed);
    mutex.unlock();
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  phase.store(2, std::memory_order_relaxed);
  holder.join();
  waiter.join();
  expect(depth.load() == 1, "waiter eventually entered");
}

// One volatile increment, a few nanoseconds. Inside the critical section and
// again between operations. Give-up is -DASF_TRY_SPIN=0 so this dwell, not a
// microsecond pause, is what a failing try_lock races against.
void dwell() {
  volatile std::uint32_t step = 1;
  step = step + 1;
  if (step == 0) {
    std::atomic_signal_fence(std::memory_order_relaxed);
  }
}

void hold(std::atomic<int>& depth, std::atomic<std::int64_t>& entries) {
  const int seen = depth.fetch_add(1, std::memory_order_relaxed);
  if (seen != 0) {
    g_failures.fetch_add(1, std::memory_order_relaxed);
  }
  note_success();
  dwell();
  entries.fetch_add(1, std::memory_order_relaxed);
  depth.fetch_sub(1, std::memory_order_relaxed);
}

void worker_loop(asf::queue_mutex& mutex, std::atomic<bool>& stop, std::atomic<int>& depth,
                std::atomic<std::int64_t>& entries, std::atomic<std::int64_t>& try_attempts,
                std::atomic<std::int64_t>& try_failures, unsigned seed) {
  unsigned rng = seed;
  while (!stop.load(std::memory_order_relaxed)) {
    rng = rng * 1664525u + 1013904223u;
    if ((rng % 16u) < 6u) {
      mutex.lock();
      hold(depth, entries);
      mutex.unlock();
    } else {
      try_attempts.fetch_add(1, std::memory_order_relaxed);
      if (mutex.try_lock()) {
        hold(depth, entries);
        mutex.unlock();
      } else {
        try_failures.fetch_add(1, std::memory_order_relaxed);
      }
    }
    dwell();
  }
}

void test_churn() {
  constexpr int kLive = 64;
  constexpr int kHalf = kLive / 2;
  constexpr int kSeconds = 60;
  asf::queue_mutex mutex;
  std::atomic<int> depth{0};
  std::atomic<std::int64_t> entries{0};
  std::atomic<std::int64_t> try_attempts{0};
  std::atomic<std::int64_t> try_failures{0};
  const int failures_before = g_failures.load(std::memory_order_relaxed);

  struct Worker {
    std::atomic<bool> stop{false};
    std::thread thread;
  };
  std::vector<std::unique_ptr<Worker>> workers(kLive);
  unsigned seed = 1;
  auto start_one = [&](int index) {
    auto worker = std::make_unique<Worker>();
    Worker* raw = worker.get();
    raw->thread = std::thread(worker_loop, std::ref(mutex), std::ref(raw->stop), std::ref(depth),
                              std::ref(entries), std::ref(try_attempts), std::ref(try_failures),
                              seed++);
    workers[index] = std::move(worker);
  };
  for (int i = 0; i < kLive; ++i) {
    start_one(i);
  }

  int cursor = 0;
  std::int64_t previous = 0;
  std::uint64_t previous_parks = asf::park_count();
  std::uint64_t previous_park_ns = asf::park_ns();
  for (int sec = 1; sec <= kSeconds; ++sec) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    for (int k = 0; k < kHalf; ++k) {
      const int index = (cursor + k) % kLive;
      workers[index]->stop.store(true, std::memory_order_relaxed);
    }
    for (int k = 0; k < kHalf; ++k) {
      const int index = (cursor + k) % kLive;
      workers[index]->thread.join();
      start_one(index);
    }
    cursor = (cursor + kHalf) % kLive;
    const std::int64_t now = entries.load(std::memory_order_relaxed);
    const std::int64_t attempts = try_attempts.load(std::memory_order_relaxed);
    const std::int64_t failed = try_failures.load(std::memory_order_relaxed);
    const std::uint64_t parks = asf::park_count();
    const std::uint64_t parked_ns = asf::park_ns();
    std::printf("sec %d entries %lld (+%lld) depth %d try %lld fail %lld parks +%llu park_ms %llu\n",
                sec, static_cast<long long>(now), static_cast<long long>(now - previous),
                depth.load(), static_cast<long long>(attempts), static_cast<long long>(failed),
                static_cast<unsigned long long>(parks - previous_parks),
                static_cast<unsigned long long>((parked_ns - previous_park_ns) / 1000000ull));
    previous_parks = parks;
    previous_park_ns = parked_ns;
    std::fflush(stdout);
    if (now == previous) {
      expect(false, "no progress in one second");
    }
    previous = now;
  }

  for (auto& worker : workers) {
    worker->stop.store(true, std::memory_order_relaxed);
    worker->thread.join();
  }
  expect(depth.load() == 0, "churn depth drained");
  expect(g_failures.load() == failures_before, "churn exclusion");
  expect_success_counters(entries.load(), "churn success counters");

  mutex.lock();
  mutex.unlock();
}

void test_stress() {
  constexpr int kThreads = 6;
  constexpr int kIters = 4000;
  asf::queue_mutex mutex;
  std::atomic<int> depth{0};
  std::atomic<std::int64_t> entries{0};
  std::atomic<std::int64_t> try_attempts{0};
  std::atomic<std::int64_t> try_failures{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      unsigned rng = 0x9e3779b9u ^ static_cast<unsigned>(t * 17 + 1);
      for (int i = 0; i < kIters; ++i) {
        rng = rng * 1664525u + 1013904223u;
        if ((rng % 16u) < 6u) {
          mutex.lock();
          hold(depth, entries);
          mutex.unlock();
        } else {
          try_attempts.fetch_add(1, std::memory_order_relaxed);
          if (mutex.try_lock()) {
            hold(depth, entries);
            mutex.unlock();
          } else {
            try_failures.fetch_add(1, std::memory_order_relaxed);
          }
        }
        dwell();
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  expect(depth.load() == 0, "depth drained");
  expect(entries.load() > 0, "stress made progress");
  std::printf("stress try %lld fail %lld\n", static_cast<long long>(try_attempts.load()),
              static_cast<long long>(try_failures.load()));
  expect_success_counters(entries.load(), "stress success counters");

  mutex.lock();
  mutex.unlock();
}

void test_mixed() {
  asf::queue_mutex mutex;
  mutex.lock();
  mutex.unlock();
  if (!mutex.try_lock()) {
    expect(false, "uncontended try_lock");
  } else {
    mutex.unlock();
  }

  mutex.lock();
  expect(!mutex.try_lock(), "try_lock while lock held");
  mutex.unlock();

  constexpr int kThreads = 4;
  constexpr int kIters = 400;
  struct alignas(64) Plain {
    int value = 0;
  };
  struct alignas(64) Relaxed {
    std::atomic<int> value{0};
  };
  Plain plain;
  Relaxed relaxed;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kIters; ++i) {
        if (((i + t) & 1) == 0) {
          mutex.lock();
          ++plain.value;
          relaxed.value.fetch_add(1, std::memory_order_relaxed);
          mutex.unlock();
        } else if (mutex.try_lock()) {
          ++plain.value;
          relaxed.value.fetch_add(1, std::memory_order_relaxed);
          mutex.unlock();
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  const int count = relaxed.value.load(std::memory_order_relaxed);
  if (plain.value != count || count == 0) {
    std::fprintf(stderr, "FAIL mixed counters plain %d relaxed %d\n", plain.value, count);
    g_failures.fetch_add(1, std::memory_order_relaxed);
  }
}

// Hold the mutex, queue 64 lock waiters, and keep them blocked long enough that
// ASF_LOCK_SPIN=0 forces park_word (futex / WaitOnAddress / ulock).
void test_lock_os_park() {
  const std::uint64_t parks_before = asf::park_count();
  asf::queue_mutex mutex;
  mutex.lock();

  constexpr int kThreads = 64;
  std::atomic<int> ready{0};
  std::atomic<int> entered{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&] {
      ready.fetch_add(1, std::memory_order_relaxed);
      mutex.lock();
      entered.fetch_add(1, std::memory_order_relaxed);
      mutex.unlock();
    });
  }
  while (ready.load(std::memory_order_relaxed) < kThreads) {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  expect(entered.load(std::memory_order_relaxed) == 0, "park waiters entered while held");
  const std::uint64_t parks = asf::park_count() - parks_before;
  expect(parks > 0, "park waiters did not call OS park");
  std::printf("lock os park threads %d parks %llu\n", kThreads,
              static_cast<unsigned long long>(parks));
  std::fflush(stdout);

  mutex.unlock();
  for (auto& thread : threads) {
    thread.join();
  }
  expect(entered.load(std::memory_order_relaxed) == kThreads, "park waiters did not all enter");
}

}  // namespace

int main() {
  test_uncontended();
  test_try_fails_while_held();
  test_lock_waits_for_try();
  test_mixed();
  test_lock_os_park();
  test_stress();
  test_churn();
  if (g_failures.load() != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures.load());
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
