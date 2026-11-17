#pragma once

// try_lock nodes form the spine. lock nodes hang off the current try_lock tail.
// Each row below is one 64-bit CAS word: a 16-byte-aligned pointer in bits
// [47:4], a 16-bit index in bits [63:48], and flags in bits [3:0].
//
// meta.state:     locked | try_lock_tail
// meta.lock_tail: lock_index | lock_tail | splice bit
//
// try_node.link:    next try | released
// try_node.segment: lock_index | lock_tail | closed
// try_node.front:   begin_index | segment front
//
// lock_node.word:   granted | head | lock_index | next
// The head bit marks the lock node that has no predecessor. try unlock
// frees the owner; that node takes the mutex itself.
//
// A node never keeps the tail it displaced. That pointer is used once, to
// publish this node into its next word, and then dropped. Waiting is on
// this node's own word. lock::unlock does not walk a hanging segment.

#include <atomic>
#include <cassert>
#include <cstdint>
#include <new>
#include <thread>

#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
#include <immintrin.h>
#elif defined(_MSC_VER) && (defined(_M_ARM64) || defined(_M_ARM))
#include <intrin.h>
#endif

namespace asf {

// Give-up budget of the try_lock head. -DASF_TRY_SPIN overrides it.
#ifndef ASF_TRY_SPIN
#define ASF_TRY_SPIN 256
#endif

#if !defined(NDEBUG) && !defined(__OPTIMIZE__)
#define ASF_CHECK(cond) assert(cond)
#else
#define ASF_CHECK(cond) ((void)0)
#endif

class queue_mutex {
 public:
  struct alignas(16) lock_node {
    lock_node() = default;
    lock_node(const lock_node&) = delete;
    lock_node& operator=(const lock_node&) = delete;

   private:
    friend class queue_mutex;
    std::atomic<std::uint64_t> word{0};
  };

  struct alignas(16) try_node {
    try_node() = default;
    try_node(const try_node&) = delete;
    try_node& operator=(const try_node&) = delete;

   private:
    friend class queue_mutex;
    std::atomic<std::uint64_t> link{0};
    std::atomic<std::uint64_t> segment{0};
    std::atomic<std::uint64_t> front{0};
  };

  queue_mutex() = default;
  queue_mutex(const queue_mutex&) = delete;
  queue_mutex& operator=(const queue_mutex&) = delete;

  void lock(lock_node& self);
  void unlock(lock_node& self);

  // After this returns, the try node is no longer read by the mutex.
  // Only the current head polls, for kTrySpin pauses; a later try_lock waits
  // until it is head.
  [[nodiscard]] bool try_lock(try_node& self);
  void unlock(try_node& self);

  // One TLS slot per thread, shared by every mutex.
  // unlock() uses the node the slot holds. unlock_lock() and unlock_try()
  // use that node directly; the other alternative is undefined.
  void lock();
  [[nodiscard]] bool try_lock();
  void unlock();
  void unlock_lock();
  void unlock_try();

 private:
  static constexpr std::uint64_t kPtr = 0x0000FFFFFFFFFFF0ull;
  static constexpr std::uint64_t kGrant = 0x1;
  static constexpr std::uint64_t kHead = 0x2;
  static constexpr std::uint64_t kClosed = 0x1;
  static constexpr std::uint64_t kReleased = 0x1;
  static constexpr std::uint64_t kQueued = 0x2;
  static constexpr std::uint64_t kSplice = 0x2;
  static constexpr std::uint64_t kSegmentPin = 0x4;
  static constexpr unsigned kIndexShift = 48;

  static constexpr std::uint32_t kFree = 0;
  // Head polls this many times, then gives up. Later arrivals wait to become
  // head, so the budget is part of the failure order and is not per call.
  static constexpr std::uint32_t kTrySpin = ASF_TRY_SPIN;
  static constexpr std::uint32_t kTry = 1;
  static constexpr std::uint32_t kLock = 2;

  static std::uint16_t index_of(std::uint64_t word) noexcept {
    return static_cast<std::uint16_t>(word >> kIndexShift);
  }

  static std::uint16_t next_index(std::uint16_t index) noexcept {
    const auto n = static_cast<std::uint16_t>(index + 1);
    return n == 0 ? 1 : n;
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

  static std::uint64_t pack_indexed(void* node, std::uint16_t index, std::uint64_t flags) noexcept {
    return (std::uint64_t{index} << kIndexShift) | ptr_bits(node) | flags;
  }

  static bool cas(std::atomic<std::uint64_t>& word, std::uint64_t& expect, std::uint64_t desire) {
    return word.compare_exchange_weak(expect, desire, std::memory_order_relaxed,
                                      std::memory_order_relaxed);
  }

  static void link_lock_after(lock_node& tail, lock_node& self) {
    std::uint64_t cur = tail.word.load(std::memory_order_relaxed);
    while (!cas(tail.word, cur, (cur & ~kPtr) | ptr_bits(&self))) {
    }
  }

  static void link_try_after(try_node& tail, try_node& self) {
    std::uint64_t cur = tail.link.load(std::memory_order_relaxed);
    while (!cas(tail.link, cur, (cur & ~kPtr) | ptr_bits(&self))) {
    }
  }

  static void grant_lock(lock_node& node) {
    std::uint64_t cur = node.word.load(std::memory_order_relaxed);
    while (!cas(node.word, cur, cur | kGrant)) {
    }
  }

  static void mark_head(lock_node& node) {
    std::uint64_t cur = node.word.load(std::memory_order_relaxed);
    while (!cas(node.word, cur, cur | kHead)) {
    }
  }

  void wait_lock(lock_node& self);
  bool join_segment(lock_node& self, try_node& owner);
  void join_origin(lock_node& self);
  bool acquire_try(try_node& self);
  bool run_head(try_node& self);
  void publish_held(try_node& self);
  void seal(try_node& self);
  void retire(try_node& self);
  void handoff();
  static void relax(std::uint32_t& spins) noexcept;

  struct tls_hold {
    union {
      lock_node held_lock;
      try_node held_try;
    };
    bool try_held;

    tls_hold() : held_lock(), try_held(false) {}
    ~tls_hold() { destroy(); }
    tls_hold(const tls_hold&) = delete;
    tls_hold& operator=(const tls_hold&) = delete;

    void destroy() noexcept {
      if (try_held) {
        held_try.~try_node();
      } else {
        held_lock.~lock_node();
      }
    }

    lock_node& emplace_lock() {
      destroy();
      try_held = false;
      return *::new (&held_lock) lock_node();
    }

    try_node& emplace_try() {
      destroy();
      try_held = true;
      return *::new (&held_try) try_node();
    }
  };
  static thread_local tls_hold tls_;
  static_assert(alignof(tls_hold) >= 16);

  // locked | try_lock_tail
  std::atomic<std::uint64_t> state_{0};
  // lock_index | lock_tail | splice
  std::atomic<std::uint64_t> lock_tail_{0};
};

inline thread_local queue_mutex::tls_hold queue_mutex::tls_{};

inline void queue_mutex::relax(std::uint32_t& spins) noexcept {
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
  if ((++spins & 63u) == 0) {
    std::this_thread::yield();
  }
}

inline void queue_mutex::wait_lock(lock_node& self) {
  std::uint32_t spins = 0;
  for (;;) {
    const std::uint64_t word = self.word.load(std::memory_order_relaxed);
    if ((word & kGrant) != 0) {
      std::atomic_thread_fence(std::memory_order_acquire);
      return;
    }
    if ((word & kHead) != 0) {
      std::uint64_t state = state_.load(std::memory_order_relaxed);
      if (owner_of(state) == kFree && cas(state_, state, with_owner(state, kLock))) {
        grant_lock(self);
        std::atomic_thread_fence(std::memory_order_acquire);
        return;
      }
    }
    relax(spins);
  }
}

inline bool queue_mutex::join_segment(lock_node& self, try_node& owner) {
  std::uint64_t cur = owner.segment.load(std::memory_order_relaxed);
  std::uint32_t spins = 0;
  for (;;) {
    if ((cur & kClosed) != 0) {
      return false;
    }
    const std::uint16_t index = next_index(index_of(cur));
    const std::uint64_t desire = pack_indexed(&self, index, 0);
    self.word.store(pack_indexed(nullptr, index, 0), std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    if (cas(owner.segment, cur, desire)) {
      std::atomic_thread_fence(std::memory_order_acquire);
      if (lock_node* tail = ptr_of<lock_node>(cur)) {
        link_lock_after(*tail, self);
      } else {
        std::uint64_t front = 0;
        while (!cas(owner.front, front, pack_indexed(&self, index, 0))) {
          if (ptr_of<lock_node>(front) != nullptr) {
            break;
          }
        }
      }
      return true;
    }
    relax(spins);
  }
}

inline void queue_mutex::join_origin(lock_node& self) {
  std::uint64_t cur = lock_tail_.load(std::memory_order_relaxed);
  lock_node* displaced = nullptr;
  std::uint16_t index = 1;
  std::uint32_t spins = 0;
  for (;;) {
    if ((cur & kSplice) != 0) {
      relax(spins);
      cur = lock_tail_.load(std::memory_order_relaxed);
      continue;
    }
    displaced = ptr_of<lock_node>(cur);
    index = next_index(index_of(cur));
    const std::uint64_t flags = displaced == nullptr ? kHead : 0;
    self.word.store(pack_indexed(nullptr, index, flags), std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    if (cas(lock_tail_, cur, pack_indexed(&self, index, 0))) {
      break;
    }
  }
  if (displaced != nullptr) {
    std::atomic_thread_fence(std::memory_order_acquire);
    link_lock_after(*displaced, self);
  }
  wait_lock(self);
}

inline void queue_mutex::lock(lock_node& self) {
  self.word.store(0, std::memory_order_relaxed);
  std::uint32_t spins = 0;
  for (;;) {
    std::uint64_t state = state_.load(std::memory_order_relaxed);
    try_node* tail = ptr_of<try_node>(state);
    if (tail == nullptr) {
      join_origin(self);
      return;
    }
    if ((state & kSegmentPin) != 0 || !cas(state_, state, state | kSegmentPin)) {
      relax(spins);
      continue;
    }
    const bool joined = join_segment(self, *tail);
    std::uint64_t cur = state_.load(std::memory_order_relaxed);
    while (!cas(state_, cur, cur & ~kSegmentPin)) {
    }
    if (joined) {
      wait_lock(self);
      return;
    }
    join_origin(self);
    return;
  }
}

inline void queue_mutex::unlock(lock_node& self) {
  std::atomic_thread_fence(std::memory_order_release);
  std::uint64_t mine = self.word.load(std::memory_order_relaxed);
  lock_node* next = ptr_of<lock_node>(mine);
  if (next == nullptr) {
    std::uint64_t cur = lock_tail_.load(std::memory_order_relaxed);
    if (ptr_of<lock_node>(cur) == &self && (cur & kSplice) == 0 && cas(lock_tail_, cur, 0)) {
      std::uint64_t state = state_.load(std::memory_order_relaxed);
      ASF_CHECK(owner_of(state) == kLock);
      while (!cas(state_, state, with_owner(state, kFree))) {
        ASF_CHECK(owner_of(state) == kLock);
      }
      return;
    }
    std::uint32_t spins = 0;
    while ((next = ptr_of<lock_node>(self.word.load(std::memory_order_relaxed))) == nullptr) {
      relax(spins);
    }
  }
  std::atomic_thread_fence(std::memory_order_acquire);
  grant_lock(*next);
}

inline void queue_mutex::seal(try_node& self) {
  std::uint32_t spins = 0;
  std::uint64_t segment = self.segment.load(std::memory_order_relaxed);
  for (;;) {
    if (index_of(segment) != 0) {
      break;
    }
    if (cas(self.segment, segment, segment | kClosed)) {
      return;
    }
  }

  std::uint64_t pinned = lock_tail_.load(std::memory_order_relaxed);
  for (;;) {
    if ((pinned & kSplice) != 0) {
      relax(spins);
      pinned = lock_tail_.load(std::memory_order_relaxed);
      continue;
    }
    if (cas(lock_tail_, pinned, pinned | kSplice)) {
      break;
    }
  }

  for (;;) {
    if (cas(self.segment, segment, segment | kClosed)) {
      break;
    }
  }

  std::uint64_t front = self.front.load(std::memory_order_relaxed);
  while (ptr_of<lock_node>(front) == nullptr) {
    relax(spins);
    front = self.front.load(std::memory_order_relaxed);
  }
  std::atomic_thread_fence(std::memory_order_acquire);
  std::atomic_thread_fence(std::memory_order_release);

  lock_node* head = ptr_of<lock_node>(front);
  lock_node* origin_tail = ptr_of<lock_node>(pinned);
  if (origin_tail != nullptr) {
    link_lock_after(*origin_tail, *head);
  }

  std::uint64_t expect = pinned | kSplice;
  const std::uint64_t desire = pack_indexed(ptr_of<lock_node>(segment), index_of(segment), 0);
  while (!cas(lock_tail_, expect, desire)) {
    expect = pinned | kSplice;
    relax(spins);
  }
  if (origin_tail == nullptr) {
    mark_head(*head);
  }
}

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

inline void queue_mutex::publish_held(try_node& self) {
  std::uint32_t spins = 0;
  std::uint64_t segment = self.segment.load(std::memory_order_relaxed);
  for (;;) {
    if (index_of(segment) != 0) {
      break;
    }
    if (cas(self.segment, segment, segment | kClosed)) {
      return;
    }
  }
  for (;;) {
    if (cas(self.segment, segment, segment | kClosed)) {
      break;
    }
  }

  std::uint64_t front = self.front.load(std::memory_order_relaxed);
  while (ptr_of<lock_node>(front) == nullptr) {
    relax(spins);
    front = self.front.load(std::memory_order_relaxed);
  }
  std::atomic_thread_fence(std::memory_order_acquire);
  std::atomic_thread_fence(std::memory_order_release);
  lock_node* head = ptr_of<lock_node>(front);
  const std::uint64_t desire = pack_indexed(ptr_of<lock_node>(segment), index_of(segment), 0);
  std::uint64_t cur = lock_tail_.load(std::memory_order_relaxed);
  for (;;) {
    if ((cur & kSplice) != 0) {
      relax(spins);
      cur = lock_tail_.load(std::memory_order_relaxed);
      continue;
    }
    lock_node* origin_tail = ptr_of<lock_node>(cur);
    if (!cas(lock_tail_, cur, desire)) {
      continue;
    }
    if (origin_tail == nullptr) {
      mark_head(*head);
    } else {
      link_lock_after(*origin_tail, *head);
    }
    return;
  }
}

inline void queue_mutex::retire(try_node& self) {
  if ((self.link.load(std::memory_order_relaxed) & kQueued) == 0) {
    return;
  }
  std::uint64_t link = self.link.load(std::memory_order_relaxed);
  try_node* next = ptr_of<try_node>(link);
  if (next == nullptr) {
    std::uint32_t spins = 0;
    for (;;) {
      std::uint64_t state = state_.load(std::memory_order_relaxed);
      if (ptr_of<try_node>(state) != &self) {
        break;
      }
      if ((state & kSegmentPin) != 0) {
        relax(spins);
        continue;
      }
      if (cas(state_, state, with_try(state, nullptr))) {
        return;
      }
    }
    while ((next = ptr_of<try_node>(self.link.load(std::memory_order_relaxed))) == nullptr) {
      relax(spins);
    }
  }
  std::atomic_thread_fence(std::memory_order_acquire);
  std::uint64_t next_link = next->link.load(std::memory_order_relaxed);
  while (!cas(next->link, next_link, next_link | kReleased)) {
  }
}

inline void queue_mutex::handoff() {
  std::uint64_t state = state_.load(std::memory_order_relaxed);
  ASF_CHECK(owner_of(state) == kTry);
  while (!cas(state_, state, with_owner(state, kFree))) {
    ASF_CHECK(owner_of(state) == kTry);
  }
}

inline bool queue_mutex::run_head(try_node& self) {
  std::uint32_t spin = kTrySpin;
  for (;;) {
    if (acquire_try(self)) {
      return true;
    }
    if (spin == 0) {
      seal(self);
      retire(self);
      return false;
    }
    --spin;
    std::uint32_t spins = 0;
    relax(spins);
  }
}

inline bool queue_mutex::try_lock(try_node& self) {
  self.link.store(0, std::memory_order_relaxed);
  self.segment.store(0, std::memory_order_relaxed);
  self.front.store(0, std::memory_order_relaxed);

  std::uint64_t state = state_.load(std::memory_order_relaxed);
  if (owner_of(state) == kFree && ptr_of<try_node>(state) == nullptr &&
      cas(state_, state, with_owner(state, kTry))) {
    const std::uint64_t tail = lock_tail_.load(std::memory_order_relaxed);
    if (ptr_of<lock_node>(tail) == nullptr && (tail & kSplice) == 0) {
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
  std::atomic_thread_fence(std::memory_order_release);
  std::uint32_t spins = 0;
  for (;;) {
    if ((state & kSegmentPin) != 0) {
      relax(spins);
      state = state_.load(std::memory_order_relaxed);
      continue;
    }
    const std::uint64_t desire = with_try(state, &self);
    if (cas(state_, state, desire)) {
      break;
    }
    relax(spins);
  }

  try_node* displaced = ptr_of<try_node>(state);
  if (displaced == nullptr) {
    if (!run_head(self)) {
      return false;
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    return true;
  }
  link_try_after(*displaced, self);
  std::uint32_t wait_spins = 0;
  while ((self.link.load(std::memory_order_relaxed) & kReleased) == 0) {
    relax(wait_spins);
  }
  if (!run_head(self)) {
    return false;
  }
  std::atomic_thread_fence(std::memory_order_acquire);
  return true;
}

inline void queue_mutex::unlock(try_node& self) {
  std::atomic_thread_fence(std::memory_order_release);
  handoff();
  retire(self);
}

inline void queue_mutex::lock() { lock(tls_.emplace_lock()); }

inline bool queue_mutex::try_lock() { return try_lock(tls_.emplace_try()); }

inline void queue_mutex::unlock() {
  if (tls_.try_held) {
    unlock(tls_.held_try);
  } else {
    unlock(tls_.held_lock);
  }
}

inline void queue_mutex::unlock_lock() { unlock(tls_.held_lock); }

inline void queue_mutex::unlock_try() { unlock(tls_.held_try); }

#undef ASF_CHECK
#undef ASF_TRY_SPIN

}  // namespace asf
