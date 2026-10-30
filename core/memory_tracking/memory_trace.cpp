/**************************************************************************/
/*  memory_trace.cpp                                                      */
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

#include "memory_trace.h"

#include <cstring>

namespace GodotMemoryTracking {
namespace {

constexpr size_t MAX_TRACE_BYTES = 32 * 1024 * 1024;
constexpr size_t MAX_CHUNK_BYTES = 4 * 1024 * 1024;
constexpr size_t HEADER_SIZE = 52;
constexpr size_t CHUNK_HEADER_SIZE = 24;
constexpr size_t ROW_SIZE = 32;
constexpr size_t FOOTER_SIZE = 100;
constexpr uint64_t HASH_BASIS = UINT64_C(14695981039346656037);
constexpr uint64_t HASH_PRIME = UINT64_C(1099511628211);

void put16(uint8_t *p, uint16_t value) {
	for (int i = 0; i < 2; ++i) {
		p[i] = uint8_t(value >> (8 * i));
	}
}
void put32(uint8_t *p, uint32_t value) {
	for (int i = 0; i < 4; ++i) {
		p[i] = uint8_t(value >> (8 * i));
	}
}
void put64(uint8_t *p, uint64_t value) {
	for (int i = 0; i < 8; ++i) {
		p[i] = uint8_t(value >> (8 * i));
	}
}
uint16_t get16(const uint8_t *p) {
	return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
uint32_t get32(const uint8_t *p) {
	uint32_t value = 0;
	for (int i = 0; i < 4; ++i) {
		value |= uint32_t(p[i]) << (8 * i);
	}
	return value;
}
uint64_t get64(const uint8_t *p) {
	uint64_t value = 0;
	for (int i = 0; i < 8; ++i) {
		value |= uint64_t(p[i]) << (8 * i);
	}
	return value;
}
uint32_t crc32(const uint8_t *p_bytes, size_t p_size) {
	uint32_t crc = UINT32_MAX;
	for (size_t i = 0; i < p_size; ++i) {
		crc ^= p_bytes[i];
		for (int bit = 0; bit < 8; ++bit) {
			crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0xedb88320) : 0);
		}
	}
	return ~crc;
}
uint64_t update_hash(uint64_t p_hash, const uint8_t *p_bytes, size_t p_size) {
	for (size_t i = 0; i < p_size; ++i) {
		p_hash = (p_hash ^ p_bytes[i]) * HASH_PRIME;
	}
	return p_hash;
}

struct Writer {
	TraceSink sink = nullptr;
	void *userdata = nullptr;
	uint64_t hash = HASH_BASIS;
	size_t total_bytes = 0;

	bool emit(const uint8_t *p_data, size_t p_length, bool p_hash = true) {
		if (!sink || p_length > MAX_TRACE_BYTES - total_bytes) {
			return false;
		}
		if (!sink(p_data, p_length, userdata)) {
			return false;
		}
		total_bytes += p_length;
		if (p_hash) {
			hash = update_hash(hash, p_data, p_length);
		}
		return true;
	}

	bool chunk(uint16_t p_type, uint64_t p_first, uint32_t p_count, const uint8_t *p_data, uint32_t p_size, bool p_hash = true) {
		if (p_size > MAX_CHUNK_BYTES || p_size > MAX_TRACE_BYTES - CHUNK_HEADER_SIZE) {
			return false;
		}
		uint8_t header[CHUNK_HEADER_SIZE] = {};
		put32(header, p_size);
		put16(header + 4, p_type);
		put64(header + 8, p_first);
		put32(header + 16, p_count);
		put32(header + 20, crc32(p_data, p_size));
		return emit(header, sizeof(header), p_hash) && emit(p_data, p_size, p_hash);
	}
};

void write_event(uint8_t *p_dst, const Event &p_event) {
	put64(p_dst, p_event.sequence);
	put64(p_dst + 8, p_event.allocation_id);
	put64(p_dst + 16, p_event.bytes);
	put32(p_dst + 24, p_event.category);
	p_dst[28] = uint8_t(p_event.kind);
	p_dst[29] = p_event.gpu ? 1 : 0;
	p_dst[30] = 0;
	p_dst[31] = 0;
}
void write_record(uint8_t *p_dst, const Record &p_record, bool p_gpu) {
	put64(p_dst, p_record.allocation_id);
	put64(p_dst + 8, p_record.bytes);
	put32(p_dst + 16, p_record.category);
	put32(p_dst + 20, p_record.generation);
	p_dst[24] = p_gpu ? 1 : 0;
	std::memset(p_dst + 25, 0, 7);
}
}

bool write_trace(TraceSink p_sink, void *p_userdata) {
	const Summary begin = summary();
	if (begin.active || begin.capture_id == 0 || !p_sink) {
		return false;
	}
	Writer writer;
	writer.sink = p_sink;
	writer.userdata = p_userdata;
	uint8_t header[HEADER_SIZE] = {};
	std::memcpy(header, "GDMEM001", 8);
	put16(header + 8, 1);
	put64(header + 24, begin.capture_id);
	put64(header + 32, 1000000); // microsecond tick frequency
	put32(header + 48, crc32(header, 48));
	if (!writer.emit(header, sizeof(header))) {
		return false;
	}

	uint8_t payload[ROW_SIZE * 64];
	Event page[64];
	uint64_t next = 1;
	while (true) {
		const size_t count = events(next, page, 64);
		if (!count) {
			break;
		}
		for (size_t i = 0; i < count; ++i) {
			write_event(payload + ROW_SIZE * i, page[i]);
		}
		if (!writer.chunk(1, page[0].sequence, uint32_t(count), payload, uint32_t(ROW_SIZE * count))) {
			return false;
		}
		next = page[count - 1].sequence + 1;
		if (count < 64) {
			break;
		}
	}
	Record records[64];
	for (int gpu = 0; gpu < 2; ++gpu) {
		size_t offset = 0;
		while (true) {
			const size_t count = gpu ? gpu_live_records(offset, records, 64) : live_records(offset, records, 64);
			if (!count) {
				break;
			}
			for (size_t i = 0; i < count; ++i) {
				write_record(payload + ROW_SIZE * i, records[i], gpu != 0);
			}
			if (!writer.chunk(8, uint64_t(offset), uint32_t(count), payload, uint32_t(ROW_SIZE * count))) {
				return false;
			}
			offset += count;
			if (count < 64) {
				break;
			}
		}
	}

	const Summary end = summary();
	if (end.active || end.capture_id != begin.capture_id) {
		return false; // A restarted capture can never be mixed into this trace.
	}
	uint8_t footer[FOOTER_SIZE] = {};
	const uint64_t counts[] = { end.capture_id, end.sequence, end.live_bytes, end.peak_bytes,
		end.live_allocations, end.total_allocations, end.dropped_records, end.overwritten_events,
		end.gpu_allocation_bytes, end.gpu_peak_bytes, end.gpu_live_allocations };
	for (size_t i = 0; i < 11; ++i) {
		put64(footer + i * 8, counts[i]);
	}
	put32(footer + 88, end.incomplete ? 1 : 0);
	put64(footer + 92, writer.hash);
	return writer.chunk(7, end.sequence, 1, footer, sizeof(footer), false);
}

bool read_trace(const uint8_t *p_bytes, size_t p_size, const TraceVisitor &p_visitor) {
	if (!p_bytes || p_size < HEADER_SIZE + CHUNK_HEADER_SIZE + FOOTER_SIZE || p_size > MAX_TRACE_BYTES ||
			std::memcmp(p_bytes, "GDMEM001", 8) != 0 || get16(p_bytes + 8) != 1 ||
			get16(p_bytes + 10) != 0 || get32(p_bytes + 12) != 0 ||
			get32(p_bytes + 48) != crc32(p_bytes, 48)) {
		return false;
	}
	const uint64_t capture = get64(p_bytes + 24);
	uint64_t hash = update_hash(HASH_BASIS, p_bytes, HEADER_SIZE);
	uint64_t last_seq = 0;
	size_t offset = HEADER_SIZE;
	bool completed = false;
	while (offset + CHUNK_HEADER_SIZE <= p_size) {
		const uint8_t *h = p_bytes + offset;
		const uint32_t len = get32(h);
		const uint16_t type = get16(h + 4);
		const uint32_t count = get32(h + 16);
		if (len > MAX_CHUNK_BYTES || len > p_size - offset - CHUNK_HEADER_SIZE || get16(h + 6) != 0) {
			return false;
		}
		const uint8_t *data = h + CHUNK_HEADER_SIZE;
		if (crc32(data, len) != get32(h + 20)) {
			return false;
		}
		if (type == 1 || type == 8) {
			if (count != len / ROW_SIZE || len % ROW_SIZE != 0) {
				return false;
			}
			for (size_t i = 0; i < count; ++i) {
				const uint8_t *row = data + i * ROW_SIZE;
				if (type == 1) {
					Event event;
					event.sequence = get64(row);
					event.allocation_id = get64(row + 8);
					event.bytes = get64(row + 16);
					event.category = get32(row + 24);
					if (row[28] > 2 || row[29] > 1 || event.sequence <= last_seq) {
						return false;
					}
					event.kind = RecordKind(row[28]);
					event.gpu = row[29] != 0;
					last_seq = event.sequence;
					if (p_visitor.on_event && !p_visitor.on_event(event, p_visitor.userdata)) {
						return false;
					}
				} else {
					Record record;
					record.allocation_id = get64(row);
					record.bytes = get64(row + 8);
					record.category = get32(row + 16);
					record.generation = get32(row + 20);
					if (row[24] > 1) {
						return false;
					}
					if (p_visitor.on_record && !p_visitor.on_record(record, row[24] != 0, p_visitor.userdata)) {
						return false;
					}
				}
			}
		} else if (type == 7) {
			if (completed || count != 1 || len != FOOTER_SIZE || get64(data) != capture ||
					get64(data + 92) != hash || offset + CHUNK_HEADER_SIZE + len != p_size) {
				return false;
			}
			Summary s;
			s.capture_id = capture;
			s.sequence = get64(data + 8);
			s.live_bytes = get64(data + 16);
			s.peak_bytes = get64(data + 24);
			s.live_allocations = get64(data + 32);
			s.total_allocations = get64(data + 40);
			s.dropped_records = get64(data + 48);
			s.overwritten_events = get64(data + 56);
			s.gpu_allocation_bytes = get64(data + 64);
			s.gpu_peak_bytes = get64(data + 72);
			s.gpu_live_allocations = get64(data + 80);
			s.incomplete = get32(data + 88) != 0;
			if (p_visitor.on_summary && !p_visitor.on_summary(s, p_visitor.userdata)) {
				return false;
			}
			completed = true;
		} else if (type & 0x8000) {
			return false; // Unknown required chunk.
		}
		if (type != 7) {
			hash = update_hash(hash, h, CHUNK_HEADER_SIZE + len);
		}
		offset += CHUNK_HEADER_SIZE + len;
	}
	return completed && offset == p_size;
}

} // namespace GodotMemoryTracking
