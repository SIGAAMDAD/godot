/**************************************************************************/
/*  rendering_context_driver_vulkan_windows.cpp                           */
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

#if defined(WINDOWS_ENABLED) && defined(VULKAN_ENABLED)

#include "rendering_context_driver_vulkan_windows.h"

#include "core/os/os.h"

#include <drivers/vulkan/godot_vulkan.h>
#ifdef STREAMLINE_ENABLED
#include "drivers/streamline/streamline_runtime.h"
#endif

const char *RenderingContextDriverVulkanWindows::_get_platform_surface_extension() const {
	return VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
}

RenderingContextDriverVulkanWindows::RenderingContextDriverVulkanWindows() {
	// Workaround for Vulkan not working on setups with AMD integrated graphics + NVIDIA dedicated GPU (GH-57708).
	// This prevents using AMD integrated graphics with Vulkan entirely, but it allows the engine to start
	// even on outdated/broken driver setups.
	OS::get_singleton()->set_environment("DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1", "1");
}

RenderingContextDriverVulkanWindows::~RenderingContextDriverVulkanWindows() {
	// Does nothing.
}

RenderingContextDriver::SurfaceID RenderingContextDriverVulkanWindows::surface_create(const void *p_platform_data) {
	const WindowPlatformData *wpd = (const WindowPlatformData *)(p_platform_data);

	VkWin32SurfaceCreateInfoKHR create_info = {};
	create_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
	create_info.hinstance = wpd->instance;
	create_info.hwnd = wpd->window;

	VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
	PFN_vkCreateWin32SurfaceKHR create_surface = vkCreateWin32SurfaceKHR;
#ifdef STREAMLINE_ENABLED
	if (StreamlineRuntime::get().is_initialized()) {
		PFN_vkGetInstanceProcAddr proxy_get = StreamlineRuntime::get().get_vulkan_instance_proc_addr();
		if (proxy_get) {
			PFN_vkCreateWin32SurfaceKHR proxy_create = (PFN_vkCreateWin32SurfaceKHR)proxy_get(instance_get(), "vkCreateWin32SurfaceKHR");
			if (proxy_create != nullptr) {
				create_surface = proxy_create;
			}
		}
	}
#endif
	VkResult err = create_surface(instance_get(), &create_info, get_allocation_callbacks(VK_OBJECT_TYPE_SURFACE_KHR), &vk_surface);
	ERR_FAIL_COND_V(err != VK_SUCCESS, SurfaceID());

	Surface *surface = memnew(Surface);
	surface->vk_surface = vk_surface;
	return SurfaceID(surface);
}

#endif // WINDOWS_ENABLED && VULKAN_ENABLED
