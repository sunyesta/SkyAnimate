#include "core/parallel.hh"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace oanim {

namespace {

/* Whether this thread is one of the pool's (or a caller sharing out work): work it shares runs on it alone. */
thread_local bool t_in_pool = false;

/* One caller's work: its pieces are taken by counting up `next`; `left` counts pieces not yet finished. Each job
 * has its own counters, so a thread still finishing an earlier job never takes a piece of this one. */
struct Job {
  const std::function<void(size_t, size_t)> *fn = nullptr;
  size_t count = 0, piece = 1;
  std::atomic<size_t> next{0};
  std::atomic<size_t> left{0};
};

struct Pool {
  std::vector<std::thread> threads;
  std::mutex mutex;
  std::condition_variable wake, done;
  std::shared_ptr<Job> job;
  uint64_t round = 0;
  /* One caller's work at a time; another caller meanwhile works alone. */
  std::mutex busy;

  Pool()
  {
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    const unsigned extra = std::min(cores, 16u) - 1;
    for (unsigned i = 0; i < extra; i++) {
      threads.emplace_back([this] { run(); });
      threads.back().detach();
    }
  }

  /* Takes the job's pieces until none are left. */
  void take(Job &j)
  {
    for (;;) {
      const size_t begin = j.next.fetch_add(j.piece);
      if (begin >= j.count) {
        return;
      }
      (*j.fn)(begin, std::min(begin + j.piece, j.count));
      if (j.left.fetch_sub(1) == 1) {
        std::lock_guard lock(mutex);
        done.notify_all();
      }
    }
  }

  void run()
  {
    t_in_pool = true;
    uint64_t seen = 0;
    for (;;) {
      std::shared_ptr<Job> j;
      {
        std::unique_lock lock(mutex);
        wake.wait(lock, [&] { return round != seen; });
        seen = round;
        j = job;
      }
      if (j) {
        take(*j);
      }
    }
  }
};

Pool &pool()
{
  /* Never destroyed: its threads are detached and live as long as the app. */
  static Pool *p = new Pool();
  return *p;
}

}  // namespace

int parallel_threads()
{
  return int(pool().threads.size()) + 1;
}

void parallel_for(size_t count, size_t grain, const std::function<void(size_t begin, size_t end)> &fn)
{
  grain = std::max<size_t>(grain, 1);
  if (count == 0) {
    return;
  }
  if (count <= grain || t_in_pool) {
    fn(0, count);
    return;
  }
  Pool &p = pool();
  std::unique_lock busy(p.busy, std::try_to_lock);
  if (!busy.owns_lock() || p.threads.empty()) {
    fn(0, count);
    return;
  }
  /* About four pieces a thread, so a slow piece doesn't hold the rest up. */
  const size_t threads = p.threads.size() + 1;
  auto j = std::make_shared<Job>();
  j->fn = &fn;
  j->count = count;
  j->piece = std::max(grain, (count + threads * 4 - 1) / (threads * 4));
  j->left = (count + j->piece - 1) / j->piece;
  {
    std::lock_guard lock(p.mutex);
    p.job = j;
    p.round++;
  }
  p.wake.notify_all();
  /* This thread works too, then waits for the pieces still running elsewhere. */
  t_in_pool = true;
  p.take(*j);
  t_in_pool = false;
  {
    std::unique_lock lock(p.mutex);
    p.done.wait(lock, [&] { return j->left.load() == 0; });
    /* Threads that wake late find no job, or this one with nothing left. */
    p.job.reset();
  }
}

}  // namespace oanim
