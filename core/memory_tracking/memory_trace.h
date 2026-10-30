/**************************************************************************/
/*  memory_trace.h                                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.        */
/**************************************************************************/

#pragma once

#include "memory_tracker.h"

namespace GodotMemoryTracking {

// Bounded, little-endian .gdmemtrace v1 streaming codec.
// Exported trace records intentionally do not contain process pointers.
struct TraceVisitor {
	bool (*on_event)(const Event &, void *) = nullptr;
	bool (*on_record)(const Record &, bool p_gpu, void *) = nullptr;
	bool (*on_summary)(const Summary &, void *) = nullptr;
	void *userdata = nullptr;
};

using TraceSink = bool (*)(const uint8_t *p_bytes, size_t p_size, void *p_userdata);

// Only stopped captures can be exported. Partial event history is explicitly
// marked incomplete in the footer. Failure of a sink immediately ends export.
bool write_trace(TraceSink p_sink, void *p_userdata);

// Import validates magic, version, CRCs, chunk lengths, final hash and footer.
// Inputs are capped to 32 MiB. No raw addresses are deserialized into pointers.
bool read_trace(const uint8_t *p_bytes, size_t p_size, const TraceVisitor &p_visitor);

} // namespace GodotMemoryTracking
