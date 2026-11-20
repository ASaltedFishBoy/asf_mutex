#pragma once

// fair_try_lock nodes form the spine. lock nodes hang off the current try tail.
// Each row below is one 64-bit CAS word: a 16-byte-aligned pointer in bits
// [47:4], a 16-bit index in bits [63:48], and flags in bits [3:0].
//
// meta.state:      owner | try_tail | claim_index
// meta.lock_tail:  lock_index | lock_tail | splice bit
// meta.lock_front: next grant target, written by the holder at acquire
//
// try_node.link:    end_index | next try | released
// try_node.segment: lock_index | lock_tail | closed
// try_node.front:   begin_index | segment front
//
// lock_node.word:   parked | grant | head | lock_index | next
// A node that saw a null tail already knows it is the head and keeps that in
// a local. The head bit is written into a waiting successor: unconditionally
// for a genuine front -- try unlock frees the owner and only a front polling
// state_ takes the mutex itself -- but for a standby wake only once the node
// parked, since a spinner already watches its own word. The parked bit is
// set only once that node is about to sleep, and only then does the
// predecessor wake it.
//
// A node never keeps the tail it displaced. That pointer is used once, to
// publish this node into its next word, and then dropped. Waiting is on
// this node's own word. After a lock acquires, it records its successor in
// lock_front_ (or swings the tail clear) and its stack node dies unreferenced.
// unlock grants lock_front_ (kGrant carries ownership itself, state_ stays
// kLock through the whole handoff chain); only when the queue drained at
// acquire time does unlock free state_ so a front polling there can CAS.

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
extern "C" int __ulock_wait(uint32_t operation, void* addr, uint64_t value, uint32_t timeout);
extern "C" int __ulock_wake(uint32_t operation, void* addr, uint64_t wake_value);
#endif

#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
#include <immintrin.h>
#elif defined(_MSC_VER) && (defined(_M_ARM64) || defined(_M_ARM))
#include <intrin.h>
#endif

namespace asf {

// Give-up budget of the fair_try_lock head. -DASF_TRY_SPIN overrides it.
#ifndef ASF_TRY_SPIN
#define ASF_TRY_SPIN 256
#endif

// Total spin budget of one acquisition: the lock() fast lane, the try_lock
// retries, and the queue wait all spend from this one count, so a waiter
// parks after the same total delay however the spins were divided.
// -DASF_LOCK_SPIN overrides it. Default covers one scheduling jitter (~85us
// at ~40ns per pause): a waiter parks only after outlasting ordinary OS
// noise. Shorter budgets let one jitter ignite a park convoy where every
// handoff pays a kernel wake.
#ifndef ASF_LOCK_SPIN
#define ASF_LOCK_SPIN 2048
#endif

// Pause iterations assumed to happen inside the OS park call itself, subtracted
// from ASF_LOCK_SPIN so the overall spin stays near that budget.
// WaitOnAddress spins with pause before the kernel wait (Microsoft STL); the
// exact count is undocumented. 256 is the Windows sync spin Wine-NSPA
// calibrated against native SRW-style waiting. futex / ulock do not spin.
#ifndef ASF_LOCK_PARK_SPIN
#if defined(_WIN32)
#define ASF_LOCK_PARK_SPIN 256
#else
#define ASF_LOCK_PARK_SPIN 0
#endif
#endif

#if !defined(NDEBUG) && !defined(__OPTIMIZE__)
#define ASF_CHECK(cond) assert(cond)
#else
#define ASF_CHECK(cond) ((void)0)
#endif

#if defined(ASF_COUNT_PARK)
inline std::atomic<std::uint64_t> g_park_calls{0};
inline std::atomic<std::uint64_t> g_park_ns{0};

inline std::uint64_t park_count() noexcept {
  return g_park_calls.load(std::memory_order_relaxed);
}

inline std::uint64_t park_ns() noexcept {
  return g_park_ns.load(std::memory_order_relaxed);
}
#endif

class queue_mutex {
 public:
  queue_mutex() = default;
  queue_mutex(const queue_mutex&) = delete;
  queue_mutex& operator=(const queue_mutex&) = delete;

  // Uncontended lock retries one CAS on state_ within the shared spin count
  // and builds no node on success. Contended lock parks on a stack lock_node,
  // then records the successor in lock_front_ before returning; unlock grants
  // that node, or frees state_ when the queue had drained at acquire time.
  void lock();

  // Unfair attempt: succeeds only when the mutex is grabbable (owner free, no
  // try spine, no lock queue), retrying within the shared ASF_LOCK_SPIN count
  // while the mutex is merely held and failing at once once anything queues.
  // Never queues itself, so it takes no position in any order.
  [[nodiscard]] bool try_lock();

  // Fair attempt: queues on the try spine and fails in its own queue order;
  // a lock() takes no position in that order. A successful fair try retires
  // its stack try_node before returning; unlock only clears state_. The head
  // polls ASF_TRY_SPIN times then gives up; a later fair try waits until it
  // is head. Only the thread entering to acquire races for the owner.
  [[nodiscard]] bool fair_try_lock();

  void unlock();

 private:
  // One cache line per spin tag: waiters never share a line with a neighbor.
  struct alignas(64) lock_node {
    lock_node() = default;
    lock_node(const lock_node&) = delete;
    lock_node& operator=(const lock_node&) = delete;
    std::atomic<std::uint64_t> word{0};
  };

  struct alignas(16) try_node {
    try_node() = default;
    try_node(const try_node&) = delete;
    try_node& operator=(const try_node&) = delete;
    std::atomic<std::uint64_t> link{0};
    std::atomic<std::uint64_t> segment{0};
    std::atomic<std::uint64_t> front{0};
  };

  static constexpr std::uint64_t kPtr = 0x0000FFFFFFFFFFF0ull;
  static constexpr std::uint64_t kHead = 0x2;
  // kHead: you are the queue front, acquire by CAS-ing state_ owner.
  // kGrant: ownership itself arrives with this bit; state_ stays kLock.
  static constexpr std::uint64_t kGrant = 0x4;
  static constexpr std::uint64_t kParked = 0x1;
  static constexpr std::uint64_t kClosed = 0x1;
  static constexpr std::uint64_t kReleased = 0x1;
  static constexpr std::uint64_t kQueued = 0x2;
  static constexpr std::uint64_t kDetached = 0x4;
  static constexpr std::uint64_t kTimedOut = 0x8;
  static constexpr std::uint64_t kSplice = 0x2;
  static constexpr unsigned kIndexShift = 48;

  static constexpr std::uint32_t kFree = 0;
  // Head polls this many times, then gives up. A waiter that reaches the same
  // budget marks itself timed out. One waker then releases that run and stops
  // at the first node that has not timed out.
  static constexpr std::uint32_t kTrySpin = ASF_TRY_SPIN;
  // Non-head lock waiters spin this many relaxes, then park on the node word.
  // On Windows, ASF_LOCK_PARK_SPIN is deducted for WaitOnAddress's own pause loop.
  static constexpr std::uint32_t kLockSpin =
      ASF_LOCK_SPIN > ASF_LOCK_PARK_SPIN ? ASF_LOCK_SPIN - ASF_LOCK_PARK_SPIN : 0;
  // Yield allowance per wait, spent between an exhausted spin budget and an
  // OS park: a descheduled holder gets this core before we pay a kernel wake
  // for it. Bounded so a wait must reach the park after finite progress.
  static constexpr std::uint32_t kYieldBridge = 12;
  // Standby wake pipeline: when a queued grant finds its target parked, walk
  // the chain and mark further nodes head so their wakes are already in
  // flight when their turn comes. The count follows the last measured grant
  // gap, one standby per kTicksPerStandby of observed handoff latency.
  static constexpr std::uint32_t kStandbyMax = 8;
  // Counts, not ns -- like the spin budgets, so the sizing follows the
  // machine's own rhythm (frequency, cache-sync speed) instead of pinning
  // the tuning to one frequency. ~1us on a 2GHz part.
  static constexpr std::uint64_t kTicksPerStandby = 2048;
  // Pre-wake allowance for a parked entry: wake the successor at acquire
  // only when the last measured hold says this critical section hides a wake
  // for less than the successor's early spin burns.
  static constexpr std::uint64_t kPreWakeMaxTicks = 4 * kTicksPerStandby;
  static constexpr std::uint32_t kTry = 1;
  static constexpr std::uint32_t kLock = 2;

  static std::uint16_t index_of(std::uint64_t word) noexcept {
    return static_cast<std::uint16_t>(word >> kIndexShift);
  }

  static std::uint16_t next_index(std::uint16_t index) noexcept {
    return static_cast<std::uint16_t>(index + 1);
  }

  template <typename T>
  static T* ptr_of(std::uint64_t word) noexcept {
    return reinterpret_cast<T*>(word & kPtr);
  }

  template <typename T>
  static std::uint64_t ptr_bits(T* node) noexcept {
    const auto bits = reinterpret_cast<std::uint64_t>(node);
    ASF_CHECK((bits & ~kPtr) == 0);
    return bits & kPtr;
  }

  static std::uint32_t owner_of(std::uint64_t word) noexcept {
    return static_cast<std::uint32_t>(word & 3u);
  }

  static std::uint64_t with_owner(std::uint64_t word, std::uint32_t owner) noexcept {
    return (word & ~std::uint64_t{3}) | owner;
  }

  static std::uint64_t with_try(std::uint64_t word, try_node* node) noexcept {
    return (word & ~kPtr) | ptr_bits(node);
  }

  static std::uint64_t with_index(std::uint64_t word, std::uint16_t index) noexcept {
    return (word & ~(std::uint64_t{0xffff} << kIndexShift)) |
           (std::uint64_t{index} << kIndexShift);
  }

  static std::uint64_t pack_indexed(void* node, std::uint16_t index, std::uint64_t flags) noexcept {
    return (std::uint64_t{index} << kIndexShift) | ptr_bits(node) | flags;
  }

  static bool cas(std::atomic<std::uint64_t>& word, std::uint64_t& expect, std::uint64_t desire) {
    return word.compare_exchange_weak(expect, desire, std::memory_order_relaxed,
                                      std::memory_order_relaxed);
  }

  // Timeout and its wake decide on the bits actually stored. A weak failure
  // can be spurious and must not look like the other side won the race.
  static bool cas_strong(std::atomic<std::uint64_t>& word, std::uint64_t& expect,
                         std::uint64_t desire) {
    return word.compare_exchange_strong(expect, desire, std::memory_order_relaxed,
                                        std::memory_order_relaxed);
  }

  static void link_lock_after(lock_node& tail, lock_node& self) {
    std::uint64_t cur = tail.word.load(std::memory_order_relaxed);
    while (!cas(tail.word, cur, (cur & ~kPtr) | ptr_bits(&self))) {
    }
  }

  static void link_try_after(try_node& tail, try_node& self, std::uint16_t end) {
    std::uint64_t cur = tail.link.load(std::memory_order_relaxed);
    while (!cas(tail.link, cur, pack_indexed(&self, end, cur & 0xfu))) {
    }
  }

  // Standby wake hint. Set only once the node parked: a spinner already
  // watches its own word for the grant, and marking it would push it onto
  // the state_-polling head branch, which has no park exit. A node that
  // parks right after this load sleeps until its own grant, losing nothing
  // but a pipeline slot. kParked is never cleared, so a load that finds it
  // set cannot miss a node that ever slept.
  static void mark_head(lock_node& node) {
    std::uint64_t cur = node.word.load(std::memory_order_relaxed);
    if ((cur & kParked) == 0) {
      return;
    }
    cur = node.word.fetch_or(kHead, std::memory_order_relaxed);
    if ((cur & kParked) != 0) {
      wake_word(node.word);
    }
  }

  // Genuine front of a state_-driven queue: the owner is freed without
  // touching this word, so the head bit is load-bearing here -- the node
  // must leave its word and poll state_. Set whether it spins or sleeps.
  static void mark_front(lock_node& node) {
    const std::uint64_t cur = node.word.fetch_or(kHead, std::memory_order_relaxed);
    if ((cur & kParked) != 0) {
      wake_word(node.word);
    }
  }

  // Ownership transfer proper: the bit alone makes the target the holder.
  static void grant_node(lock_node& node) {
    const std::uint64_t cur = node.word.fetch_or(kGrant, std::memory_order_relaxed);
    if ((cur & kParked) != 0) {
      wake_word(node.word);
    }
  }

  void lock_contended(lock_node& self, std::uint32_t& spins);
  void wait_lock(lock_node& self, bool head, std::uint32_t& spins);
  void publish_successor(lock_node& self);
  bool try_lock_fast(std::uint32_t& spins);
  bool try_lock_contended(try_node& self);
  bool join_segment(lock_node& self, try_node& owner, std::uint16_t index, std::uint16_t prev);
  bool join_origin(lock_node& self, std::uint16_t index, std::uint16_t prev);
  void handoff_subchain(try_node& self);
  void splice_segment(try_node& self, std::uint64_t segment);
  bool acquire_try(try_node& self);
  bool run_head(try_node& self);
  void publish_held(try_node& self);
  void seal(try_node& self);
  void retire(try_node& self);
  void wake_next(try_node* next);
  static void pause() noexcept;
  static void relax(std::uint32_t& spins) noexcept;
  static std::uint64_t tick_count() noexcept;
  static void park_word(std::atomic<std::uint64_t>& word, std::uint64_t expected) noexcept;
  static void wake_word(std::atomic<std::uint64_t>& word) noexcept;

  // Each meta word on its own line: join traffic (state_ bump + lock_tail_
  // swap, by arrivals) and handoff traffic (lock_front_, by holder pairs)
  // never share a cache line.
  // owner | try_tail | claim_index
  alignas(64) std::atomic<std::uint64_t> state_{0};
  // lock_index | lock_tail | splice
  alignas(64) std::atomic<std::uint64_t> lock_tail_{0};
  // Grant target for the holder's own unlock: written by the holder at
  // acquire (successor pointer, or null when the queue drained there). Only
  // the holder ever writes it, so unlock is a plain load of its own store.
  // Null whenever the lock queue is empty; in particular null while a try
  // holds, so try unlock always takes the state_ path.
  alignas(64) std::atomic<std::uint64_t> lock_front_{0};
  // Last queued-grant stamp (tick counts), holder-private and sharing
  // lock_front_'s line: the gap between stamps measures the handoff latency
  // that sizes the standby pipeline at unlock.
  std::atomic<std::uint64_t> grant_ts_{0};
  // Acquire stamp of a holder that parked on its way in (0 when it did not),
  // holder-private and sharing the same line; read back at the queued unlock
  // to measure the hold below.
  std::atomic<std::uint64_t> acquire_ts_{0};
  // Last hold measured from a parked entry (tick counts). The pre-wake gates
  // on this count rather than on how the acquiring waiter itself happened to
  // wait.
  std::atomic<std::uint64_t> hold_ticks_{0};
};

inline void queue_mutex::pause() noexcept {
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
  _mm_pause();
#elif defined(_MSC_VER) && (defined(_M_ARM64) || defined(_M_ARM))
  __yield();
#elif defined(__x86_64__) || defined(__i386__)
  asm volatile("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
  asm volatile("yield" ::: "memory");
#elif defined(__riscv)
  // fence w,0 — the Zihintpause pause hint; older cores treat it as a fence.
  asm volatile(".4byte 0x0100000f" ::: "memory");
#elif defined(__powerpc64__) || defined(__ppc64__) || defined(__PPC64__) || \
    defined(__powerpc__) || defined(__ppc__) || defined(__PPC__)
  asm volatile("or 27,27,27" ::: "memory");
#elif defined(__loongarch__)
  asm volatile("dbar 0" ::: "memory");
#elif defined(__mips__)
  asm volatile("ssnop" ::: "memory");
#elif defined(__s390x__) || defined(__s390__)
  asm volatile("" ::: "memory");
#elif defined(__sparc_v9__) || defined(__sparc64__)
  asm volatile("rd %%ccr, %%g0" ::: "memory");
#elif defined(__sparc__)
  asm volatile("membar #LoadLoad" ::: "memory");
#else
  asm volatile("" ::: "memory");
#endif
}

inline void queue_mutex::relax(std::uint32_t& spins) noexcept {
  pause();
  ++spins;
}

// A cheap machine-rhythm stamp: rdtsc on x86, steady_clock elsewhere. Gaps
// measured in these counts scale with the part's own frequency and cache
// synchronization speed, so thresholds in counts travel across CPUs the way
// the pause-count spin budgets do; nanoseconds would pin the tuning to one
// frequency.
inline std::uint64_t queue_mutex::tick_count() noexcept {
#if defined(__x86_64__) && !defined(_MSC_VER)
  std::uint32_t lo, hi;
  // serializes like RDTSCP but stays a leaf; the counter is the same TSC
  __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi));
  return (static_cast<std::uint64_t>(hi) << 32) | lo;
#elif defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
  return __rdtsc();
#elif defined(__i386__)
  return __builtin_ia32_rdtsc();
#else
  return static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

inline void queue_mutex::park_word(std::atomic<std::uint64_t>& word,
                                   std::uint64_t expected) noexcept {
#if defined(ASF_COUNT_PARK)
  g_park_calls.fetch_add(1, std::memory_order_relaxed);
  const auto park_t0 = std::chrono::steady_clock::now();
#endif
#if defined(_WIN32)
  WaitOnAddress(static_cast<volatile void*>(&word), &expected, sizeof(expected), INFINITE);
#elif defined(__linux__)
  // Flags live in the low bits; futex watches one 32-bit cell of the word.
  auto* cell = reinterpret_cast<int*>(&word);
  const auto was = static_cast<int>(static_cast<std::uint32_t>(expected));
  syscall(SYS_futex, cell, FUTEX_WAIT_PRIVATE, was, nullptr, nullptr, 0);
#elif defined(__APPLE__)
  // UL_COMPARE_AND_WAIT: 32-bit compare on the flag half of the word.
  constexpr std::uint32_t kUlCompareAndWait = 1u;
  __ulock_wait(kUlCompareAndWait, &word,
               static_cast<std::uint64_t>(static_cast<std::uint32_t>(expected)), 0);
#else
  (void)expected;
  std::this_thread::yield();
#endif
#if defined(ASF_COUNT_PARK)
  const auto park_dt = std::chrono::steady_clock::now() - park_t0;
  g_park_ns.fetch_add(
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(park_dt).count()),
      std::memory_order_relaxed);
#endif
}

inline void queue_mutex::wake_word(std::atomic<std::uint64_t>& word) noexcept {
#if defined(_WIN32)
  WakeByAddressSingle(static_cast<void*>(&word));
#elif defined(__linux__)
  auto* cell = reinterpret_cast<int*>(&word);
  syscall(SYS_futex, cell, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
#elif defined(__APPLE__)
  constexpr std::uint32_t kUlCompareAndWait = 1u;
  __ulock_wake(kUlCompareAndWait, &word, 0);
#else
  (void)word;
#endif
}

// Record what the holder's own unlock should do. Same lookup as the classic
// MCS release (swing the tail clear when nobody follows); the outcome goes to
// lock_front_ so the grant can happen at unlock time, after this stack node
// died unreferenced. The holder is the only writer of that word -- successor
// pointer, or null when the queue drained here -- so unlock is a plain load
// of its own store, with no consume ordering against the grant. The
// successor is marked head early on a spin entry, or on a parked entry when
// the last measured hold is short enough to hide the wake (adaptive
// pre-wake, see below).
inline void queue_mutex::publish_successor(lock_node& self) {
  std::uint64_t mine = self.word.load(std::memory_order_relaxed);
  lock_node* next = ptr_of<lock_node>(mine);
  if (next == nullptr) {
    std::uint64_t cur = lock_tail_.load(std::memory_order_relaxed);
    if (ptr_of<lock_node>(cur) == &self && (cur & kSplice) == 0 &&
        cas(lock_tail_, cur, pack_indexed(nullptr, index_of(cur), 0))) {
      // Queue drained: leave front null for the holder's own unlock, which
      // loads whatever this acquire leaves here.
      lock_front_.store(0, std::memory_order_relaxed);
      return;
    }
    std::uint32_t spins = 0;
    while ((next = ptr_of<lock_node>(self.word.load(std::memory_order_relaxed))) == nullptr) {
      relax(spins);
    }
  }
  lock_front_.store(ptr_bits(next), std::memory_order_relaxed);
  // Adaptive pre-wake. A spin entry says handoffs are fast right now, so the
  // early wake is free to send (and mark_head skips the successor while it
  // spins anyway). A parked entry only says our own wait was slow; it says
  // nothing about how long this critical section will keep a woken successor
  // spinning, so that case is gated on time instead: stamp the acquire, and
  // pre-wake only when the last measured hold says the section hides a wake
  // for less than it burns.
  if ((mine & kParked) == 0) {
    mark_head(*next);
    return;
  }
  acquire_ts_.store(tick_count(), std::memory_order_relaxed);
  if (hold_ticks_.load(std::memory_order_relaxed) <= kPreWakeMaxTicks) {
    mark_head(*next);
  }
}

inline void queue_mutex::wait_lock(lock_node& self, bool head, std::uint32_t& spins) {
  std::uint32_t yields = 0;
  std::uint64_t word = self.word.load(std::memory_order_relaxed);
  for (;;) {
    while (!head && (word & (kHead | kGrant)) == 0 && spins < kLockSpin) {
      relax(spins);
      word = self.word.load(std::memory_order_relaxed);
    }
    if ((word & kGrant) != 0) {
      // The predecessor's unlock granted ownership with this bit; state_
      // keeps owner=kLock the whole chain, nothing to CAS here.
      publish_successor(self);
      std::atomic_thread_fence(std::memory_order_acquire);
      return;
    }
    if (head || (word & kHead) != 0) {
      // Front of a state_-driven queue: poll owner. try unlock frees the
      // owner without touching this word, so parking here would miss it.
      std::uint64_t state = state_.load(std::memory_order_relaxed);
      if (owner_of(state) == kFree && cas(state_, state, with_owner(state, kLock))) {
        publish_successor(self);
        std::atomic_thread_fence(std::memory_order_acquire);
        return;
      }
      relax(spins);
      word = self.word.load(std::memory_order_relaxed);
      continue;
    }
    // Budget exhausted without a grant. Before paying a park, bridge a
    // possibly descheduled holder: yield this core, respinning if the queue
    // moves. With nothing runnable the yields are no-ops, so a merely long
    // critical section is unaffected. The allowance is per wait, so progress
    // cannot postpone the park forever.
    if (!head && (word & (kHead | kGrant)) == 0 && yields < kYieldBridge) {
      const std::uint64_t front = lock_front_.load(std::memory_order_relaxed);
      while (yields < kYieldBridge && (word & (kHead | kGrant)) == 0) {
        std::this_thread::yield();
        ++yields;
        word = self.word.load(std::memory_order_relaxed);
        if (lock_front_.load(std::memory_order_relaxed) != front) {
          spins = 0;
          break;
        }
      }
      if ((word & (kHead | kGrant)) != 0 || spins == 0) {
        continue;
      }
    }
    if ((word & kParked) == 0 && !cas(self.word, word, word | kParked)) {
      continue;
    }
    park_word(self.word, word | kParked);
    word = self.word.load(std::memory_order_relaxed);
  }
}

inline bool queue_mutex::join_segment(lock_node& self, try_node& owner, std::uint16_t index,
                                      std::uint16_t prev) {
  self.word.store(pack_indexed(nullptr, index, 0), std::memory_order_relaxed);
  std::uint32_t spins = 0;
  std::uint64_t cur = owner.segment.load(std::memory_order_relaxed);
  for (;;) {
    while ((cur & kClosed) == 0 && index_of(cur) != prev) {
      relax(spins);
      cur = owner.segment.load(std::memory_order_relaxed);
    }
    if ((cur & kClosed) != 0) {
      return join_origin(self, index, prev);
    }
    lock_node* pred = ptr_of<lock_node>(cur);
    if (pred == nullptr) {
      owner.front.store(pack_indexed(&self, index, 0), std::memory_order_relaxed);
      owner.segment.store(pack_indexed(&self, index, 0), std::memory_order_relaxed);
      return false;
    }
    if (cas(owner.segment, cur, pack_indexed(&self, index, 0))) {
      link_lock_after(*pred, self);
      return false;
    }
  }
}

inline bool queue_mutex::join_origin(lock_node& self, std::uint16_t index, std::uint16_t prev) {
  self.word.store(pack_indexed(nullptr, index, 0), std::memory_order_relaxed);
  std::uint32_t spins = 0;
  std::uint64_t tail_word = lock_tail_.load(std::memory_order_relaxed);
  for (;;) {
    while (index_of(tail_word) != prev || (tail_word & kSplice) != 0) {
      relax(spins);
      tail_word = lock_tail_.load(std::memory_order_relaxed);
    }
    lock_node* pred = ptr_of<lock_node>(tail_word);
    if (pred == nullptr) {
      lock_tail_.store(pack_indexed(&self, index, 0), std::memory_order_relaxed);
      return true;
    }
    if (cas(lock_tail_, tail_word, pack_indexed(&self, index, 0))) {
      link_lock_after(*pred, self);
      return false;
    }
  }
}

inline void queue_mutex::lock_contended(lock_node& self, std::uint32_t& spins) {
  self.word.store(0, std::memory_order_relaxed);
  std::uint64_t state = state_.load(std::memory_order_relaxed);
  for (;;) {
    const std::uint16_t prev = index_of(state);
    const std::uint16_t index = next_index(prev);
    if (!cas(state_, state, with_index(state, index))) {
      relax(spins);
      continue;
    }
    try_node* tail = ptr_of<try_node>(state);
    if (tail == nullptr) {
      wait_lock(self, join_origin(self, index, prev), spins);
      return;
    }
    wait_lock(self, join_segment(self, *tail, index, prev), spins);
    return;
  }
}

inline bool queue_mutex::try_lock_fast(std::uint32_t& spins) {
  // Grabbable-right-now conditions retried against the caller's overall spin
  // count: lock() spends from the same count its queue wait continues with,
  // and a plain try_lock fails when the count runs out. The spin happens only
  // while the mutex is merely held -- the lane dissolves the moment anything
  // queues anywhere (try spine or lock tail), so contended arrivals fail on
  // their first pass and reach the queue with a full budget, keeping the
  // convoy regime's whole wait spin. Retry never barges a queued front.
  for (;;) {
    std::uint64_t state = state_.load(std::memory_order_relaxed);
    if (ptr_of<try_node>(state) != nullptr) {
      return false;
    }
    const std::uint64_t tail = lock_tail_.load(std::memory_order_relaxed);
    if (ptr_of<lock_node>(tail) != nullptr) {
      return false;
    }
    if (owner_of(state) == kFree && cas(state_, state, with_owner(state, kLock))) {
      std::atomic_thread_fence(std::memory_order_acquire);
      return true;
    }
    if (spins >= kLockSpin) {
      return false;
    }
    relax(spins);
  }
}

inline void queue_mutex::lock() {
  std::uint32_t spins = 0;
  if (try_lock_fast(spins)) {
    return;
  }
  lock_node node;
  lock_contended(node, spins);
}

inline void queue_mutex::splice_segment(try_node& self, std::uint64_t segment) {
  std::uint64_t cur = segment;
  while ((cur & kClosed) == 0 && !cas(self.segment, cur, cur | kClosed)) {
  }
  lock_node* tail = ptr_of<lock_node>(cur);
  if (tail == nullptr) {
    return;
  }
  std::uint32_t spins = 0;
  std::uint64_t front = self.front.load(std::memory_order_relaxed);
  while (ptr_of<lock_node>(front) == nullptr) {
    relax(spins);
    front = self.front.load(std::memory_order_relaxed);
  }
  lock_node* head = ptr_of<lock_node>(front);
  const std::uint16_t base = static_cast<std::uint16_t>(index_of(front) - 1);
  std::uint64_t tail_word = lock_tail_.load(std::memory_order_relaxed);
  for (;;) {
    while (index_of(tail_word) != base || (tail_word & kSplice) != 0) {
      relax(spins);
      tail_word = lock_tail_.load(std::memory_order_relaxed);
    }
    lock_node* origin = ptr_of<lock_node>(tail_word);
    if (origin == nullptr) {
      lock_tail_.store(pack_indexed(tail, index_of(cur), 0), std::memory_order_relaxed);
      mark_front(*head);
      return;
    }
    if (cas(lock_tail_, tail_word, pack_indexed(tail, index_of(cur), 0))) {
      link_lock_after(*origin, *head);
      return;
    }
  }
}

inline void queue_mutex::handoff_subchain(try_node& self) {
  std::uint32_t spins = 0;
  std::uint64_t state = state_.load(std::memory_order_relaxed);
  std::uint16_t end = 0;
  for (;;) {
    if (ptr_of<try_node>(state) != &self) {
      std::uint64_t link = self.link.load(std::memory_order_relaxed);
      while (ptr_of<try_node>(link) == nullptr) {
        relax(spins);
        link = self.link.load(std::memory_order_relaxed);
      }
      end = index_of(link);
      break;
    }
    end = index_of(state);
    if (cas(state_, state, with_try(state, nullptr))) {
      std::uint64_t cur = self.link.load(std::memory_order_relaxed);
      while (!cas(self.link, cur, cur | kDetached)) {
      }
      break;
    }
  }

  std::uint64_t segment = self.segment.load(std::memory_order_relaxed);
  while (index_of(segment) != end) {
    relax(spins);
    segment = self.segment.load(std::memory_order_relaxed);
  }
  splice_segment(self, segment);
}

inline void queue_mutex::seal(try_node& self) { handoff_subchain(self); }

inline bool queue_mutex::acquire_try(try_node& self) {
  std::uint64_t tail = lock_tail_.load(std::memory_order_relaxed);
  if (ptr_of<lock_node>(tail) != nullptr || (tail & kSplice) != 0) {
    return false;
  }
  std::uint64_t state = state_.load(std::memory_order_relaxed);
  if (owner_of(state) != kFree) {
    return false;
  }
  if (!cas(state_, state, with_owner(state, kTry))) {
    return false;
  }
  publish_held(self);
  return true;
}

inline void queue_mutex::publish_held(try_node& self) { handoff_subchain(self); }

inline void queue_mutex::retire(try_node& self) {
  std::uint64_t link = self.link.load(std::memory_order_relaxed);
  if ((link & kQueued) == 0 || (link & kDetached) != 0) {
    return;
  }
  std::uint32_t spins = 0;
  try_node* next = ptr_of<try_node>(link);
  while (next == nullptr && (link & kDetached) == 0) {
    relax(spins);
    link = self.link.load(std::memory_order_relaxed);
    next = ptr_of<try_node>(link);
  }
  if (next == nullptr) {
    return;
  }
  wake_next(next);
}

inline void queue_mutex::wake_next(try_node* next) {
  while (next != nullptr) {
    std::uint64_t link = next->link.load(std::memory_order_relaxed);
    for (;;) {
      if ((link & kTimedOut) != 0) {
        break;
      }
      if (cas_strong(next->link, link, link | kReleased)) {
        return;
      }
    }
    // This node already gave up. Hand off its subchain here so it does not
    // chain-notify, then keep walking. The next loop stops at the first node
    // that has not timed out, after releasing that one.
    seal(*next);
    link = next->link.load(std::memory_order_relaxed);
    while (!cas_strong(next->link, link, link | kReleased)) {
    }
    next = ptr_of<try_node>(link);
  }
}

inline bool queue_mutex::run_head(try_node& self) {
  std::uint32_t spin = kTrySpin;
  for (;;) {
    if (acquire_try(self)) {
      return true;
    }
    if (spin == 0) {
      std::uint64_t cur = self.link.load(std::memory_order_relaxed);
      while ((cur & kTimedOut) == 0 && !cas_strong(self.link, cur, cur | kTimedOut)) {
      }
      seal(self);
      retire(self);
      return false;
    }
    --spin;
    pause();
  }
}

inline bool queue_mutex::try_lock_contended(try_node& self) {
  self.link.store(0, std::memory_order_relaxed);
  self.front.store(0, std::memory_order_relaxed);

  std::uint64_t state = state_.load(std::memory_order_relaxed);
  if (owner_of(state) == kFree && ptr_of<try_node>(state) == nullptr &&
      cas(state_, state, with_owner(state, kTry))) {
    const std::uint64_t tail = lock_tail_.load(std::memory_order_relaxed);
    if (ptr_of<lock_node>(tail) == nullptr && (tail & kSplice) == 0) {
      // Uncontended success: never queued, nothing to retire.
      std::atomic_thread_fence(std::memory_order_acquire);
      return true;
    }
    std::uint64_t back = state_.load(std::memory_order_relaxed);
    ASF_CHECK(owner_of(back) == kTry);
    while (!cas(state_, back, with_owner(back, kFree))) {
      ASF_CHECK(owner_of(back) == kTry);
    }
    state = state_.load(std::memory_order_relaxed);
  }

  self.link.store(kQueued, std::memory_order_relaxed);
  std::uint32_t spins = 0;
  state = state_.load(std::memory_order_relaxed);
  try_node* displaced = nullptr;
  std::uint16_t snapshot = 0;
  for (;;) {
    snapshot = index_of(state);
    const std::uint16_t base = snapshot;
    if (cas(state_, state, with_index(with_try(state, &self), base))) {
      displaced = ptr_of<try_node>(state);
      self.segment.store(pack_indexed(nullptr, base, 0), std::memory_order_relaxed);
      break;
    }
    relax(spins);
  }

  if (displaced == nullptr) {
    if (!run_head(self)) {
      return false;
    }
    retire(self);
    std::atomic_thread_fence(std::memory_order_acquire);
    return true;
  }
  link_try_after(*displaced, self, snapshot);
  std::uint64_t cur = self.link.load(std::memory_order_relaxed);
  std::uint32_t wait_spins = 0;
  bool timed_out = false;
  for (;;) {
    while ((cur & kReleased) == 0 && wait_spins < kTrySpin) {
      pause();
      ++wait_spins;
      cur = self.link.load(std::memory_order_relaxed);
    }
    if ((cur & kReleased) != 0) {
      break;
    }
    if (!cas_strong(self.link, cur, cur | kTimedOut)) {
      continue;
    }
    timed_out = true;
    cur |= kTimedOut;
    while ((cur & kReleased) == 0) {
      pause();
      cur = self.link.load(std::memory_order_relaxed);
    }
    break;
  }
  if (timed_out) {
    return false;
  }
  if (!run_head(self)) {
    return false;
  }
  retire(self);
  std::atomic_thread_fence(std::memory_order_acquire);
  return true;
}

inline bool queue_mutex::try_lock() {
  std::uint32_t spins = 0;
  return try_lock_fast(spins);
}

inline bool queue_mutex::fair_try_lock() {
  try_node node;
  return try_lock_contended(node);
}

inline void queue_mutex::unlock() {
  std::atomic_thread_fence(std::memory_order_release);
  // Pure read: the holder wrote this word itself at acquire (successor
  // pointer, or null when the queue drained there), and nobody else ever
  // writes it, so this load returns this thread's own store.
  lock_node* next = ptr_of<lock_node>(lock_front_.load(std::memory_order_relaxed));
  if (next != nullptr) {
    // The grant target is alive until granted, so its word can be read here;
    // nothing of it may be touched after grant_node below.
    const std::uint64_t word = next->word.load(std::memory_order_relaxed);
    const std::uint64_t stamped = acquire_ts_.load(std::memory_order_relaxed);
    std::uint64_t now = 0;
    if (stamped != 0 || (word & kParked) != 0) {
      now = tick_count();
    }
    if (stamped != 0) {
      // This holder parked on its way in and stamped its acquire: record the
      // hold it just had -- the input of the next parked entry's pre-wake.
      hold_ticks_.store(now - stamped, std::memory_order_relaxed);
      acquire_ts_.store(0, std::memory_order_relaxed);
    }
    if ((word & kParked) != 0) {
      // Convoy regime: handoffs are wake-limited while parked waiters leave
      // cores idle. Pipeline the wakes -- mark the next few queued nodes
      // head, so their wake latency overlaps the turns ahead of them. Nodes
      // still spinning are left alone by mark_head: they already watch
      // their own word, and a mark would push them onto the state_ poll
      // with no park exit. Every node walked is ungranted (grants happen in
      // chain order), hence alive; all marking finishes before the grant,
      // so no node is touched once it can die. The count follows the last
      // grant gap: one standby per kTicksPerStandby of observed handoff latency,
      // refilling deeper the slower the convoy runs. Fast-lane grants find
      // no parked bit and skip this.
      const std::uint64_t prev = grant_ts_.exchange(now, std::memory_order_relaxed);
      std::uint64_t count = (now - prev) / kTicksPerStandby;
      if (count > kStandbyMax) {
        count = kStandbyMax;
      }
      lock_node* standby = ptr_of<lock_node>(word);
      for (std::uint64_t i = 0; i < count && standby != nullptr; ++i) {
        mark_head(*standby);
        standby = ptr_of<lock_node>(standby->word.load(std::memory_order_relaxed));
      }
    }
    // Steady-state handoff: the grant bit alone makes the successor the
    // holder. state_ keeps owner=kLock; no meta word is written here.
    grant_node(*next);
    return;
  }
  // The queue had drained at acquire time (try holders always land here:
  // lock_front_ is null whenever no lock node follows). A front that joined
  // since polls state_, so ownership is released through the owner bits.
  std::uint64_t state = state_.load(std::memory_order_relaxed);
  ASF_CHECK(owner_of(state) == kTry || owner_of(state) == kLock);
  while (!cas(state_, state, with_owner(state, kFree))) {
    ASF_CHECK(owner_of(state) == kTry || owner_of(state) == kLock);
  }
}

#undef ASF_CHECK
#undef ASF_TRY_SPIN
#undef ASF_LOCK_SPIN
#undef ASF_LOCK_PARK_SPIN

}  // namespace asf
