// Stress test of the semaphore wake-up in sdk/src/core/threading_posix.cpp (PosixCondition<Semaphore>::Release /
// post_execution with thread_semaphore_wake_one), repeated here so it runs on a PC. Pattern from Tekken 6: one thread
// hands jobs to six workers with ReleaseSemaphore(1) (or n), the workers WaitForSingleObject the semaphore, some with
// short timeouts (alertable slices). A lost wake-up leaves counts in the semaphore while workers sleep: the watchdog
// reports it. Every count released must be taken exactly once.
//   clang++ -O2 -std=c++20 sdk/tests/semaphore_wake_test.cpp -o out/tools/semaphore_wake_test.exe
//   semaphore_wake_test [seconds=5] [old|addr]   ("old": notify_all on every release and take, as before; "addr":
//   thread_semaphore_address_wait, with C++20 atomic wait / notify standing in for svcWaitForAddress(WaitIfEqual) /
//   svcSignalToAddress)
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace {

bool g_old = false;
bool g_addr = false;

class Semaphore {
 public:
  explicit Semaphore(uint32_t max) : max_(max) {}
  bool Release(uint32_t n) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (n > max_ - count_) return false;
    count_ += n;
    const uint32_t waiters = waiters_;
    lock.unlock();
    if (g_addr && waiters > n) {
      Wake(int(n));
    } else if (g_addr && waiters != 0) {
      Wake(-1);
    } else if (!g_old && waiters > n) {
      for (uint32_t i = 0; i < n; ++i) cond_.notify_one();
    } else if (waiters != 0) {
      cond_.notify_all();
    }
    return true;
  }
  // true = took a count
  bool Wait(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto pred = [this] { return count_ > 0; };
    bool ok;
    if (pred()) {
      ok = true;
    } else {
      ++waiters_;
      if (g_addr) {
        ok = AddrWait(lock, timeout, pred);
      } else if (timeout == std::chrono::milliseconds::max()) {
        cond_.wait(lock, pred);
        ok = true;
      } else {
        ok = cond_.wait_for(lock, timeout, pred);
      }
      --waiters_;
    }
    if (!ok) return false;
    --count_;
    if (g_old) {
      if (waiters_ != 0) cond_.notify_all();
    } else if (count_ > 0 && waiters_ > 0) {
      if (g_addr) {
        Wake(1);
      } else {
        cond_.notify_one();
      }
    }
    return true;
  }
  uint32_t count() {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
  }

 private:
  void Wake(int count) {
    seq_.fetch_add(1);
    if (count < 0) {
      seq_.notify_all();
    } else {
      for (int i = 0; i < count; ++i) seq_.notify_one();
    }
  }
  template <typename P>
  bool AddrWait(std::unique_lock<std::mutex>& lock, std::chrono::milliseconds timeout, P& pred) {
    const auto deadline = timeout == std::chrono::milliseconds::max()
                              ? std::chrono::steady_clock::time_point::max()
                              : std::chrono::steady_clock::now() + timeout;
    for (;;) {
      if (pred()) return true;
      if (std::chrono::steady_clock::now() >= deadline) return false;
      const uint32_t seen = seq_.load();
      lock.unlock();
      if (deadline == std::chrono::steady_clock::time_point::max()) {
        seq_.wait(seen);
      } else {
        // atomic wait has no timeout: poll in short slices (the svc takes one)
        for (int i = 0; i < 2000 && seq_.load() == seen && std::chrono::steady_clock::now() < deadline; ++i) {
          std::this_thread::yield();
        }
      }
      lock.lock();
    }
  }
  std::atomic<uint32_t> seq_{0};
  std::mutex mutex_;
  std::condition_variable cond_;
  uint32_t count_ = 0, waiters_ = 0;
  const uint32_t max_;
};

}  // namespace

int main(int argc, char** argv) {
  const int seconds = argc > 1 ? std::atoi(argv[1]) : 5;
  g_old = argc > 2 && std::strcmp(argv[2], "old") == 0;
  g_addr = argc > 2 && std::strcmp(argv[2], "addr") == 0;
  constexpr int kWorkers = 6;
  Semaphore sem(1u << 20);
  Semaphore done(1u << 20);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> taken{0};
  std::vector<std::thread> workers;
  for (int w = 0; w < kWorkers; ++w) {
    workers.emplace_back([&, w] {
      std::mt19937 rng(w);
      while (!stop.load()) {
        // two workers poll in 1 ms slices (alertable waits), the rest block
        const auto t = w < 2 ? std::chrono::milliseconds(1) : std::chrono::milliseconds::max();
        if (!sem.Wait(t)) continue;
        if (stop.load()) {
          taken.fetch_add(1);
          done.Release(1);
          break;
        }
        for (volatile int i = int(rng() % 200); i > 0; --i) {
        }
        taken.fetch_add(1);
        done.Release(1);
      }
    });
  }
  uint64_t released = 0, rounds = 0;
  std::mt19937 rng(99);
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  bool stalled = false;
  while (std::chrono::steady_clock::now() < end) {
    // a frame: hand out 1..12 jobs, one release each or in batches, then wait for all of them
    const uint32_t jobs = 1 + rng() % 12;
    for (uint32_t left = jobs; left > 0;) {
      const uint32_t n = (rng() % 3 == 0) ? left : 1;
      sem.Release(n);
      left -= n;
    }
    released += jobs;
    for (uint32_t i = 0; i < jobs; ++i) {
      if (!done.Wait(std::chrono::milliseconds(2000))) {
        std::printf("STALL at round %llu: %u counts left in the semaphore, taken %llu of %llu\n",
                    (unsigned long long)rounds, sem.count(), (unsigned long long)taken.load(),
                    (unsigned long long)released);
        stalled = true;
        break;
      }
    }
    if (stalled) break;
    ++rounds;
  }
  stop.store(true);
  sem.Release(kWorkers);
  for (auto& t : workers) t.join();
  const uint64_t expect = released + kWorkers;
  std::printf("%s: %llu rounds, %llu jobs released, %llu taken (+%d stop counts expected), %s\n",
              g_old ? "old" : g_addr ? "addr" : "new", (unsigned long long)rounds, (unsigned long long)released,
              (unsigned long long)taken.load(), kWorkers,
              stalled ? "STALLED" : (taken.load() <= expect ? "ok" : "TAKEN TWICE"));
  return stalled ? 1 : 0;
}
