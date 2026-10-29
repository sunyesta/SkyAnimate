/* Work split over the CPU's cores: a pool of threads kept for the app's life, started on first use. */

#pragma once

#include <cstddef>
#include <functional>

namespace oanim {

/* Calls `fn(begin, end)` over [0, count) in pieces of at least `grain`, on the pool's threads and this one,
 * returning once all are done. Runs on this thread alone when there is too little to share, when called from
 * the pool's own threads (nested), or while another thread's work has the pool. `fn` must be safe to run on
 * several threads at once over different pieces. */
void parallel_for(size_t count, size_t grain, const std::function<void(size_t begin, size_t end)> &fn);

/* How many threads parallel_for shares work over (this one included). */
int parallel_threads();

}  // namespace oanim
