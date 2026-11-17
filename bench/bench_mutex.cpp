#include "asf/queue_mutex.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr auto kRun = std::chrono::milliseconds(1000);

struct Sample {
  std::uint64_t ops = 0;
  double seconds = 0;
};

double ns_per(const Sample& sample) {
  return sample.ops == 0 ? 0 : sample.seconds * 1e9 / static_cast<double>(sample.ops);
}

void report(const char* name, int threads, const char* work, const Sample& sample) {
  const double ops_s = sample.seconds == 0 ? 0 : static_cast<double>(sample.ops) / sample.seconds;
  std::printf("%-18s %2d %-8s %10.1f ns  %10.2f Mop/s\n", name, threads, work, ns_per(sample),
              ops_s / 1e6);
}

template <typename Body>
Sample run(int threads, Body body) {
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  std::atomic<bool> stop{false};
  std::vector<std::uint64_t> counts(static_cast<std::size_t>(threads));
  std::vector<std::uint64_t> sinks(static_cast<std::size_t>(threads));
  std::vector<std::thread> pool;
  pool.reserve(static_cast<std::size_t>(threads));
  for (int t = 0; t < threads; ++t) {
    pool.emplace_back([&, t] {
      ready.fetch_add(1, std::memory_order_relaxed);
      while (!go.load(std::memory_order_relaxed)) {
      }
      std::uint64_t n = 0;
      std::uint64_t sink = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        body(sink);
        ++n;
      }
      counts[static_cast<std::size_t>(t)] = n;
      sinks[static_cast<std::size_t>(t)] = sink;
    });
  }
  while (ready.load(std::memory_order_relaxed) < threads) {
  }
  const auto t0 = std::chrono::steady_clock::now();
  go.store(true, std::memory_order_relaxed);
  std::this_thread::sleep_for(kRun);
  const auto t1 = std::chrono::steady_clock::now();
  stop.store(true, std::memory_order_relaxed);
  for (auto& thread : pool) {
    thread.join();
  }
  Sample sample;
  sample.seconds = std::chrono::duration<double>(t1 - t0).count();
  std::uint64_t check = 0;
  for (int t = 0; t < threads; ++t) {
    sample.ops += counts[static_cast<std::size_t>(t)];
    check += sinks[static_cast<std::size_t>(t)];
  }
  if (check == 0) {
    std::fprintf(stderr, "bench sink collapsed\n");
  }
  return sample;
}

void hold_short(std::uint64_t& sink) { sink += 1; }

void hold_burn(std::uint64_t& sink) {
  volatile std::uint64_t local = sink;
  for (int i = 0; i < 64; ++i) {
    local = local + static_cast<std::uint64_t>(i);
  }
  sink = local;
}

template <typename Hold>
Sample bench_std(int threads, Hold hold) {
  std::mutex mutex;
  return run(threads, [&](std::uint64_t& sink) {
    mutex.lock();
    hold(sink);
    mutex.unlock();
  });
}

template <typename Hold>
Sample bench_queue(int threads, Hold hold) {
  asf::queue_mutex mutex;
  return run(threads, [&](std::uint64_t& sink) {
    mutex.lock();
    hold(sink);
    mutex.unlock();
  });
}

template <typename Hold>
void sweep(const char* work, Hold hold) {
  const int threads[] = {1, 2, 4, 8, 16};
  for (int n : threads) {
    report("std::mutex", n, work, bench_std(n, hold));
    report("queue_mutex", n, work, bench_queue(n, hold));
  }
}

}  // namespace

int main() {
  std::printf("threads  impl                work         ns/op      Mop/s\n");
  sweep("inc", hold_short);
  sweep("burn64", hold_burn);
  return 0;
}
