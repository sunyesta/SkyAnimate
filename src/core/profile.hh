/* Where the UI's frames go: named sections timed on the main thread, summed per frame and kept for the last few
 * hundred frames, so a slow edit can be measured from Python (oanim.wm.profile) rather than guessed at. Off until
 * profile_enable: a section then costs one flag read. */

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace oanim {

class ProfileScope {
 public:
  explicit ProfileScope(const char *name);
  ~ProfileScope();
  ProfileScope(const ProfileScope &) = delete;
  ProfileScope &operator=(const ProfileScope &) = delete;

 private:
  const char *name_;
  long long start_ = 0;
};

#define OANIM_PROFILE_CAT2(a, b) a##b
#define OANIM_PROFILE_CAT(a, b) OANIM_PROFILE_CAT2(a, b)
/* Times the rest of the enclosing block as `name` (a string literal). Sections may nest; each is counted whole. */
#define OANIM_PROFILE(name) ::oanim::ProfileScope OANIM_PROFILE_CAT(profile_scope_, __LINE__)(name)

/* Called once per UI frame, after it is drawn: closes the frame's sums. */
void profile_frame_end();
/* Starts or stops timing (and forgets what was timed). */
void profile_enable(bool on);
bool profile_enabled();
/* The last `frames` frames: each section's mean milliseconds per frame (and the slowest frame), "frame" for the
 * whole frame. Sorted slowest first. */
struct ProfileSection {
  std::string name;
  double mean_ms = 0.0;
  double max_ms = 0.0;
  int calls = 0;
};
std::vector<ProfileSection> profile_last(int frames);
void profile_clear();

}  // namespace oanim
