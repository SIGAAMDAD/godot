/**************************************************************************/
/*  xess_vulkan.cpp                                                       */
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

// Pulls in both the XeSS Vulkan headers and Godot's volk-based Vulkan headers.
// volk requires VK_NO_PROTOTYPES; XeSS only needs Vulkan types here, so defining
// it keeps both happy.
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

#include "xess.h"

#ifdef XESS_ENABLED_VULKAN

#include "core/error/error_macros.h"
#include "core/os/memory.h"
#include "core/string/print_string.h"
#include "core/variant/variant.h"
#include "drivers/vulkan/rendering_device_driver_vulkan.h"
#include "drivers/xess/xess_context.h"
#include "servers/rendering/rendering_device.h"

using namespace RendererRD;

namespace {

// Heap-allocated payload for the render graph callback, freed inside it. The
// native handles are resolved up front on the rendering thread and stay valid
// for the frame.
struct CallbackArgs {
	xess_context_handle_t context = nullptr;
	XeSSScalerContext *scaler_context = nullptr;
	xess_vk_image_view_info color = {};
	xess_vk_image_view_info velocity = {};
	xess_vk_image_view_info depth = {};
	xess_vk_image_view_info exposure = {};
	xess_vk_image_view_info output = {};
	bool has_depth = false;
	bool has_exposure = false;
	float jitter_x = 0.0f;
	float jitter_y = 0.0f;
	uint32_t reset = 0;
	uint32_t input_width = 0;
	uint32_t input_height = 0;
};

xess_vk_image_view_info image_view_info_from_rid(RID p_rid, VkImageAspectFlags p_aspect) {
	xess_vk_image_view_info info = {};
	RD *rd = RD::get_singleton();
	RD::TextureFormat format = rd->texture_get_format(p_rid);
	info.image = (VkImage)(uintptr_t)rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, p_rid);
	info.imageView = (VkImageView)(uintptr_t)rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_VIEW, p_rid);
	info.format = (VkFormat)rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_rid);
	info.width = format.width;
	info.height = format.height;
	info.subresourceRange.aspectMask = p_aspect;
	info.subresourceRange.baseMipLevel = 0;
	info.subresourceRange.levelCount = 1;
	info.subresourceRange.baseArrayLayer = 0;
	info.subresourceRange.layerCount = 1;
	return info;
}

void upscale_callback(RenderingDeviceDriver *p_driver, RenderingDeviceDriver::CommandBufferID p_command_buffer, void *p_userdata) {
	CallbackArgs *args = (CallbackArgs *)p_userdata;
	XeSSContext &runtime = XeSSContext::get();

	if (runtime.xessVKExecute != nullptr && args->context != nullptr) {
		VkCommandBuffer command_buffer = (VkCommandBuffer)p_driver->command_buffer_get_native_handle(p_command_buffer);

		xess_vk_execute_params_t exec_params = {};
		exec_params.colorTexture = args->color;
		exec_params.velocityTexture = args->velocity;
		if (args->has_depth) {
			exec_params.depthTexture = args->depth;
		}
		if (args->has_exposure) {
			exec_params.exposureScaleTexture = args->exposure;
		}
		exec_params.outputTexture = args->output;
		exec_params.jitterOffsetX = args->jitter_x;
		exec_params.jitterOffsetY = args->jitter_y;
		exec_params.exposureScale = 1.0f;
		exec_params.resetHistory = args->reset;
		exec_params.inputWidth = args->input_width;
		exec_params.inputHeight = args->input_height;

		xess_result_t result = runtime.xessVKExecute(args->context, command_buffer, &exec_params);
		if (result != XESS_RESULT_SUCCESS) {
			args->scaler_context->execution_failed.store(true);
			ERR_PRINT_ONCE(vformat("Intel XeSS: xessVKExecute failed (%s).", XeSSContext::result_to_string(result)));
		}
	}

	memdelete(args);
}

} // namespace

XeSSScalerContext *XeSSEffect::_create_context_vulkan(Size2i p_internal_size, Size2i p_target_size) {
	XeSSContext &runtime = XeSSContext::get();
	ERR_FAIL_NULL_V(runtime.xessVKCreateContext, nullptr);
	ERR_FAIL_NULL_V(runtime.xessVKInit, nullptr);

	// Taken from the device driver directly: RenderingDevice::get_driver_resource()
	// does not expose the instance.
	RenderingDeviceDriverVulkan *rdd = static_cast<RenderingDeviceDriverVulkan *>(RD::get_singleton()->get_device_driver());
	ERR_FAIL_NULL_V(rdd, nullptr);

	xess_context_handle_t handle = nullptr;
	xess_result_t result = runtime.xessVKCreateContext(rdd->vulkan_instance_get(), rdd->vulkan_physical_device_get(), rdd->vulkan_device_get(), &handle);
	if (result != XESS_RESULT_SUCCESS) {
		ERR_PRINT(vformat("Intel XeSS: xessVKCreateContext failed (%s). Falling back to another upscaler.", XeSSContext::result_to_string(result)));
		return nullptr;
	}

	_attach_logging(handle);

	xess_vk_init_params_t init_params = {};
	init_params.outputResolution.x = p_target_size.width;
	init_params.outputResolution.y = p_target_size.height;
	init_params.qualitySetting = _quality_from_resolutions(p_internal_size, p_target_size);
	// Godot uses reverse-Z. Its motion vectors do not carry camera jitter (the FSR2
	// path treats them the same way), so XESS_INIT_FLAG_JITTERED_MV is left unset.
	init_params.initFlags = XESS_INIT_FLAG_INVERTED_DEPTH;

	result = runtime.xessVKInit(handle, &init_params);
	if (result != XESS_RESULT_SUCCESS) {
		ERR_PRINT(vformat("Intel XeSS: xessVKInit failed (%s). Falling back to another upscaler.", XeSSContext::result_to_string(result)));
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

bool XeSSEffect::_upscale_vulkan(const Parameters &p_params) {
	CallbackArgs *args = memnew(CallbackArgs);
	args->context = p_params.context->handle;
	args->scaler_context = p_params.context;
	args->color = image_view_info_from_rid(p_params.color, VK_IMAGE_ASPECT_COLOR_BIT);
	args->velocity = image_view_info_from_rid(p_params.velocity, VK_IMAGE_ASPECT_COLOR_BIT);
	args->output = image_view_info_from_rid(p_params.output, VK_IMAGE_ASPECT_COLOR_BIT);
	if (p_params.depth.is_valid()) {
		args->depth = image_view_info_from_rid(p_params.depth, VK_IMAGE_ASPECT_DEPTH_BIT);
		args->has_depth = true;
	}
	if (p_params.exposure.is_valid()) {
		args->exposure = image_view_info_from_rid(p_params.exposure, VK_IMAGE_ASPECT_COLOR_BIT);
		args->has_exposure = true;
	}
	args->jitter_x = p_params.jitter.x;
	args->jitter_y = p_params.jitter.y;
	args->reset = p_params.reset_accumulation ? 1 : 0;
	args->input_width = p_params.internal_size.width;
	args->input_height = p_params.internal_size.height;

	// Inputs are sampled, the output is written as a storage image.
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

#endif // XESS_ENABLED_VULKAN
