/**************************************************************************/
/*  terrain3d_forward_plus_bridge_abi.h                                   */
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

#pragma once

#include <stddef.h>
#include <stdint.h>

// The native callback ABI transports function pointers through 64-bit integers.
#if UINTPTR_MAX == UINT64_MAX
#define T3D_NATIVE_BRIDGE_ENABLED 1
#else
#define T3D_NATIVE_BRIDGE_ENABLED 0
#endif
#include <type_traits>

#if defined(_WIN32)
#define T3D_BRIDGE_CALL __cdecl
#else
#define T3D_BRIDGE_CALL
#endif

#define T3D_FORWARD_BRIDGE_ABI UINT32_C(12)
#define T3D_FORWARD_MAX_VIEWS UINT32_C(2)

typedef enum t3dBridgeCapability : uint64_t {
	T3D_BRIDGE_CAP_DIRECT_POD = UINT64_C(1) << 0,
	T3D_BRIDGE_CAP_DEPTH = UINT64_C(1) << 1,
	T3D_BRIDGE_CAP_OPAQUE = UINT64_C(1) << 2,
	T3D_BRIDGE_CAP_SHADOW = UINT64_C(1) << 3,
	T3D_BRIDGE_CAP_MOTION = UINT64_C(1) << 4,
	T3D_BRIDGE_CAP_TRANSPARENT = UINT64_C(1) << 5,
	T3D_BRIDGE_CAP_CLUSTERED_LIGHTING = UINT64_C(1) << 6,
	T3D_BRIDGE_CAP_DECALS = UINT64_C(1) << 7,
	T3D_BRIDGE_CAP_REFLECTIONS = UINT64_C(1) << 8,
	T3D_BRIDGE_CAP_RESOURCE_GENERATIONS = UINT64_C(1) << 9,
	T3D_BRIDGE_CAP_SPLIT_SHADOW_PACKET = UINT64_C(1) << 10,
	T3D_BRIDGE_CAP_ENVIRONMENT_LIGHTING = UINT64_C(1) << 11,
	T3D_BRIDGE_CAP_PREVIOUS_DEPTH_HIZ = UINT64_C(1) << 12,
	T3D_BRIDGE_CAP_DEFERRED_UPLOAD = UINT64_C(1) << 13,
} t3dBridgeCapability;

static constexpr uint64_t T3D_FORWARD_BRIDGE_CAPABILITIES =
		T3D_BRIDGE_CAP_DIRECT_POD | T3D_BRIDGE_CAP_DEPTH | T3D_BRIDGE_CAP_OPAQUE |
		T3D_BRIDGE_CAP_SHADOW | T3D_BRIDGE_CAP_MOTION | T3D_BRIDGE_CAP_TRANSPARENT |
		T3D_BRIDGE_CAP_CLUSTERED_LIGHTING | T3D_BRIDGE_CAP_DECALS | T3D_BRIDGE_CAP_REFLECTIONS |
		T3D_BRIDGE_CAP_RESOURCE_GENERATIONS | T3D_BRIDGE_CAP_SPLIT_SHADOW_PACKET |
		T3D_BRIDGE_CAP_ENVIRONMENT_LIGHTING | T3D_BRIDGE_CAP_PREVIOUS_DEPTH_HIZ | T3D_BRIDGE_CAP_DEFERRED_UPLOAD;

typedef enum t3dBridgePassKind : uint32_t {
	T3D_BRIDGE_PASS_INVALID = 0,
	T3D_BRIDGE_PASS_DEPTH = 1,
	T3D_BRIDGE_PASS_OPAQUE = 2,
	T3D_BRIDGE_PASS_SHADOW = 3,
	T3D_BRIDGE_PASS_MOTION = 4,
	T3D_BRIDGE_PASS_MATERIAL = 5,
	T3D_BRIDGE_PASS_SDF = 6,
	T3D_BRIDGE_PASS_GI = 7,
	T3D_BRIDGE_PASS_REFLECTION = 8,
	T3D_BRIDGE_PASS_TRANSPARENT = 9,
} t3dBridgePassKind;

typedef enum t3dBridgeEnvironmentFlags : uint32_t {
	T3D_BRIDGE_ENV_USE_AMBIENT_LIGHT = 1u << 0,
	T3D_BRIDGE_ENV_USE_AMBIENT_CUBEMAP = 1u << 1,
	T3D_BRIDGE_ENV_USE_REFLECTION_CUBEMAP = 1u << 2,
	T3D_BRIDGE_ENV_RADIANCE_OCTMAP_ARRAY = 1u << 3,
} t3dBridgeEnvironmentFlags;

typedef enum t3dBridgeFrameFlags : uint32_t {
	T3D_BRIDGE_FRAME_MOTION_VECTORS = 1u << 0,
	T3D_BRIDGE_FRAME_DIRECTIONAL_SOFT_SHADOWS = 1u << 1,
} t3dBridgeFrameFlags;

typedef enum t3dBridgePassFlags : uint32_t {
	T3D_BRIDGE_PASS_DEPTH_PREPASS_ACTIVE = 1u << 0,
	T3D_BRIDGE_PASS_REVERSE_CULL = 1u << 1,
} t3dBridgePassFlags;

typedef struct alignas(8) t3dBridgeFrame {
	uint32_t abi_version;
	uint32_t struct_size;
	uint64_t generation;
	uint64_t scene_pass;
	uint64_t render_frame;
	uint64_t viewport_key;
	uint32_t view_count;
	uint32_t sample_count;
	uint32_t camera_visible_layers;
	uint32_t flags;
	int32_t render_region[4];
	int32_t render_size[2];
	uint32_t cluster_max_elements;
	uint32_t cluster_shift;
	uint32_t cluster_width;
	uint32_t cluster_type_size;
	uint32_t cluster_max_elements_div_32;
	uint32_t directional_light_count;
	float camera_z_near;
	float camera_z_far;
	float reflection_atlas_border_size;
	float camera_transform[12];
	float view_projection[T3D_FORWARD_MAX_VIEWS][16];
	float previous_view_projection[T3D_FORWARD_MAX_VIEWS][16];
	float eye_offset[T3D_FORWARD_MAX_VIEWS][4];
	float motion_jitter[4];
	uint32_t abi10_reserved_tail;
	uint32_t environment_flags;
	uint32_t environment_reserved0;
	float ambient_light_color_energy[4];
	float ambient_color_sky_mix;
	float emissive_exposure_normalization;
	float ibl_exposure_normalization;
	float radiance_pixel_size;
	float radiance_border_size;
	float environment_reserved1;
	float radiance_inverse_xform[12];
} t3dBridgeFrame;

typedef struct alignas(8) t3dBridgeSceneResources {
	uint32_t abi_version;
	uint32_t struct_size;
	uint32_t generation;
	uint32_t reserved0;
	uint64_t directional_light_buffer;
	uint64_t omni_light_buffer;
	uint64_t spot_light_buffer;
	uint64_t area_light_buffer;
	uint64_t cluster_buffer;
	uint64_t shadow_atlas;
	uint64_t directional_shadow_atlas;
	uint64_t shadow_sampler;
	uint64_t reflection_probe_buffer;
	uint64_t reflection_atlas;
	uint64_t decal_buffer;
	uint64_t decal_atlas;
	uint64_t decal_atlas_srgb;
	uint64_t radiance_texture;
	uint64_t previous_depth_texture;
} t3dBridgeSceneResources;

typedef struct alignas(8) t3dBridgePass {
	uint32_t abi_version;
	uint32_t struct_size;
	uint32_t pass_kind;
	uint32_t native_pass_mode;
	uint32_t color_pass_flags;
	uint32_t flags;
	int64_t framebuffer_format;
	uint64_t serial;
	uint64_t native_framebuffer;
} t3dBridgePass;

typedef struct alignas(8) t3dBridgeShadow {
	uint32_t abi_version;
	uint32_t struct_size;
	uint32_t native_pass_mode;
	uint32_t flags;
	uint32_t sample_count;
	uint32_t shadow_caster_mask;
	uint32_t shadow_light_cull_mask;
	uint32_t reserved0;
	int64_t framebuffer_format;
	uint64_t serial;
	uint64_t scene_pass;
	uint64_t render_frame;
	uint64_t viewport_key;
	float camera_transform[12];
	float view_projection[16];
	float eye_offset[4];
} t3dBridgeShadow;

typedef uint64_t(T3D_BRIDGE_CALL *t3dPrepareFn)(void *, const t3dBridgeFrame *,
		const t3dBridgeSceneResources *, const t3dBridgePass *) noexcept;
typedef uint64_t(T3D_BRIDGE_CALL *t3dPrepareShadowFn)(void *, const t3dBridgeShadow *) noexcept;
typedef uint32_t(T3D_BRIDGE_CALL *t3dSubmitFn)(void *, uint64_t, int64_t) noexcept;

// Optional bridge-only deferred upload protocol. ABI 12 packets are unchanged.
#define T3D_FORWARD_DEFERRED_UPLOAD_ABI UINT32_C(1)

typedef struct alignas(8) t3dBridgeDeferredUpload {
	uint32_t abi_version;
	uint32_t struct_size;
	uint64_t render_frame;
	uint64_t device_frame;
	uint32_t frame_slot;
	uint32_t frame_slot_count;
	uint32_t byte_count;
	uint32_t reserved0;
} t3dBridgeDeferredUpload;

// Called after normal pass recording, before the reserved transfer executes.
// Fill exactly byte_count bytes. Memory is borrowed only for this call.
// No RD calls, allocation-dependent GPU operations, or exceptions are allowed.
// Reserve once in the first PREPARE; use frame_slot to select fence-safe HOT
// storage. Keep callback/user data alive through device frame completion.
// Pass slices/indices are allocated by the extension as PREPARE calls arrive.
typedef void(T3D_BRIDGE_CALL *t3dDeferredUploadFn)(void *, const t3dBridgeDeferredUpload *, uint8_t *) noexcept;
static_assert(std::is_standard_layout_v<t3dBridgeDeferredUpload> && std::is_trivially_copyable_v<t3dBridgeDeferredUpload>);
static_assert(sizeof(t3dBridgeDeferredUpload) == 40);
#define T3D_FORWARD_PERSISTENT_HOT_ABI UINT32_C(1)
typedef t3dBridgeDeferredUpload t3dBridgeHotFrameInfo;
typedef bool(T3D_BRIDGE_CALL *t3dHotSealFn)(void *, const t3dBridgeHotFrameInfo *) noexcept;
static_assert(offsetof(t3dBridgeHotFrameInfo, render_frame) == 8);

#if T3D_NATIVE_BRIDGE_ENABLED
static_assert(sizeof(void *) == 8, "The Terrain3D Forward+ bridge requires 64-bit pointers.");
static_assert(std::is_standard_layout_v<t3dBridgeFrame> && std::is_trivially_copyable_v<t3dBridgeFrame>);
static_assert(std::is_standard_layout_v<t3dBridgeSceneResources> && std::is_trivially_copyable_v<t3dBridgeSceneResources>);
static_assert(std::is_standard_layout_v<t3dBridgePass> && std::is_trivially_copyable_v<t3dBridgePass>);
static_assert(std::is_standard_layout_v<t3dBridgeShadow> && std::is_trivially_copyable_v<t3dBridgeShadow>);
static_assert(alignof(t3dBridgeFrame) == 8 && sizeof(t3dBridgeFrame) == 568, "Unexpected ABI 12 frame layout.");
static_assert(offsetof(t3dBridgeFrame, abi10_reserved_tail) == 468, "ABI 10 tail padding position changed.");
static_assert(offsetof(t3dBridgeFrame, environment_flags) == 472, "ABI 11 Environment payload must remain append-only.");
static_assert(alignof(t3dBridgeSceneResources) == 8 && sizeof(t3dBridgeSceneResources) == 136, "Unexpected ABI 12 resource layout.");
static_assert(offsetof(t3dBridgeSceneResources, radiance_texture) == 120, "ABI 12 inherited radiance RID moved.");
static_assert(offsetof(t3dBridgeSceneResources, previous_depth_texture) == 128, "ABI 12 previous-depth RID must remain append-only.");
static_assert(alignof(t3dBridgePass) == 8 && sizeof(t3dBridgePass) == 48, "Unexpected ABI 12 pass layout.");
static_assert(alignof(t3dBridgeShadow) == 8 && sizeof(t3dBridgeShadow) == 200, "Unexpected ABI 12 shadow layout.");
#endif // T3D_NATIVE_BRIDGE_ENABLED

#undef T3D_BRIDGE_CALL
