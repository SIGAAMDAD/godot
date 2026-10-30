/**************************************************************************/
/*  xess_d3d12.cpp                                                        */
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

#include "xess.h"

#ifdef XESS_ENABLED_D3D12

#include "core/error/error_macros.h"
#include "core/os/memory.h"
#include "core/string/print_string.h"
#include "core/variant/variant.h"
#include "drivers/xess/xess_context.h"
#include "servers/rendering/rendering_device.h"

using namespace RendererRD;

namespace {

// Heap-allocated payload for the render graph callback, freed inside it. The
// native resources are resolved up front on the rendering thread and stay valid
// for the frame.
struct CallbackArgs {
	xess_context_handle_t context = nullptr;
	XeSSScalerContext *scaler_context = nullptr;
	ID3D12Resource *color = nullptr;
	ID3D12Resource *velocity = nullptr;
	ID3D12Resource *depth = nullptr;
	ID3D12Resource *exposure = nullptr;
	ID3D12Resource *output = nullptr;
	float jitter_x = 0.0f;
	float jitter_y = 0.0f;
	uint32_t reset = 0;
	uint32_t input_width = 0;
	uint32_t input_height = 0;
};

ID3D12Resource *resource_from_rid(RID p_rid) {
	return (ID3D12Resource *)(uintptr_t)RD::get_singleton()->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, p_rid);
}

void upscale_callback(RenderingDeviceDriver *p_driver, RenderingDeviceDriver::CommandBufferID p_command_buffer, void *p_userdata) {
	CallbackArgs *args = (CallbackArgs *)p_userdata;
	XeSSContext &runtime = XeSSContext::get();

	if (runtime.xessD3D12Execute != nullptr && args->context != nullptr) {
		ID3D12GraphicsCommandList *command_list = (ID3D12GraphicsCommandList *)p_driver->command_buffer_get_native_handle(p_command_buffer);

		xess_d3d12_execute_params_t exec_params = {};
		exec_params.pColorTexture = args->color;
		exec_params.pVelocityTexture = args->velocity;
		exec_params.pDepthTexture = args->depth;
		exec_params.pExposureScaleTexture = args->exposure;
		exec_params.pOutputTexture = args->output;
		exec_params.jitterOffsetX = args->jitter_x;
		exec_params.jitterOffsetY = args->jitter_y;
		exec_params.exposureScale = 1.0f;
		exec_params.resetHistory = args->reset;
		exec_params.inputWidth = args->input_width;
		exec_params.inputHeight = args->input_height;

		xess_result_t result = runtime.xessD3D12Execute(args->context, command_list, &exec_params);
		if (result != XESS_RESULT_SUCCESS) {
			args->scaler_context->execution_failed.store(true);
			ERR_PRINT_ONCE(vformat("Intel XeSS: xessD3D12Execute failed (%s).", XeSSContext::result_to_string(result)));
		}

		// XeSS binds its own descriptor heaps, root signature and pipeline state on
		// the command list, invalidating what the D3D12 driver believes is bound.
		p_driver->command_buffer_invalidate_bound_state(p_command_buffer);
	}

	memdelete(args);
}

} // namespace

XeSSScalerContext *XeSSEffect::_create_context_d3d12(Size2i p_internal_size, Size2i p_target_size) {
	XeSSContext &runtime = XeSSContext::get();
	ERR_FAIL_NULL_V(runtime.xessD3D12CreateContext, nullptr);
	ERR_FAIL_NULL_V(runtime.xessD3D12Init, nullptr);

	ID3D12Device *device = (ID3D12Device *)(uintptr_t)RD::get_singleton()->get_driver_resource(RD::DRIVER_RESOURCE_LOGICAL_DEVICE);
	ERR_FAIL_NULL_V(device, nullptr);

	xess_context_handle_t handle = nullptr;
	xess_result_t result = runtime.xessD3D12CreateContext(device, &handle);
	if (result != XESS_RESULT_SUCCESS) {
		ERR_PRINT(vformat("Intel XeSS: xessD3D12CreateContext failed (%s). Falling back to another upscaler.", XeSSContext::result_to_string(result)));
		return nullptr;
	}

	_attach_logging(handle);

	xess_d3d12_init_params_t init_params = {};
	init_params.outputResolution.x = p_target_size.width;
	init_params.outputResolution.y = p_target_size.height;
	init_params.qualitySetting = _quality_from_resolutions(p_internal_size, p_target_size);
	// Godot uses reverse-Z. Its motion vectors do not carry camera jitter (the FSR2
	// path treats them the same way), so XESS_INIT_FLAG_JITTERED_MV is left unset.
	init_params.initFlags = XESS_INIT_FLAG_INVERTED_DEPTH;

	result = runtime.xessD3D12Init(handle, &init_params);
	if (result != XESS_RESULT_SUCCESS) {
		ERR_PRINT(vformat("Intel XeSS: xessD3D12Init failed (%s). Falling back to another upscaler.", XeSSContext::result_to_string(result)));
		runtime.xessDestroyContext(handle);
		return nullptr;
	}

	// Godot stores motion vectors in UV space, XeSS expects input-resolution pixels.
	// Without this scale, history fails to reproject and ghosting is severe.
	if (runtime.xessSetVelocityScale != nullptr) {
		runtime.xessSetVelocityScale(handle, float(p_internal_size.width), float(p_internal_size.height));
	}

	XeSSScalerContext *context = memnew(XeSSScalerContext);
	context->handle = handle;
	context->internal_size = p_internal_size;
	context->target_size = p_target_size;
	context->initialized = true;
	return context;
}

bool XeSSEffect::_upscale_d3d12(const Parameters &p_params) {
	CallbackArgs *args = memnew(CallbackArgs);
	args->context = p_params.context->handle;
	args->scaler_context = p_params.context;
	args->color = resource_from_rid(p_params.color);
	args->velocity = resource_from_rid(p_params.velocity);
	args->output = resource_from_rid(p_params.output);
	if (p_params.depth.is_valid()) {
		args->depth = resource_from_rid(p_params.depth);
	}
	if (p_params.exposure.is_valid()) {
		args->exposure = resource_from_rid(p_params.exposure);
	}
	args->jitter_x = p_params.jitter.x;
	args->jitter_y = p_params.jitter.y;
	args->reset = p_params.reset_accumulation ? 1 : 0;
	args->input_width = p_params.internal_size.width;
	args->input_height = p_params.internal_size.height;

	// Inputs are sampled, the output is written as a storage image. The render graph
	// turns these into the NON_PIXEL_SHADER_RESOURCE and UNORDERED_ACCESS states XeSS
	// requires.
	RD::CallbackResource res[5] = {};
	int num_resources = 0;
	res[num_resources].rid = p_params.color;
	res[num_resources++].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
	res[num_resources].rid = p_params.velocity;
	res[num_resources++].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
	if (p_params.depth.is_valid()) {
		res[num_resources].rid = p_params.depth;
		res[num_resources++].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
	}
	if (p_params.exposure.is_valid()) {
		res[num_resources].rid = p_params.exposure;
		res[num_resources++].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
	}
	res[num_resources].rid = p_params.output;
	res[num_resources++].usage = RD::CALLBACK_RESOURCE_USAGE_STORAGE_IMAGE_READ_WRITE;

	Error err = RD::get_singleton()->driver_callback_add((RDD::DriverCallback)upscale_callback, args, VectorView<RD::CallbackResource>(res, num_resources));
	if (err != OK) {
		memdelete(args);
		return false;
	}
	return true;
}

#endif // XESS_ENABLED_D3D12
