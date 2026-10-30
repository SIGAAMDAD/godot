/**************************************************************************/
/*  xess_context.h                                                        */
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

#ifdef XESS_ENABLED

#include "drivers/xess/xess_headers.h"

// libxess.dll is loaded dynamically instead of linked at build time: a missing
// runtime leaves the pointers null and XeSS reports itself unavailable rather
// than blocking startup. The typedefs mirror the vendored XeSS headers.

typedef xess_result_t (*PFN_xessGetVersion)(xess_version_t *p_version);
typedef xess_result_t (*PFN_xessDestroyContext)(xess_context_handle_t p_context);
typedef xess_result_t (*PFN_xessGetInputResolution)(xess_context_handle_t p_context, const xess_2d_t *p_output_resolution, xess_quality_settings_t p_quality, xess_2d_t *r_input_resolution);
typedef xess_result_t (*PFN_xessSetVelocityScale)(xess_context_handle_t p_context, float p_x, float p_y);
typedef xess_result_t (*PFN_xessSetJitterScale)(xess_context_handle_t p_context, float p_x, float p_y);
typedef xess_result_t (*PFN_xessSetLoggingCallback)(xess_context_handle_t p_context, xess_logging_level_t p_level, xess_app_log_callback_t p_callback);

#ifdef XESS_ENABLED_VULKAN
typedef xess_result_t (*PFN_xessVKGetRequiredInstanceExtensions)(uint32_t *r_count, const char *const **r_extensions, uint32_t *r_min_api_version);
typedef xess_result_t (*PFN_xessVKGetRequiredDeviceExtensions)(VkInstance p_instance, VkPhysicalDevice p_physical_device, uint32_t *r_count, const char *const **r_extensions);
typedef xess_result_t (*PFN_xessVKGetRequiredDeviceFeatures)(VkInstance p_instance, VkPhysicalDevice p_physical_device, void **r_features);
typedef xess_result_t (*PFN_xessVKCreateContext)(VkInstance p_instance, VkPhysicalDevice p_physical_device, VkDevice p_device, xess_context_handle_t *r_context);
typedef xess_result_t (*PFN_xessVKBuildPipelines)(xess_context_handle_t p_context, VkPipelineCache p_pipeline_cache, bool p_blocking, uint32_t p_init_flags);
typedef xess_result_t (*PFN_xessVKInit)(xess_context_handle_t p_context, const xess_vk_init_params_t *p_init_params);
typedef xess_result_t (*PFN_xessVKExecute)(xess_context_handle_t p_context, VkCommandBuffer p_command_buffer, const xess_vk_execute_params_t *p_exec_params);
#endif

#ifdef XESS_ENABLED_D3D12
typedef xess_result_t (*PFN_xessD3D12CreateContext)(ID3D12Device *p_device, xess_context_handle_t *r_context);
typedef xess_result_t (*PFN_xessD3D12BuildPipelines)(xess_context_handle_t p_context, ID3D12PipelineLibrary *p_pipeline_library, bool p_blocking, uint32_t p_init_flags);
typedef xess_result_t (*PFN_xessD3D12Init)(xess_context_handle_t p_context, const xess_d3d12_init_params_t *p_init_params);
typedef xess_result_t (*PFN_xessD3D12Execute)(xess_context_handle_t p_context, ID3D12GraphicsCommandList *p_command_list, const xess_d3d12_execute_params_t *p_exec_params);
#endif

// Process-wide loader for the XeSS runtime entry points.
class XeSSContext {
	bool library_loaded = false;
	void *library_handle = nullptr;
#ifdef XESS_ENABLED_VULKAN
	bool vulkan_available = false;
#endif
#ifdef XESS_ENABLED_D3D12
	bool d3d12_available = false;
#endif

public:
	// Backend-independent entry points.
	PFN_xessGetVersion xessGetVersion = nullptr;
	PFN_xessDestroyContext xessDestroyContext = nullptr;
	PFN_xessGetInputResolution xessGetInputResolution = nullptr;
	PFN_xessSetVelocityScale xessSetVelocityScale = nullptr;
	PFN_xessSetJitterScale xessSetJitterScale = nullptr;
	PFN_xessSetLoggingCallback xessSetLoggingCallback = nullptr;

#ifdef XESS_ENABLED_VULKAN
	PFN_xessVKGetRequiredInstanceExtensions xessVKGetRequiredInstanceExtensions = nullptr;
	PFN_xessVKGetRequiredDeviceExtensions xessVKGetRequiredDeviceExtensions = nullptr;
	PFN_xessVKGetRequiredDeviceFeatures xessVKGetRequiredDeviceFeatures = nullptr;
	PFN_xessVKCreateContext xessVKCreateContext = nullptr;
	PFN_xessVKBuildPipelines xessVKBuildPipelines = nullptr;
	PFN_xessVKInit xessVKInit = nullptr;
	PFN_xessVKExecute xessVKExecute = nullptr;
#endif

#ifdef XESS_ENABLED_D3D12
	PFN_xessD3D12CreateContext xessD3D12CreateContext = nullptr;
	PFN_xessD3D12BuildPipelines xessD3D12BuildPipelines = nullptr;
	PFN_xessD3D12Init xessD3D12Init = nullptr;
	PFN_xessD3D12Execute xessD3D12Execute = nullptr;
#endif

	// Loads libxess.dll and resolves the entry points. Only the first call does work.
	bool load_functions();
	bool is_loaded() const { return library_loaded; }

	// True when every entry point of that backend resolved. Backends are resolved
	// independently, so a runtime shipping only one still leaves the other usable.
	bool has_vulkan() const {
#ifdef XESS_ENABLED_VULKAN
		return vulkan_available;
#else
		return false;
#endif
	}

	bool has_d3d12() const {
#ifdef XESS_ENABLED_D3D12
		return d3d12_available;
#else
		return false;
#endif
	}

	static const char *result_to_string(xess_result_t p_result);

	static XeSSContext &get();
};

#endif // XESS_ENABLED
