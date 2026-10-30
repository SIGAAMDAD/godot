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

enum class RecordKind : uint8_t { ALLOC, FREE, REALLOC };

struct Record {
	uint64_t allocation_id = 0;
	uintptr_t address = 0;
	uint64_t bytes = 0;
	uint32_t category = 0;
	uint32_t generation = 0;
};

struct Event {
	uint64_t sequence = 0;
	uint64_t allocation_id = 0;
	uintptr_t address = 0;
	uint64_t bytes = 0;
	uint32_t category = 0;
	RecordKind kind = RecordKind::ALLOC;
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

void record_alloc(void *p_ptr, size_t p_bytes);
void record_free(void *p_ptr);
void record_realloc(void *p_old_ptr, void *p_new_ptr, size_t p_bytes, bool p_success);

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