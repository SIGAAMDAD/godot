#include "core/memory_tracking/memory_tracker.h"
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <thread>
#include <vector>

int main() {
	using namespace GodotMemoryTracking;
	assert(start());
	std::atomic<unsigned> failures{0};
	std::vector<std::thread> workers;
	for (int i = 0; i < 12; i++) {
		workers.emplace_back([&, i]() {
			ScopeCategory cat(i + 1);
			for (unsigned k = 0; k < 12000; k++) {
				const size_t n = 1 + (k % 397);
				void *p = std::malloc(n);
				if (!p) { failures++; continue; }
				record_alloc(p, n);
				if ((k % 3) == 0) {
					void *q = std::realloc(p, n + 17);
					if (q) {
						record_realloc(p, q, n + 17, true);
						p = q;
					}
				}
				record_free(p);
				std::free(p);
			}
		});
	}
	for (auto &t : workers) { t.join(); }
	const Summary s = summary();
	assert(failures == 0);
	assert(s.live_bytes == 0);
	assert(s.live_allocations == 0);
	stop();
	assert(!summary().active);
	return 0;
}
