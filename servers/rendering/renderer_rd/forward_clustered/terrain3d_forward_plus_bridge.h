/**************************************************************************/
/*  terrain3d_forward_plus_bridge.h                                      */
/**************************************************************************/

#pragma once

#include "core/object/class_db.h"
#include "core/object/object.h"
#include "core/os/mutex.h"
#include "core/templates/list.h"
#include "servers/rendering/renderer_rd/forward_clustered/terrain3d_forward_plus_bridge_abi.h"

#include <atomic>

class Terrain3DForwardPlusBridge : public Object {
	GDCLASS(Terrain3DForwardPlusBridge, Object);

	struct NativeCallbackSlot {
		t3dPrepareFn prepare = nullptr;
		t3dPrepareShadowFn prepare_shadow = nullptr;
		t3dSubmitFn submit = nullptr;
		void *user_data = nullptr;
		uint64_t prepare_render_frame = UINT64_MAX;
		uint32_t prepare_count = 0;
		bool inside_prepare = false;
		uint64_t deferred_render_frame = UINT64_MAX;
		std::atomic<bool> deferred_pending{ false };
		std::atomic<bool> delete_after_upload{ false };
		t3dDeferredUploadFn deferred_fill = nullptr;
		t3dHotSealFn deferred_seal = nullptr;
		void *deferred_user_data = nullptr;
		t3dBridgeDeferredUpload deferred_info = {};
	};

	static Terrain3DForwardPlusBridge *singleton;

	std::atomic<NativeCallbackSlot *> active_slot{ nullptr };
#if defined(TOOLS_ENABLED) || defined(DEBUG_ENABLED)
	mutable std::atomic<uint32_t> environment_flags{ 0 };
	mutable std::atomic<bool> environment_radiance_valid{ false };
	mutable std::atomic<uint64_t> environment_frame_generation{ 0 };
	mutable std::atomic<uint32_t> environment_resource_generation{ 0 };
#endif
	Mutex state_mutex;
	List<NativeCallbackSlot *> retired_slots;
	String last_error;

	void _set_error_locked(const String &p_error);
	bool _begin_prepare(NativeCallbackSlot *p_slot, uint64_t p_render_frame) const;
	static void _fill_deferred_upload(void *p_userdata, uint8_t *p_data, uint32_t p_size);

	enum RDRequestKind { RD_COMPLETE, RD_BACKING_SIZE, RD_CONFIGURE_STAGING, RD_DEFAULT_STAGING, RD_PERSISTENT_ABI };
	struct RDRequest {
		RDRequestKind kind;
		int64_t value = 0;
		int64_t result = 0;
		bool callback_registered = false;
	};
	void _dispatch_rd_request(int64_t p_request_address);
	int64_t _request_rd(RDRequestKind p_kind, int64_t p_value = 0);
	static bool _seal_persistent_hot(void *p_userdata);

protected:
	static void _bind_methods();

public:
	typedef uintptr_t RenderThreadHandle;

	static Terrain3DForwardPlusBridge *get_singleton() { return singleton; }

	int64_t get_abi_version() const { return T3D_FORWARD_BRIDGE_ABI; }
	int64_t get_native_capabilities() const { return int64_t(T3D_FORWARD_BRIDGE_CAPABILITIES); }
	int64_t get_deferred_upload_abi_version();
	int64_t get_native_device_frame() const;
	bool complete_native_hot_frame();
	int64_t get_persistent_hot_abi_version();
	int64_t get_native_hot_backing_size(int64_t p_logical_bytes);
	bool configure_native_hot_staging(int64_t p_logical_bytes);
	bool defer_native_buffer_flush(RID p_buffer, int64_t p_seal_address, int64_t p_user_data_address);
	bool has_native_callback() const;
	String get_last_error();
#if defined(TOOLS_ENABLED) || defined(DEBUG_ENABLED)
	int64_t get_environment_flags() const { return int64_t(environment_flags.load(std::memory_order_relaxed)); }
	bool get_environment_radiance_valid() const { return environment_radiance_valid.load(std::memory_order_relaxed); }
	int64_t get_environment_frame_generation() const { return int64_t(environment_frame_generation.load(std::memory_order_relaxed)); }
	int64_t get_environment_resource_generation() const { return int64_t(environment_resource_generation.load(std::memory_order_relaxed)); }
#endif

	bool register_native_callback(int64_t p_prepare_address, int64_t p_shadow_prepare_address,
			int64_t p_submit_address, int64_t p_user_data_address, int64_t p_frame_size,
			int64_t p_resource_size, int64_t p_pass_size, int64_t p_shadow_size);
	bool unregister_native_callback(int64_t p_prepare_address, int64_t p_shadow_prepare_address,
			int64_t p_submit_address, int64_t p_user_data_address);
	void collect_retired_callbacks();

	// Reserve the HOT transfer during this frame's first native PREPARE, before
	// recording any Terrain3D GPU consumer. Later PREPARE calls only populate
	// extension-owned CPU data. The fill callback runs at device frame end and
	// must only write the supplied memory; it must not call RenderingDevice.
	bool defer_native_buffer_update(RID p_buffer, int64_t p_offset, int64_t p_size,
			int64_t p_fill_address, int64_t p_user_data_address);

	// RenderForwardClustered runs these callbacks from Godot's dedicated render
	// thread. Snapshot the callback slot once at the scene boundary and reuse the
	// same immutable slot for PREPARE/SUBMIT. Unregistration retires the slot and
	// the extension synchronizes the render thread before collection, so a cached
	// handle remains valid for the in-flight render scene.
	_FORCE_INLINE_ RenderThreadHandle acquire_render_thread_handle() const {
		return RenderThreadHandle(active_slot.load(std::memory_order_acquire));
	}

	_FORCE_INLINE_ uint64_t prepare(RenderThreadHandle p_handle, const t3dBridgeFrame *p_frame,
			const t3dBridgeSceneResources *p_resources, const t3dBridgePass *p_pass) const {
#if defined(TOOLS_ENABLED) || defined(DEBUG_ENABLED)
		if (p_frame != nullptr) {
			environment_flags.store(p_frame->environment_flags, std::memory_order_relaxed);
			environment_frame_generation.store(p_frame->generation, std::memory_order_relaxed);
		}
		if (p_resources != nullptr) {
			environment_radiance_valid.store(p_resources->radiance_texture != 0, std::memory_order_relaxed);
			environment_resource_generation.store(p_resources->generation, std::memory_order_relaxed);
		}
#endif
		NativeCallbackSlot *const slot = reinterpret_cast<NativeCallbackSlot *>(p_handle);
		if (slot == nullptr || !_begin_prepare(slot, p_frame != nullptr ? p_frame->render_frame : slot->prepare_render_frame)) {
			return 0;
		}
		const uint64_t token = slot->prepare(slot->user_data, p_frame, p_resources, p_pass);
		slot->inside_prepare = false;
		return token;
	}

	_FORCE_INLINE_ uint64_t prepare_shadow(RenderThreadHandle p_handle, const t3dBridgeShadow *p_shadow) const {
		NativeCallbackSlot *const slot = reinterpret_cast<NativeCallbackSlot *>(p_handle);
		if (slot == nullptr || !_begin_prepare(slot, p_shadow != nullptr ? p_shadow->render_frame : slot->prepare_render_frame)) {
			return 0;
		}
		const uint64_t token = slot->prepare_shadow(slot->user_data, p_shadow);
		slot->inside_prepare = false;
		return token;
	}

	_FORCE_INLINE_ uint32_t submit(RenderThreadHandle p_handle, uint64_t p_serial, int64_t p_draw_list) const {
		if (p_serial == 0 || p_draw_list < 0) {
			return 0;
		}
		NativeCallbackSlot *const slot = reinterpret_cast<NativeCallbackSlot *>(p_handle);
		return slot != nullptr && slot->submit != nullptr ? slot->submit(slot->user_data, p_serial, p_draw_list) : 0;
	}

	Terrain3DForwardPlusBridge();
	~Terrain3DForwardPlusBridge();
};
