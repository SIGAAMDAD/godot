/**************************************************************************/
/*  memory_tracker.cpp                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "memory_tracker.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <new>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__GLIBC__) || defined(__APPLE__)
#include <execinfo.h>
#endif

namespace GodotMemoryTracking {
namespace {

constexpr size_t SHARD_COUNT = 64;
constexpr size_t RECORDS_PER_SHARD = 2048;
constexpr size_t EVENT_CAPACITY = 16384;
constexpr uintptr_t TOMBSTONE = 1;
constexpr size_t CALLSTACK_CAPACITY = 8192;
constexpr size_t CALLSTACK_DEPTH = 12;

struct Slot {
	uintptr_t address = 0;
	uint64_t allocation_id = 0;
	uint64_t bytes = 0;
	uint32_t category = 0;
	uint32_t generation = 0;
	uint32_t stack_id = 0;
};

struct StackEntry {
	uint64_t signature = 0;
	uint32_t id = 0;
	uint32_t depth = 0;
	uintptr_t frames[CALLSTACK_DEPTH] = {};
};

struct Shard {
	std::mutex mutex;
	Slot slots[RECORDS_PER_SHARD];
};

struct State {
	std::atomic<bool> enabled{ false };
	std::atomic<uint32_t> active_hooks{ 0 };
	std::mutex lifecycle_mutex;
	std::mutex event_mutex;
	std::mutex callstack_mutex;
	StackEntry callstacks[CALLSTACK_CAPACITY];
	Shard shards[SHARD_COUNT];
	Shard gpu_shards[SHARD_COUNT];
	Shard logical_shards[SHARD_COUNT];
	Event event_ring[EVENT_CAPACITY];
	std::atomic<uint64_t> capture_id{ 0 };
	std::atomic<uint64_t> sequence{ 0 };
	std::atomic<uint64_t> frame_id{ 0 };
	std::atomic<uint64_t> live_bytes{ 0 };
	std::atomic<uint64_t> peak_bytes{ 0 };
	std::atomic<uint64_t> live_allocations{ 0 };
	std::atomic<uint64_t> gpu_live_bytes{ 0 };
	std::atomic<uint64_t> gpu_peak_bytes{ 0 };
	std::atomic<uint64_t> gpu_live_allocations{ 0 };
	std::atomic<uint64_t> gpu_total_allocations{ 0 };
	std::atomic<uint64_t> logical_live_bytes{ 0 };
	std::atomic<uint64_t> logical_peak_bytes{ 0 };
	std::atomic<uint64_t> logical_live_allocations{ 0 };
	std::atomic<uint64_t> logical_total_allocations{ 0 };
	std::atomic<uint64_t> gpu_driver_allocated_bytes{ 0 };
	std::atomic<uint64_t> gpu_reserved_bytes{ 0 };
	std::atomic<uint64_t> gpu_budget_bytes{ 0 };
	std::atomic<bool> gpu_budget_supported{ false };
	std::atomic<uint64_t> total_allocations{ 0 };
	std::atomic<uint64_t> dropped_records{ 0 };
	std::atomic<uint64_t> overwritten_events{ 0 };
};

// The state persists to process shutdown. No callback can race a state free.
std::atomic<State *> state{ nullptr };
thread_local uint32_t active_category = 0;
std::atomic<uint64_t> completed_render_frame_us{ 0 };
std::mutex completed_render_frame_mutex;
CompletedRenderFrameSample completed_render_frame_sample;

size_t shard_index(uintptr_t p_ptr) {
	uint64_t h = (uint64_t)(p_ptr >> 4);
	h ^= h >> 33;
	h *= UINT64_C(0xff51afd7ed558ccd);
	h ^= h >> 33;
	return (size_t)h & (SHARD_COUNT - 1);
}

size_t slot_index(uintptr_t p_ptr) {
	uint64_t h = (uint64_t)(p_ptr >> 4);
	h ^= h >> 29;
	h *= UINT64_C(0xc4ceb9fe1a85ec53);
	h ^= h >> 32;
	return (size_t)h & (RECORDS_PER_SHARD - 1);
}

uint32_t sample_stack(State *p_state) {
	void *sample[CALLSTACK_DEPTH] = {};
	size_t count = 0;
#if defined(_WIN32)
	count = CaptureStackBackTrace(3, (DWORD)CALLSTACK_DEPTH, sample, nullptr);
#elif defined(__GLIBC__) || defined(__APPLE__)
	const int depth = backtrace(sample, int(CALLSTACK_DEPTH));
	if (depth > 3) {
		count = size_t(depth - 3);
		for (size_t i = 0; i < count; ++i) {
			sample[i] = sample[i + 3];
		}
	}
#endif
	if (!count) {
		return 0;
	}
	uint64_t signature = UINT64_C(14695981039346656037);
	for (size_t i = 0; i < count; ++i) {
		signature = (signature ^ (uint64_t)(uintptr_t)sample[i]) * UINT64_C(1099511628211);
	}
	if (!signature) {
		signature = 1;
	}
	std::lock_guard<std::mutex> guard(p_state->callstack_mutex);
	const size_t first = size_t(signature) & (CALLSTACK_CAPACITY - 1);
	for (size_t i = 0; i < CALLSTACK_CAPACITY; ++i) {
		const size_t slot = (first + i) & (CALLSTACK_CAPACITY - 1);
		StackEntry &entry = p_state->callstacks[slot];
		if (!entry.id) {
			entry.signature = signature;
			entry.id = uint32_t(slot + 1);
			entry.depth = uint32_t(count);
			for (size_t f = 0; f < count; ++f) {
				entry.frames[f] = (uintptr_t)sample[f];
			}
			return entry.id;
		}
		if (entry.signature == signature && entry.depth == count) {
			bool equal = true;
			for (size_t f = 0; f < count; ++f) {
				if (entry.frames[f] != (uintptr_t)sample[f]) {
					equal = false;
					break;
				}
			}
			if (equal) {
				return entry.id;
			}
		}
	}
	return 0; // A full stack dictionary does not invalidate allocation data.
}

void write_event(State *p_state, RecordKind p_kind, const Slot &p_record, bool p_gpu = false) {
	// Publish sequence and payload under the same lock. A producer that obtains
	// its sequence before taking the lock could otherwise overwrite a newer
	// event after another producer has already committed it.
	std::lock_guard<std::mutex> guard(p_state->event_mutex);
	const uint64_t seq = p_state->sequence.load(std::memory_order_relaxed) + 1;
	const uint64_t timestamp_us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch())
					.count());
	const uint64_t thread_id = uint64_t(std::hash<std::thread::id>{}(std::this_thread::get_id()));
	p_state->event_ring[(seq - 1) % EVENT_CAPACITY] = { seq, p_record.allocation_id, p_record.address, p_record.bytes,
		p_record.category, p_kind, p_gpu, p_record.stack_id, timestamp_us, thread_id,
		p_state->frame_id.load(std::memory_order_relaxed) };
	p_state->sequence.store(seq, std::memory_order_release);
	if (seq > EVENT_CAPACITY) {
		p_state->overwritten_events.fetch_add(1, std::memory_order_relaxed);
	}
}

void add_record(State *p_state, uintptr_t p_address, size_t p_bytes, RecordKind p_kind, bool p_gpu = false, bool p_logical = false) {
	auto &live_bytes = p_logical ? p_state->logical_live_bytes : p_gpu ? p_state->gpu_live_bytes
																	   : p_state->live_bytes;
	auto &peak_bytes = p_logical ? p_state->logical_peak_bytes : p_gpu ? p_state->gpu_peak_bytes
																	   : p_state->peak_bytes;
	auto &live_allocations = p_logical ? p_state->logical_live_allocations : p_gpu ? p_state->gpu_live_allocations
																				   : p_state->live_allocations;
	auto &total_allocations = p_logical ? p_state->logical_total_allocations : p_gpu ? p_state->gpu_total_allocations
																					 : p_state->total_allocations;
	if (p_address <= TOMBSTONE) {
		return;
	}
	const uint32_t stack_id = sample_stack(p_state);
	Shard &shard = (p_logical ? p_state->logical_shards : p_gpu ? p_state->gpu_shards
																: p_state->shards)[shard_index(p_address)];
	Slot added;
	bool inserted = false;
	{
		std::lock_guard<std::mutex> guard(shard.mutex);
		const size_t start = slot_index(p_address);
		size_t free_slot = RECORDS_PER_SHARD;
		for (size_t n = 0; n < RECORDS_PER_SHARD; ++n) {
			const size_t i = (start + n) & (RECORDS_PER_SHARD - 1);
			Slot &s = shard.slots[i];
			if (s.address == p_address) {
				// A duplicate address must never count as a second live allocation.
				live_bytes.fetch_sub(s.bytes, std::memory_order_relaxed);
				s.bytes = p_bytes;
				s.category = active_category;
				s.stack_id = stack_id;
				added = s;
				inserted = true;
				break;
			}
			if (s.address == TOMBSTONE && free_slot == RECORDS_PER_SHARD) {
				free_slot = i;
			}
			if (s.address == 0) {
				if (free_slot == RECORDS_PER_SHARD) {
					free_slot = i;
				}
				break;
			}
		}
		if (!inserted && free_slot < RECORDS_PER_SHARD) {
			Slot &s = shard.slots[free_slot];
			s.address = p_address;
			s.bytes = p_bytes;
			s.category = active_category;
			s.stack_id = stack_id;
			s.generation++;
			s.allocation_id = total_allocations.fetch_add(1, std::memory_order_relaxed) + 1;
			live_allocations.fetch_add(1, std::memory_order_relaxed);
			added = s;
			inserted = true;
		}
		if (inserted) {
			const uint64_t live = live_bytes.fetch_add(p_bytes, std::memory_order_relaxed) + p_bytes;
			uint64_t old_peak = peak_bytes.load(std::memory_order_relaxed);
			while (old_peak < live && !peak_bytes.compare_exchange_weak(old_peak, live, std::memory_order_relaxed)) {
			}
			// Serialize an address's allocation event before any other thread can
			// reuse or free that slot. A later free must never precede its alloc.
			write_event(p_state, p_kind, added, p_gpu || p_logical);
		}
	}
	if (!inserted) {
		p_state->dropped_records.fetch_add(1, std::memory_order_relaxed);
	}
}

void remove_record(State *p_state, uintptr_t p_address, bool p_gpu = false, bool p_logical = false) {
	auto &live_bytes = p_logical ? p_state->logical_live_bytes : p_gpu ? p_state->gpu_live_bytes
																	   : p_state->live_bytes;
	auto &live_allocations = p_logical ? p_state->logical_live_allocations : p_gpu ? p_state->gpu_live_allocations
																				   : p_state->live_allocations;
	if (p_address <= TOMBSTONE) {
		return;
	}
	Shard &shard = (p_logical ? p_state->logical_shards : p_gpu ? p_state->gpu_shards
																: p_state->shards)[shard_index(p_address)];
	Slot removed;
	{
		std::lock_guard<std::mutex> guard(shard.mutex);
		const size_t start = slot_index(p_address);
		for (size_t n = 0; n < RECORDS_PER_SHARD; ++n) {
			Slot &s = shard.slots[(start + n) & (RECORDS_PER_SHARD - 1)];
			if (s.address == 0) {
				break;
			}
			if (s.address == p_address) {
				removed = s;
				s.address = TOMBSTONE;
				live_bytes.fetch_sub(s.bytes, std::memory_order_relaxed);
				live_allocations.fetch_sub(1, std::memory_order_relaxed);
				write_event(p_state, RecordKind::FREE, removed, p_gpu || p_logical);
				break;
			}
		}
	}
	// An allocation that predates capture is not a tracking error.
}

} // namespace

bool start() {
	State *p = state.load(std::memory_order_acquire);
	if (!p) {
		void *memory = std::malloc(sizeof(State));
		if (!memory) {
			return false;
		}
		State *fresh = new (memory) State;
		State *empty = nullptr;
		if (!state.compare_exchange_strong(empty, fresh, std::memory_order_acq_rel)) {
			fresh->~State();
			std::free(memory);
		}
		p = state.load(std::memory_order_acquire);
	}
	std::lock_guard<std::mutex> guard(p->lifecycle_mutex);
	if (p->enabled.load(std::memory_order_acquire)) {
		return false;
	}
	// Stop waits for all entered callbacks before the next reset.
	while (p->active_hooks.load(std::memory_order_acquire) != 0) {
		std::this_thread::yield();
	}
	// No hooks are active; clear all bounded state.
	{
		std::lock_guard<std::mutex> stack_guard(p->callstack_mutex);
		for (StackEntry &entry : p->callstacks) {
			entry = StackEntry();
		}
	}
	for (Shard &shard : p->shards) {
		std::lock_guard<std::mutex> shard_guard(shard.mutex);
		for (Slot &s : shard.slots) {
			s = Slot();
		}
	}
	for (Shard &shard : p->gpu_shards) {
		std::lock_guard<std::mutex> shard_guard(shard.mutex);
		for (Slot &s : shard.slots) {
			s = Slot();
		}
	}
	for (Shard &shard : p->logical_shards) {
		std::lock_guard<std::mutex> shard_guard(shard.mutex);
		for (Slot &s : shard.slots) {
			s = Slot();
		}
	}
	{
		std::lock_guard<std::mutex> event_guard(p->event_mutex);
		for (Event &e : p->event_ring) {
			e = Event();
		}
	}
	p->sequence.store(0, std::memory_order_relaxed);
	p->frame_id.store(0, std::memory_order_relaxed);
	p->live_bytes.store(0, std::memory_order_relaxed);
	p->peak_bytes.store(0, std::memory_order_relaxed);
	p->live_allocations.store(0, std::memory_order_relaxed);
	p->gpu_live_bytes.store(0, std::memory_order_relaxed);
	p->gpu_peak_bytes.store(0, std::memory_order_relaxed);
	p->gpu_live_allocations.store(0, std::memory_order_relaxed);
	p->gpu_total_allocations.store(0, std::memory_order_relaxed);
	p->logical_live_bytes.store(0, std::memory_order_relaxed);
	p->logical_peak_bytes.store(0, std::memory_order_relaxed);
	p->logical_live_allocations.store(0, std::memory_order_relaxed);
	p->logical_total_allocations.store(0, std::memory_order_relaxed);
	p->gpu_driver_allocated_bytes.store(0, std::memory_order_relaxed);
	p->gpu_reserved_bytes.store(0, std::memory_order_relaxed);
	p->gpu_budget_bytes.store(0, std::memory_order_relaxed);
	p->gpu_budget_supported.store(false, std::memory_order_relaxed);
	p->total_allocations.store(0, std::memory_order_relaxed);
	p->dropped_records.store(0, std::memory_order_relaxed);
	p->overwritten_events.store(0, std::memory_order_relaxed);
	p->capture_id.fetch_add(1, std::memory_order_relaxed);
	p->enabled.store(true, std::memory_order_release);
	return true;
}

void stop() {
	State *p = state.load(std::memory_order_acquire);
	if (p) {
		std::lock_guard<std::mutex> lifecycle(p->lifecycle_mutex);
		p->enabled.store(false, std::memory_order_release);
		while (p->active_hooks.load(std::memory_order_acquire) != 0) {
			std::this_thread::yield();
		}
	}
}

Summary summary() {
	Summary result;
	State *p = state.load(std::memory_order_acquire);
	if (!p) {
		return result;
	}
	result.capture_id = p->capture_id.load(std::memory_order_relaxed);
	result.sequence = p->sequence.load(std::memory_order_relaxed);
	result.live_bytes = p->live_bytes.load(std::memory_order_relaxed);
	result.peak_bytes = p->peak_bytes.load(std::memory_order_relaxed);
	result.live_allocations = p->live_allocations.load(std::memory_order_relaxed);
	result.total_allocations = p->total_allocations.load(std::memory_order_relaxed);
	result.dropped_records = p->dropped_records.load(std::memory_order_relaxed);
	result.overwritten_events = p->overwritten_events.load(std::memory_order_relaxed);
	result.gpu_allocation_bytes = p->gpu_live_bytes.load(std::memory_order_relaxed);
	result.gpu_peak_bytes = p->gpu_peak_bytes.load(std::memory_order_relaxed);
	result.gpu_live_allocations = p->gpu_live_allocations.load(std::memory_order_relaxed);
	result.gpu_logical_bytes = p->logical_live_bytes.load(std::memory_order_relaxed);
	result.gpu_logical_peak_bytes = p->logical_peak_bytes.load(std::memory_order_relaxed);
	result.gpu_logical_allocations = p->logical_live_allocations.load(std::memory_order_relaxed);
	result.gpu_driver_allocated_bytes = p->gpu_driver_allocated_bytes.load(std::memory_order_relaxed);
	result.gpu_reserved_bytes = p->gpu_reserved_bytes.load(std::memory_order_relaxed);
	result.gpu_budget_bytes = p->gpu_budget_bytes.load(std::memory_order_relaxed);
	result.gpu_budget_supported = p->gpu_budget_supported.load(std::memory_order_relaxed);
	result.active = p->enabled.load(std::memory_order_acquire);
	result.incomplete = result.dropped_records != 0 || result.overwritten_events != 0;
	return result;
}

static size_t enumerate_records(size_t p_offset, Record *r_records, size_t p_capacity, int p_domain) {
	State *p = state.load(std::memory_order_acquire);
	if (!p || !r_records || !p_capacity || p->enabled.load(std::memory_order_acquire)) {
		return 0;
	}
	size_t skipped = 0;
	size_t count = 0;
	for (Shard &shard : (p_domain == 2 ? p->logical_shards : p_domain == 1 ? p->gpu_shards
																		   : p->shards)) {
		std::lock_guard<std::mutex> guard(shard.mutex);
		for (const Slot &s : shard.slots) {
			if (s.address <= TOMBSTONE) {
				continue;
			}
			if (skipped++ < p_offset) {
				continue;
			}
			if (count == p_capacity) {
				return count;
			}
			r_records[count++] = { s.allocation_id, s.address, s.bytes, s.category, s.generation, s.stack_id };
		}
	}
	return count;
}

size_t live_records(size_t p_offset, Record *r_records, size_t p_capacity) {
	return enumerate_records(p_offset, r_records, p_capacity, 0);
}

size_t gpu_live_records(size_t p_offset, Record *r_records, size_t p_capacity) {
	return enumerate_records(p_offset, r_records, p_capacity, 1);
}

size_t gpu_logical_live_records(size_t p_offset, Record *r_records, size_t p_capacity) {
	return enumerate_records(p_offset, r_records, p_capacity, 2);
}

size_t callstack(uint32_t p_stack_id, uintptr_t *r_frames, size_t p_capacity) {
	State *p = state.load(std::memory_order_acquire);
	if (!p || !r_frames || !p_capacity || p_stack_id == 0 || p_stack_id > CALLSTACK_CAPACITY) {
		return 0;
	}
	std::lock_guard<std::mutex> guard(p->callstack_mutex);
	const StackEntry &entry = p->callstacks[p_stack_id - 1];
	if (entry.id != p_stack_id) {
		return 0;
	}
	const size_t count = entry.depth < p_capacity ? entry.depth : p_capacity;
	for (size_t i = 0; i < count; ++i) {
		r_frames[i] = entry.frames[i];
	}
	return count;
}

size_t events(uint64_t p_first, Event *r_events, size_t p_capacity) {
	State *p = state.load(std::memory_order_acquire);
	if (!p || !r_events || !p_capacity) {
		return 0;
	}
	std::lock_guard<std::mutex> guard(p->event_mutex);
	const uint64_t latest = p->sequence.load(std::memory_order_acquire);
	const uint64_t first = latest >= EVENT_CAPACITY ? latest - EVENT_CAPACITY + 1 : 1;
	uint64_t seq = p_first > first ? p_first : first;
	size_t count = 0;
	while (seq <= latest && count < p_capacity) {
		const Event &e = p->event_ring[(seq - 1) % EVENT_CAPACITY];
		if (e.sequence == seq) {
			r_events[count++] = e;
		}
		++seq;
	}
	return count;
}

// The second enabled check closes the stop/callback race: stop may disable
// tracing after the first check but before a callback enters the ledger.
static State *enter_hook() {
	State *p = state.load(std::memory_order_acquire);
	if (!p || !p->enabled.load(std::memory_order_acquire)) {
		return nullptr;
	}
	p->active_hooks.fetch_add(1, std::memory_order_acq_rel);
	if (!p->enabled.load(std::memory_order_acquire)) {
		p->active_hooks.fetch_sub(1, std::memory_order_release);
		return nullptr;
	}
	return p;
}

void record_gpu_alloc(void *p_allocation, size_t p_bytes) {
	State *p = enter_hook();
	if (p) {
		add_record(p, (uintptr_t)p_allocation, p_bytes, RecordKind::ALLOC, true);
		p->active_hooks.fetch_sub(1, std::memory_order_release);
	}
}

void record_gpu_free(void *p_allocation) {
	State *p = enter_hook();
	if (p) {
		remove_record(p, (uintptr_t)p_allocation, true);
		p->active_hooks.fetch_sub(1, std::memory_order_release);
	}
}

void publish_gpu_budget(uint64_t p_allocated, uint64_t p_reserved, uint64_t p_budget, bool p_supported) {
	State *p = enter_hook();
	if (p) {
		p->gpu_driver_allocated_bytes.store(p_allocated, std::memory_order_relaxed);
		p->gpu_reserved_bytes.store(p_reserved, std::memory_order_relaxed);
		p->gpu_budget_bytes.store(p_budget, std::memory_order_relaxed);
		p->gpu_budget_supported.store(p_supported, std::memory_order_release);
		p->active_hooks.fetch_sub(1, std::memory_order_release);
	}
}

void record_gpu_logical_alloc(void *p_rid, size_t p_bytes) {
	State *p = enter_hook();
	if (p) {
		add_record(p, (uintptr_t)p_rid, p_bytes, RecordKind::ALLOC, false, true);
		p->active_hooks.fetch_sub(1, std::memory_order_release);
	}
}

void record_gpu_logical_free(void *p_rid) {
	State *p = enter_hook();
	if (p) {
		remove_record(p, (uintptr_t)p_rid, false, true);
		p->active_hooks.fetch_sub(1, std::memory_order_release);
	}
}

void record_alloc(void *p_ptr, size_t p_bytes) {
	State *p = enter_hook();
	if (p) {
		add_record(p, (uintptr_t)p_ptr, p_bytes, RecordKind::ALLOC);
		p->active_hooks.fetch_sub(1, std::memory_order_release);
	}
}

void record_free(void *p_ptr) {
	State *p = enter_hook();
	if (p) {
		remove_record(p, (uintptr_t)p_ptr);
		p->active_hooks.fetch_sub(1, std::memory_order_release);
	}
}

void record_realloc(uintptr_t p_old_address, void *p_new_ptr, size_t p_bytes, bool p_success) {
	if (!p_success) {
		return;
	}
	State *p = enter_hook();
	if (p) {
		remove_record(p, p_old_address);
		if (p_new_ptr && p_bytes) {
			add_record(p, (uintptr_t)p_new_ptr, p_bytes, RecordKind::REALLOC);
		}
		p->active_hooks.fetch_sub(1, std::memory_order_release);
	}
}

void publish_render_frame_us(uint64_t p_microseconds) {
	completed_render_frame_us.store(p_microseconds, std::memory_order_release);
}

uint64_t last_render_frame_us() {
	return completed_render_frame_us.load(std::memory_order_acquire);
}

void publish_completed_render_frame(uint64_t p_frame_id, uint64_t p_microseconds) {
	std::lock_guard<std::mutex> lock(completed_render_frame_mutex);
	completed_render_frame_sample.frame_id = p_frame_id;
	completed_render_frame_sample.cpu_duration_us = p_microseconds;
	completed_render_frame_us.store(p_microseconds, std::memory_order_release);
}

CompletedRenderFrameSample last_completed_render_frame() {
	std::lock_guard<std::mutex> lock(completed_render_frame_mutex);
	return completed_render_frame_sample;
}

void set_frame(uint64_t p_frame) {
	State *p = state.load(std::memory_order_acquire);
	if (p) {
		p->frame_id.store(p_frame, std::memory_order_relaxed);
	}
}

uint32_t category() {
	return active_category;
}

void set_category(uint32_t p_category) {
	active_category = p_category;
}

} // namespace GodotMemoryTracking
