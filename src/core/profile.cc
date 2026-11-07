#include "core/profile.hh"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <thread>

namespace oanim {

namespace {

struct Frame {
  /* Section name -> (milliseconds, calls). */
  std::map<std::string, std::pair<double, int>> sections;
};

const std::thread::id g_main = std::this_thread::get_id();
Frame g_now;
std::deque<Frame> g_frames;
long long g_frame_start = 0;
constexpr size_t keep_frames = 600;
/* (Read on every thread a section runs on.) */
std::atomic<bool> g_on{false};

long long now_ns()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

ProfileScope::ProfileScope(const char *name) : name_(name)
{
  if (g_on.load(std::memory_order_relaxed)) {
    start_ = now_ns();
  }
}

ProfileScope::~ProfileScope()
{
  /* (Only the main thread's: the pool's threads and media decoders would race on the sums.) */
  if (start_ == 0 || std::this_thread::get_id() != g_main) {
    return;
  }
  std::pair<double, int> &s = g_now.sections[name_];
  s.first += double(now_ns() - start_) * 1e-6;
  s.second++;
}

void profile_enable(bool on)
{
  g_on = on;
  profile_clear();
}

bool profile_enabled()
{
  return g_on;
}

void profile_frame_end()
{
  if (!g_on) {
    return;
  }
  const long long t = now_ns();
  if (g_frame_start != 0) {
    g_now.sections["frame"] = {double(t - g_frame_start) * 1e-6, 1};
  }
  g_frame_start = t;
  g_frames.push_back(std::move(g_now));
  g_now = Frame();
  while (g_frames.size() > keep_frames) {
    g_frames.pop_front();
  }
}

std::vector<ProfileSection> profile_last(int frames)
{
  const size_t n = std::min(g_frames.size(), size_t(std::max(frames, 1)));
  std::map<std::string, ProfileSection> sum;
  for (size_t i = g_frames.size() - n; i < g_frames.size(); i++) {
    for (const auto &[name, s] : g_frames[i].sections) {
      ProfileSection &p = sum[name];
      p.name = name;
      p.mean_ms += s.first;
      p.max_ms = std::max(p.max_ms, s.first);
      p.calls += s.second;
    }
  }
  std::vector<ProfileSection> out;
  for (auto &[name, p] : sum) {
    p.mean_ms /= double(std::max<size_t>(n, 1));
    out.push_back(p);
  }
  std::sort(out.begin(), out.end(), [](const ProfileSection &a, const ProfileSection &b) {
    return a.mean_ms > b.mean_ms;
  });
  return out;
}

void profile_clear()
{
  g_frames.clear();
  g_now = Frame();
  g_frame_start = 0;
}

}  // namespace oanim
