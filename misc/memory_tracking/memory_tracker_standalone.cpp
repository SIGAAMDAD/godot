#include "core/memory_tracking/memory_tracker.h"
#include <cassert>
#include <cstdlib>
#include <thread>
#include <vector>

int main() {
	using namespace GodotMemoryTracking;
	assert(start());
	assert(!start());
	void *a = std::malloc(128);
	{
		ScopeCategory cat(7);
		record_alloc(a, 128);
		assert(category() == 7);
	}
	assert(category() == 0);
	assert(summary().live_bytes == 128);
	void *b = std::realloc(a, 256);
	record_realloc(a, b, 256, true);
	assert(summary().live_bytes == 256);
	std::thread t([&] { record_free(b); });
	t.join();
	assert(summary().live_allocations == 0);
	std::free(b);
	stop();
	assert(!summary().active);
	assert(start());
	void *c = std::malloc(30);
	record_alloc(c, 30);
	stop();
	Record rec[4];
	assert(live_records(0, rec, 4) == 1);
	assert(rec[0].bytes == 30);
	assert(rec[0].address == (uintptr_t)c);
	Event ev[4];
	assert(events(1, ev, 4) == 1);
	assert(ev[0].kind == RecordKind::ALLOC);
	std::free(c);
}