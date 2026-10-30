/**************************************************************************/
/*  memory_tracker.h                                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/*                                                                          */
/* Permission is hereby granted, free of charge, to any person obtaining   */
/* a copy of this software and associated documentation files (the         */
/* "Software"), to deal in the Software without restriction, including      */
/* without limitation the rights to use, copy, modify, merge, publish,     */
/* distribute, sublicense, and/or sell copies of the Software, and to      */
/* permit persons to whom the Software is furnished to do so, subject to   */
/* the following conditions:                                               */
/*                                                                          */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.        */
/**************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>

namespace GodotMemoryTracking {

enum AllocationCategory : uint32_t {
	CATEGORY_UNCLASSIFIED = 0,
	CATEGORY_VULKAN_BUFFER = 100,
	CATEGORY_VULKAN_TEXTURE = 101,
	CATEGORY_D3D12_BUFFER = 110,
	CATEGORY_D3D12_TEXTURE = 111,
	CATEGORY_RD_LOGICAL_BUFFER = 200,
	CATEGORY_RD_LOGICAL_TEXTURE = 201,
};

enum class RecordKind : uint8_t { ALLOC, FREE, REALLOC };

struct Record {
	uint64_t allocation_id = 0;
	uintptr_t address = 0;
	uint64_t bytes = 0;
	uint32_t category = 0;
	uint32_t generation = 0;
	uint32_t stack_id = 0;
};

struct Event {
	uint64_t sequence = 0;
	uint64_t allocation_id = 0;
	uintptr_t address = 0;
	uint64_t bytes = 0;
	uint32_t category = 0;
	RecordKind kind = RecordKind::ALLOC;
	bool gpu = false;
	uint32_t stack_id = 0;
};

struct Summary {
	uint64_t capture_id = 0;
	uint64_t sequence = 0;
	uint64_t live_bytes = 0;
	uint64_t peak_bytes = 0;
	uint64_t live_allocations = 0;
	uint64_t total_allocations = 0;
	uint64_t dropped_records = 0;
	uint64_t overwritten_events = 0;
	uint64_t gpu_allocation_bytes = 0;
	uint64_t gpu_peak_bytes = 0;
	uint64_t gpu_live_allocations = 0;
	uint64_t gpu_logical_bytes = 0;
	uint64_t gpu_logical_peak_bytes = 0;
	uint64_t gpu_logical_allocations = 0;
	bool active = false;
	bool incomplete = false;
};

// The bounded ledger is allocated with the C runtime, never Godot's tracked heap.
// Captures start with an empty ledger: allocations predating capture are not claimed.
// A stopped capture retains a readable snapshot until the next start.
// The tracker is a diagnostic tool, not an allocator or ownership authority.
bool start();
void stop();
Summary summary();

// Output is ordered by table slot, not allocation timestamp. Returns records
// written. Caller pages from offset 0 while inspecting one stopped capture.
size_t live_records(size_t p_offset, Record *r_records, size_t p_capacity);

// Recent event ring; returns only committed events with sequence >= p_first.
// Concurrent requests during capture are allowed but may see missing events.
size_t events(uint64_t p_first, Event *r_events, size_t p_capacity);

// Physical allocator suballocations (VMA/D3D12MA), distinct from logical RIDs.
void record_gpu_alloc(void *p_allocation, size_t p_bytes);
void record_gpu_free(void *p_allocation);
void record_gpu_logical_alloc(void *p_rid, size_t p_bytes);
void record_gpu_logical_free(void *p_rid);

void record_alloc(void *p_ptr, size_t p_bytes);
void record_free(void *p_ptr);
void record_realloc(void *p_old_ptr, void *p_new_ptr, size_t p_bytes, bool p_success);

size_t gpu_live_records(size_t p_offset, Record *r_records, size_t p_capacity);
size_t gpu_logical_live_records(size_t p_offset, Record *r_records, size_t p_capacity);
// Returns the bounded raw program-counter sample for an allocation stack ID.
// Only debugger requests should expose these addresses; traces omit them.
size_t callstack(uint32_t p_stack_id, uintptr_t *r_frames, size_t p_capacity);

uint32_t category();
void set_category(uint32_t p_category);

class ScopeCategory {
	uint32_t previous;

public:
	explicit ScopeCategory(uint32_t p_category) : previous(category()) { set_category(p_category); }
	~ScopeCategory() { set_category(previous); }
	ScopeCategory(const ScopeCategory &) = delete;
	ScopeCategory &operator=(const ScopeCategory &) = delete;
};

} // namespace GodotMemoryTracking