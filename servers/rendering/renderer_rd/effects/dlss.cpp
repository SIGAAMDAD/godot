/**************************************************************************/
/*  dlss.cpp                                                              */
/**************************************************************************/
#include "dlss.h"
#ifdef STREAMLINE_ENABLED
#include "core/config/engine.h"
#include "core/error/error_macros.h"
#include "core/os/memory.h"
#include "core/string/print_string.h"
#include "servers/rendering/rendering_device.h"
#include <atomic>
#include <cstring>

using namespace RendererRD;

namespace {
std::atomic<uint32_t> next_viewport{ 1 };

struct CallbackArgs {
	DLSSContext *context = nullptr;
	DLSSEffect::Parameters params;
	sl::Resource resources[5] = {};
	sl::ResourceTag tags[5] = {};
	uint32_t resource_count = 0;
	bool d3d12 = false;
};

sl::float4x4 to_sl(const Projection &p_matrix) {
	sl::float4x4 result{};
	for (int r = 0; r < 4; ++r) {
		result.setRow(r, sl::float4(p_matrix.columns[0][r], p_matrix.columns[1][r], p_matrix.columns[2][r], p_matrix.columns[3][r]));
	}
	return result;
}

sl::float4x4 sl_identity() {
	sl::float4x4 result{};
	result.setRow(0, sl::float4(1.0f, 0.0f, 0.0f, 0.0f));
	result.setRow(1, sl::float4(0.0f, 1.0f, 0.0f, 0.0f));
	result.setRow(2, sl::float4(0.0f, 0.0f, 1.0f, 0.0f));
	result.setRow(3, sl::float4(0.0f, 0.0f, 0.0f, 1.0f));
	return result;
}

void add_resource(CallbackArgs *p_args, RID p_rid, sl::BufferType p_tag, bool p_output) {
	if (!p_rid.is_valid()) {
		return;
	}
	RD *rd = RD::get_singleton();
	RD::TextureFormat fmt = rd->texture_get_format(p_rid);
	sl::Resource &resource = p_args->resources[p_args->resource_count];
	const uint64_t native = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, p_rid);
	const uint64_t view = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_VIEW, p_rid);
	const uint32_t native_format = (uint32_t)rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_rid);
	if (p_args->d3d12) {
		// D3D12: NON_PIXEL_SHADER_RESOURCE (input), UNORDERED_ACCESS (output).
		resource = sl::Resource(sl::ResourceType::eTex2d, (void *)(uintptr_t)native, p_output ? 0x8u : 0x40u);
	} else {
		const uint64_t device_memory = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_DEVICE_MEMORY, p_rid);
		// Vulkan: GENERAL (storage output), SHADER_READ_ONLY_OPTIMAL (input).
		resource = sl::Resource(sl::ResourceType::eTex2d, (void *)(uintptr_t)native, (void *)(uintptr_t)device_memory, (void *)(uintptr_t)view, p_output ? 1u : 5u);
		resource.usage = (uint32_t)rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_USAGE_FLAGS, p_rid);
	}
	resource.width = fmt.width;
	resource.height = fmt.height;
	resource.nativeFormat = native_format;
	resource.mipLevels = fmt.mipmaps;
	resource.arrayLayers = fmt.array_layers;
	p_args->tags[p_args->resource_count] = sl::ResourceTag(&resource, p_tag, sl::ResourceLifecycle::eValidUntilEvaluate);
	++p_args->resource_count;
}

void dlss_callback(RenderingDeviceDriver *p_driver, RenderingDeviceDriver::CommandBufferID p_cmd, void *p_userdata) {
	CallbackArgs *args = (CallbackArgs *)p_userdata;
	DLSSContext *ctx = args->context;
	StreamlineRuntime &slr = StreamlineRuntime::get();
	bool ok = false;
	if (slr.is_ready() && slr.set_options && slr.new_frame_token && slr.evaluate) {
		const uint32_t frame_index = (uint32_t)Engine::get_singleton()->get_frames_drawn();
		sl::FrameToken *token = nullptr;
		if (slr.new_frame_token(token, &frame_index) == sl::Result::eOk && token != nullptr) {
			sl::Constants constants{};
			constants.cameraViewToClip = to_sl(args->params.camera_projection);
			constants.clipToCameraView = to_sl(args->params.camera_projection.inverse());
			constants.clipToLensClip = sl_identity();
			const Transform3D &cam = args->params.camera_transform;
			constants.cameraPos = sl::float3(cam.origin.x, cam.origin.y, cam.origin.z);
			const Vector3 forward = -cam.basis.rows[2];
			constants.cameraFwd = sl::float3(forward.x, forward.y, forward.z);
			constants.cameraUp = sl::float3(cam.basis.rows[1].x, cam.basis.rows[1].y, cam.basis.rows[1].z);
			constants.cameraRight = sl::float3(cam.basis.rows[0].x, cam.basis.rows[0].y, cam.basis.rows[0].z);
			constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
			constants.orthographicProjection = sl::Boolean::eFalse;
			constants.depthInverted = sl::Boolean::eTrue;
			constants.cameraMotionIncluded = sl::Boolean::eTrue;
			constants.motionVectors3D = sl::Boolean::eFalse;
			constants.motionVectorsJittered = sl::Boolean::eFalse;
			constants.cameraNear = args->params.near_plane;
			constants.cameraFar = args->params.far_plane;
			constants.cameraFOV = args->params.fovy;
			constants.cameraAspectRatio = (float)ctx->target_size.width / (float)ctx->target_size.height;
			constants.jitterOffset = sl::float2(args->params.jitter.x, args->params.jitter.y);
			constants.mvecScale = sl::float2((float)args->params.internal_size.width, (float)args->params.internal_size.height);
			constants.reset = args->params.reset_history ? sl::Boolean::eTrue : sl::Boolean::eFalse;
			constants.clipToPrevClip = to_sl(args->params.reprojection);
			constants.prevClipToClip = to_sl(args->params.reprojection.inverse());
			void *native_cmd = p_driver->command_buffer_get_native_handle(p_cmd);
			if (native_cmd && slr.set_options(ctx->viewport, ctx->options) == sl::Result::eOk &&
					slr.set_constants(constants, *token, ctx->viewport) == sl::Result::eOk &&
					slr.set_tag_for_frame(*token, ctx->viewport, args->tags, args->resource_count, (sl::CommandBuffer *)native_cmd) == sl::Result::eOk) {
				const sl::BaseStructure *inputs[] = { &ctx->viewport };
				ok = slr.evaluate(sl::kFeatureDLSS, *token, inputs, 1, (sl::CommandBuffer *)native_cmd) == sl::Result::eOk;
			}
			// Vendor execution may alter binding state independently of its return code.
			p_driver->command_buffer_invalidate_bound_state(p_cmd);
		}
	}
	if (!ok) {
		ctx->execution_failed.store(true);
		ERR_PRINT_ONCE("Streamline DLSS evaluation failed. The next frame will use the upstream FSR2 path.");
	}
	memdelete(args);
}
}

DLSSContext::~DLSSContext() {
	StreamlineRuntime &runtime = StreamlineRuntime::get();
	if (runtime.is_initialized() && runtime.free_resources != nullptr) {
		RD::get_singleton()->_flush_and_stall_for_all_frames();
		runtime.free_resources(sl::kFeatureDLSS, viewport);
	}
}

DLSSContext *DLSSEffect::create_context(Size2i p_internal, Size2i p_target) {
	StreamlineRuntime &runtime = StreamlineRuntime::get();
	if (!runtime.is_ready() || p_internal.width <= 0 || p_internal.height <= 0 || p_target.width <= 0 || p_target.height <= 0) {
		return nullptr;
	}
	DLSSContext *ctx = memnew(DLSSContext(next_viewport.fetch_add(1)));
	ctx->internal_size = p_internal;
	ctx->target_size = p_target;
	const float ratio = (float)p_target.width / (float)p_internal.width;
	ctx->options.mode = ratio < 1.15f ? sl::DLSSMode::eDLAA : ratio < 1.55f ? sl::DLSSMode::eMaxQuality : ratio < 1.85f ? sl::DLSSMode::eBalanced : ratio < 2.8f ? sl::DLSSMode::eMaxPerformance : sl::DLSSMode::eUltraPerformance;
	ctx->options.outputWidth = p_target.width;
	ctx->options.outputHeight = p_target.height;
	ctx->options.colorBuffersHDR = sl::Boolean::eTrue;
	sl::DLSSOptimalSettings settings{};
	if (!runtime.optimal_settings || runtime.optimal_settings(ctx->options, settings) != sl::Result::eOk ||
		p_internal.width < (int)settings.renderWidthMin || p_internal.width > (int)settings.renderWidthMax ||
		p_internal.height < (int)settings.renderHeightMin || p_internal.height > (int)settings.renderHeightMax) {
		memdelete(ctx);
		return nullptr;
	}
	return ctx;
}

bool DLSSEffect::upscale(const Parameters &p_params) {
	if (!p_params.context || p_params.context->execution_failed.load() || !is_supported() ||
			!p_params.color.is_valid() || !p_params.depth.is_valid() || !p_params.velocity.is_valid() || !p_params.output.is_valid() ||
			p_params.internal_size.width <= 0 || p_params.internal_size.height <= 0) {
		return false;
	}
	CallbackArgs *args = memnew(CallbackArgs);
	args->context = p_params.context;
	args->params = p_params;
	args->d3d12 = !StreamlineRuntime::get().is_vulkan();
	add_resource(args, p_params.color, sl::kBufferTypeScalingInputColor, false);
	add_resource(args, p_params.output, sl::kBufferTypeScalingOutputColor, true);
	add_resource(args, p_params.depth, sl::kBufferTypeDepth, false);
	add_resource(args, p_params.velocity, sl::kBufferTypeMotionVectors, false);
	if (p_params.exposure.is_valid()) {
		add_resource(args, p_params.exposure, sl::kBufferTypeExposure, false);
	}
	RD::CallbackResource resources[5] = {};
	int count = 0;
	for (RID rid : { p_params.color, p_params.output, p_params.depth, p_params.velocity, p_params.exposure }) {
		if (rid.is_valid()) {
			resources[count].rid = rid;
			resources[count].usage = (rid == p_params.output) ? RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE : RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
			count++;
		}
	}
	const Error error = RD::get_singleton()->driver_callback_add((RDD::DriverCallback)dlss_callback, args, VectorView<RD::CallbackResource>(resources, count));
	if (error != OK) {
		memdelete(args);
		return false;
	}
	return true;
}
#endif
