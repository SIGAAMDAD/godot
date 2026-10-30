/**************************************************************************/
/*  terrain3d_forward_plus_bridge.cpp                                     */
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

#include "terrain3d_forward_plus_bridge.h"

#include "core/object/callable_mp.h"
#include "core/os/memory.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"

#if T3D_NATIVE_BRIDGE_ENABLED
Terrain3DForwardPlusBridge *Terrain3DForwardPlusBridge::singleton = nullptr;

void Terrain3DForwardPlusBridge::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_abi_version"), &Terrain3DForwardPlusBridge::get_abi_version);
	ClassDB::bind_method(D_METHOD("get_native_capabilities"), &Terrain3DForwardPlusBridge::get_native_capabilities);
	ClassDB::bind_method(D_METHOD("get_deferred_upload_abi_version"), &Terrain3DForwardPlusBridge::get_deferred_upload_abi_version);
	ClassDB::bind_method(D_METHOD("defer_native_buffer_update", "buffer", "offset", "size", "fill_address", "user_data_address"), &Terrain3DForwardPlusBridge::defer_native_buffer_update);
	ClassDB::bind_method(D_METHOD("get_native_device_frame"), &Terrain3DForwardPlusBridge::get_native_device_frame);
	ClassDB::bind_method(D_METHOD("complete_native_hot_frame"), &Terrain3DForwardPlusBridge::complete_native_hot_frame);
	ClassDB::bind_method(D_METHOD("get_persistent_hot_abi_version"), &Terrain3DForwardPlusBridge::get_persistent_hot_abi_version);
	ClassDB::bind_method(D_METHOD("get_native_hot_backing_size", "logical_bytes"), &Terrain3DForwardPlusBridge::get_native_hot_backing_size);
	ClassDB::bind_method(D_METHOD("configure_native_hot_staging", "logical_bytes"), &Terrain3DForwardPlusBridge::configure_native_hot_staging);
	ClassDB::bind_method(D_METHOD("defer_native_buffer_flush", "buffer", "seal_address", "user_data_address"), &Terrain3DForwardPlusBridge::defer_native_buffer_flush);
	ClassDB::bind_method(D_METHOD("has_native_callback"), &Terrain3DForwardPlusBridge::has_native_callback);
	ClassDB::bind_method(D_METHOD("get_last_error"), &Terrain3DForwardPlusBridge::get_last_error);
#if defined(TOOLS_ENABLED) || defined(DEBUG_ENABLED)
	ClassDB::bind_method(D_METHOD("get_environment_flags"), &Terrain3DForwardPlusBridge::get_environment_flags);
	ClassDB::bind_method(D_METHOD("get_environment_radiance_valid"), &Terrain3DForwardPlusBridge::get_environment_radiance_valid);
	ClassDB::bind_method(D_METHOD("get_environment_frame_generation"), &Terrain3DForwardPlusBridge::get_environment_frame_generation);
	ClassDB::bind_method(D_METHOD("get_environment_resource_generation"), &Terrain3DForwardPlusBridge::get_environment_resource_generation);
#endif
	ClassDB::bind_method(D_METHOD("register_native_callback", "prepare_address", "shadow_prepare_address", "submit_address", "user_data_address", "frame_size", "resource_size", "pass_size", "shadow_size"), &Terrain3DForwardPlusBridge::register_native_callback);
	ClassDB::bind_method(D_METHOD("unregister_native_callback", "prepare_address", "shadow_prepare_address", "submit_address", "user_data_address"), &Terrain3DForwardPlusBridge::unregister_native_callback);
	ClassDB::bind_method(D_METHOD("collect_retired_callbacks"), &Terrain3DForwardPlusBridge::collect_retired_callbacks);
}

void Terrain3DForwardPlusBridge::_dispatch_rd_request(int64_t p_request_address) {
	RDRequest &request = *reinterpret_cast<RDRequest *>(uintptr_t(uint64_t(p_request_address)));
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd != nullptr && rd->is_on_render_thread()) {
		switch (request.kind) {
			case RD_COMPLETE: {
				NativeCallbackSlot *slot = active_slot.load(std::memory_order_acquire);
				if (slot == nullptr || !slot->inside_prepare) {
					request.result = rd->_terrain3d_complete_hot();
				}
			} break;
			case RD_BACKING_SIZE: {
				const uint64_t size = rd->_terrain3d_hot_backing_size(uint32_t(request.value));
				request.result = size <= INT64_MAX ? int64_t(size) : 0;
			} break;
			case RD_CONFIGURE_STAGING: {
				if (!has_native_callback()) {
					request.result = rd->_terrain3d_configure_staging(uint32_t(request.value)) == OK;
				}
			} break;
			case RD_DEFAULT_STAGING: {
				request.result = rd->terrain3d_upload_staging_capacity != 0 ||
						(!has_native_callback() && rd->_terrain3d_configure_staging(2220032) == OK);
			} break;
		}
	}
	request.done.post();
}

int64_t Terrain3DForwardPlusBridge::_request_rd(RDRequestKind p_kind, int64_t p_value) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, 0);
	RDRequest request;
	request.kind = p_kind;
	request.value = p_value;
	const int64_t address = int64_t(uintptr_t(&request));
	if (rd->is_on_render_thread()) {
		_dispatch_rd_request(address);
	} else {
		RenderingServer *server = RenderingServer::get_singleton();
		ERR_FAIL_NULL_V(server, 0);
		server->call_on_render_thread(callable_mp(this, &Terrain3DForwardPlusBridge::_dispatch_rd_request).bind(address));
		request.done.wait();
	}
	return request.result;
}

int64_t Terrain3DForwardPlusBridge::get_deferred_upload_abi_version() {
	// Initialization-time discovery configures all ordinary staging slots before
	// the first PREPARE. Custom budgets can be configured before registration.
	return _request_rd(RD_DEFAULT_STAGING) != 0 ? T3D_FORWARD_DEFERRED_UPLOAD_ABI : 0;
}

int64_t Terrain3DForwardPlusBridge::get_native_device_frame() const {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, -1);
	ERR_FAIL_COND_V(!rd->is_on_render_thread(), -1);
	return int64_t(rd->get_frames_drawn());
}

bool Terrain3DForwardPlusBridge::complete_native_hot_frame() {
	return _request_rd(RD_COMPLETE) != 0;
}

int64_t Terrain3DForwardPlusBridge::get_persistent_hot_abi_version() const {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	return rd != nullptr && rd->driver->buffer_supports_persistent_hot() ? T3D_FORWARD_PERSISTENT_HOT_ABI : 0;
}

int64_t Terrain3DForwardPlusBridge::get_native_hot_backing_size(int64_t p_logical_bytes) {
	ERR_FAIL_COND_V(p_logical_bytes <= 0 || uint64_t(p_logical_bytes) > UINT32_MAX, 0);
	return _request_rd(RD_BACKING_SIZE, p_logical_bytes);
}

bool Terrain3DForwardPlusBridge::configure_native_hot_staging(int64_t p_logical_bytes) {
	ERR_FAIL_COND_V(p_logical_bytes <= 0 || uint64_t(p_logical_bytes) > UINT32_MAX, false);
	return _request_rd(RD_CONFIGURE_STAGING, p_logical_bytes) != 0;
}

bool Terrain3DForwardPlusBridge::defer_native_buffer_flush(RID p_buffer, int64_t p_seal_address, int64_t p_user_data_address) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	ERR_FAIL_COND_V(!rd->is_on_render_thread(), false);
	NativeCallbackSlot *slot = active_slot.load(std::memory_order_acquire);
	ERR_FAIL_NULL_V(slot, false);
	ERR_FAIL_COND_V(!slot->inside_prepare || slot->prepare_count != 1 || p_seal_address == 0 || p_user_data_address == 0, false);
	ERR_FAIL_COND_V(slot->deferred_render_frame == slot->prepare_render_frame || slot->deferred_pending.load(std::memory_order_acquire), false);
	auto *buffer = rd->_get_buffer_from_owner(p_buffer);
	ERR_FAIL_NULL_V(buffer, false);
	const Error err = rd->_terrain3d_buffer_flush_deferred(p_buffer, _seal_persistent_hot, slot);
	ERR_FAIL_COND_V(err != OK, false);
	slot->deferred_seal = reinterpret_cast<t3dHotSealFn>(uintptr_t(uint64_t(p_seal_address)));
	slot->deferred_user_data = reinterpret_cast<void *>(uintptr_t(uint64_t(p_user_data_address)));
	slot->deferred_render_frame = slot->prepare_render_frame;
	slot->deferred_info = { T3D_FORWARD_PERSISTENT_HOT_ABI, sizeof(t3dBridgeHotFrameInfo), slot->prepare_render_frame,
		rd->get_frames_drawn(), rd->get_frame_slot(), rd->get_frame_delay(), buffer->size, 0 };
	slot->deferred_pending.store(true, std::memory_order_release);
	return true;
}

bool Terrain3DForwardPlusBridge::_seal_persistent_hot(void *p_userdata) {
	NativeCallbackSlot *slot = static_cast<NativeCallbackSlot *>(p_userdata);
	const bool success = slot->deferred_seal(slot->deferred_user_data, &slot->deferred_info);
	const bool delete_slot = slot->delete_after_upload.load(std::memory_order_acquire);
	slot->deferred_pending.store(false, std::memory_order_release);
	if (delete_slot) {
		memdelete(slot);
	}
	return success;
}

void Terrain3DForwardPlusBridge::_set_error_locked(const String &p_error) {
	last_error = p_error;
	if (!p_error.is_empty()) {
		ERR_PRINT("Terrain3DForwardPlusBridge: " + p_error);
	}
}

bool Terrain3DForwardPlusBridge::has_native_callback() const {
	return active_slot.load(std::memory_order_acquire) != nullptr;
}

String Terrain3DForwardPlusBridge::get_last_error() {
	MutexLock lock(state_mutex);
	return last_error;
}

bool Terrain3DForwardPlusBridge::register_native_callback(int64_t p_prepare_address, int64_t p_shadow_prepare_address,
		int64_t p_submit_address, int64_t p_user_data_address, int64_t p_frame_size,
		int64_t p_resource_size, int64_t p_pass_size, int64_t p_shadow_size) {
	MutexLock lock(state_mutex);

	if (p_prepare_address == 0 || p_shadow_prepare_address == 0 || p_submit_address == 0 || p_user_data_address == 0) {
		_set_error_locked("All ABI 12 callback and user-data addresses must be nonzero.");
		return false;
	}
	if (active_slot.load(std::memory_order_relaxed) != nullptr) {
		_set_error_locked("A native bridge is already registered.");
		return false;
	}
	if (!retired_slots.is_empty()) {
		_set_error_locked("Retired callbacks must be collected after render-thread synchronization before registering again.");
		return false;
	}

	if (p_frame_size != int64_t(sizeof(t3dBridgeFrame)) ||
			p_resource_size != int64_t(sizeof(t3dBridgeSceneResources)) ||
			p_pass_size != int64_t(sizeof(t3dBridgePass)) ||
			p_shadow_size != int64_t(sizeof(t3dBridgeShadow))) {
		_set_error_locked("One or more native packet sizes do not match ABI 12 (568/136/48/200).");
		return false;
	}

	NativeCallbackSlot *slot = memnew(NativeCallbackSlot);
	slot->prepare = reinterpret_cast<t3dPrepareFn>(uintptr_t(uint64_t(p_prepare_address)));
	slot->prepare_shadow = reinterpret_cast<t3dPrepareShadowFn>(uintptr_t(uint64_t(p_shadow_prepare_address)));
	slot->submit = reinterpret_cast<t3dSubmitFn>(uintptr_t(uint64_t(p_submit_address)));
	slot->user_data = reinterpret_cast<void *>(uintptr_t(uint64_t(p_user_data_address)));
	active_slot.store(slot, std::memory_order_release);
	_set_error_locked(String());
	return true;
}

bool Terrain3DForwardPlusBridge::_begin_prepare(NativeCallbackSlot *p_slot, uint64_t p_render_frame) const {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_COND_V(rd != nullptr && rd->terrain3d_hot_failed, false);
	const uint64_t render_frame = p_render_frame;
	ERR_FAIL_COND_V_MSG(p_slot->deferred_info.abi_version != 0 && p_slot->deferred_render_frame == render_frame && !p_slot->deferred_pending.load(std::memory_order_acquire), false,
			"Terrain3D cannot append passes after its deferred upload has been finalized.");
	if (p_slot->prepare_render_frame != render_frame) {
		p_slot->prepare_render_frame = render_frame;
		p_slot->prepare_count = 0;
	}
	p_slot->prepare_count++;
	p_slot->inside_prepare = true;
	return true;
}

bool Terrain3DForwardPlusBridge::defer_native_buffer_update(RID p_buffer, int64_t p_offset, int64_t p_size,
		int64_t p_fill_address, int64_t p_user_data_address) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	ERR_FAIL_COND_V(!rd->is_on_render_thread(), false);
	NativeCallbackSlot *slot = active_slot.load(std::memory_order_acquire);
	ERR_FAIL_NULL_V(slot, false);
	ERR_FAIL_COND_V_MSG(!slot->inside_prepare || slot->prepare_count != 1, false,
			"Reserve the Terrain3D HOT upload in the first PREPARE, before recording GPU consumers.");
	ERR_FAIL_COND_V(p_fill_address == 0 || p_user_data_address == 0 || p_offset < 0 || p_size <= 0 ||
					uint64_t(p_offset) > UINT32_MAX || uint64_t(p_size) > UINT32_MAX,
			false);
	const uint64_t render_frame = slot->prepare_render_frame;
	ERR_FAIL_COND_V_MSG(slot->deferred_render_frame == render_frame || slot->deferred_pending.load(std::memory_order_acquire), false,
			"Only one Terrain3D HOT upload is allowed per render frame.");
	const Error err = rd->_terrain3d_buffer_update_deferred(p_buffer, uint32_t(p_offset), uint32_t(p_size), _fill_deferred_upload, slot);
	ERR_FAIL_COND_V(err != OK, false);
	slot->deferred_fill = reinterpret_cast<t3dDeferredUploadFn>(uintptr_t(uint64_t(p_fill_address)));
	slot->deferred_user_data = reinterpret_cast<void *>(uintptr_t(uint64_t(p_user_data_address)));
	slot->deferred_render_frame = render_frame;
	slot->deferred_info = { T3D_FORWARD_DEFERRED_UPLOAD_ABI, sizeof(t3dBridgeDeferredUpload), render_frame,
		rd->get_frames_drawn(), rd->get_frame_slot(), rd->get_frame_delay(), uint32_t(p_size), 0 };
	slot->deferred_pending.store(true, std::memory_order_release);
	return true;
}

void Terrain3DForwardPlusBridge::_fill_deferred_upload(void *p_userdata, uint8_t *p_data, uint32_t p_size) {
	NativeCallbackSlot *slot = static_cast<NativeCallbackSlot *>(p_userdata);
	DEV_ASSERT(p_size == slot->deferred_info.byte_count);
	slot->deferred_fill(slot->deferred_user_data, &slot->deferred_info, p_data);
	const bool delete_slot = slot->delete_after_upload.load(std::memory_order_acquire);
	slot->deferred_pending.store(false, std::memory_order_release);
	if (delete_slot) {
		memdelete(slot);
	}
}

bool Terrain3DForwardPlusBridge::unregister_native_callback(int64_t p_prepare_address, int64_t p_shadow_prepare_address,
		int64_t p_submit_address, int64_t p_user_data_address) {
	MutexLock lock(state_mutex);
	NativeCallbackSlot *slot = active_slot.load(std::memory_order_relaxed);
	if (slot == nullptr) {
		_set_error_locked("No native bridge is registered.");
		return false;
	}

	const t3dPrepareFn expected_prepare = reinterpret_cast<t3dPrepareFn>(uintptr_t(uint64_t(p_prepare_address)));
	const t3dPrepareShadowFn expected_shadow_prepare = reinterpret_cast<t3dPrepareShadowFn>(uintptr_t(uint64_t(p_shadow_prepare_address)));
	const t3dSubmitFn expected_submit = reinterpret_cast<t3dSubmitFn>(uintptr_t(uint64_t(p_submit_address)));
	void *const expected_user_data = reinterpret_cast<void *>(uintptr_t(uint64_t(p_user_data_address)));
	if (slot->prepare != expected_prepare || slot->prepare_shadow != expected_shadow_prepare ||
			slot->submit != expected_submit || slot->user_data != expected_user_data) {
		_set_error_locked("The unregister callback tuple does not own the active ABI 12 registration.");
		return false;
	}

	slot = active_slot.exchange(nullptr, std::memory_order_acq_rel);
	retired_slots.push_back(slot);
	_set_error_locked(String());
	return true;
}

void Terrain3DForwardPlusBridge::collect_retired_callbacks() {
	if (RenderingDevice::get_singleton() != nullptr) {
		complete_native_hot_frame();
	}
	MutexLock lock(state_mutex);
	for (NativeCallbackSlot *slot : retired_slots) {
		ERR_FAIL_COND_MSG(slot->deferred_pending.load(std::memory_order_acquire),
				"Complete the device frame before collecting Terrain3D deferred callbacks.");
	}
	for (NativeCallbackSlot *slot : retired_slots) {
		memdelete(slot);
	}
	retired_slots.clear();
}

Terrain3DForwardPlusBridge::Terrain3DForwardPlusBridge() {
	ERR_FAIL_COND(singleton != nullptr);
	singleton = this;
}

Terrain3DForwardPlusBridge::~Terrain3DForwardPlusBridge() {
	if (RenderingDevice::get_singleton() != nullptr && RenderingServer::get_singleton() != nullptr) {
		complete_native_hot_frame();
	}
	NativeCallbackSlot *slot = active_slot.exchange(nullptr, std::memory_order_acq_rel);
	if (slot != nullptr) {
		if (slot->deferred_pending.load(std::memory_order_acquire)) {
			slot->delete_after_upload.store(true, std::memory_order_release);
		} else {
			memdelete(slot);
		}
	}
	for (NativeCallbackSlot *retired : retired_slots) {
		if (retired->deferred_pending.load(std::memory_order_acquire)) {
			retired->delete_after_upload.store(true, std::memory_order_release);
		} else {
			memdelete(retired);
		}
	}
	retired_slots.clear();
	singleton = nullptr;
}

#endif // T3D_NATIVE_BRIDGE_ENABLED
