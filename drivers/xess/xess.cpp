/**************************************************************************/
/*  xess.cpp                                                              */
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

#ifdef XESS_ENABLED

#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/string/ustring.h"
#include "core/variant/variant.h"
#include "drivers/xess/xess_context.h"
#include "servers/rendering/rendering_device.h"

using namespace RendererRD;

XeSSScalerContext::~XeSSScalerContext() {
	if (handle != nullptr && XeSSContext::get().xessDestroyContext != nullptr) {
		// The context owns GPU resources that already recorded commands still reference,
		// and XeSS tracks no lifetimes of its own: destroying it while that work is in
		// flight loses the device. Wait for the queued frames first.
		RD::get_singleton()->_flush_and_stall_for_all_frames();
		XeSSContext::get().xessDestroyContext(handle);
		handle = nullptr;
	}
}

XeSSEffect::XeSSEffect() {
	if (!XeSSContext::get().load_functions()) {
		return;
	}

	const String driver_name = OS::get_singleton()->get_current_rendering_driver_name();
#ifdef XESS_ENABLED_VULKAN
	if (driver_name == "vulkan" && XeSSContext::get().has_vulkan()) {
		backend = BACKEND_VULKAN;
	}
#endif
#ifdef XESS_ENABLED_D3D12
	if (driver_name == "d3d12" && XeSSContext::get().has_d3d12()) {
		backend = BACKEND_D3D12;
	}
#endif

	supported = backend != BACKEND_NONE;
}

XeSSEffect::~XeSSEffect() {
}

// Maps the upscale ratio to the closest XeSS quality preset. XeSS sizes its
// internal resources from this; the per-frame input resolution is passed to
// execute, which supports dynamic resolution within the preset's range.
xess_quality_settings_t XeSSEffect::_quality_from_resolutions(Size2i p_internal_size, Size2i p_target_size) {
	float ratio = (p_internal_size.width > 0) ? float(p_target_size.width) / float(p_internal_size.width) : 1.0f;
	if (ratio >= 2.8f) {
		return XESS_QUALITY_SETTING_ULTRA_PERFORMANCE; // 3.0x
	} else if (ratio >= 1.85f) {
		return XESS_QUALITY_SETTING_PERFORMANCE; // 2.0x
	} else if (ratio >= 1.6f) {
		return XESS_QUALITY_SETTING_BALANCED; // 1.7x
	} else if (ratio >= 1.4f) {
		return XESS_QUALITY_SETTING_QUALITY; // 1.5x
	} else if (ratio >= 1.15f) {
		return XESS_QUALITY_SETTING_ULTRA_QUALITY; // 1.3x
	}
	return XESS_QUALITY_SETTING_AA; // 1.0x, native-resolution anti-aliasing.
}

static void _xess_log_callback(const char *p_message, xess_logging_level_t p_level) {
	if (p_level >= XESS_LOGGING_LEVEL_WARNING) {
		WARN_PRINT(vformat("Intel XeSS: %s", p_message));
	} else {
		print_verbose(vformat("Intel XeSS: %s", p_message));
	}
}

void XeSSEffect::_attach_logging(xess_context_handle_t p_handle) {
	if (XeSSContext::get().xessSetLoggingCallback != nullptr && OS::get_singleton()->is_stdout_verbose()) {
		XeSSContext::get().xessSetLoggingCallback(p_handle, XESS_LOGGING_LEVEL_DEBUG, _xess_log_callback);
	}
}

XeSSScalerContext *XeSSEffect::create_context(Size2i p_internal_size, Size2i p_target_size) {
	print_verbose(vformat("Intel XeSS: creating a %s context for %dx%d -> %dx%d.",
			backend == BACKEND_D3D12 ? "Direct3D 12" : "Vulkan",
			p_internal_size.width, p_internal_size.height, p_target_size.width, p_target_size.height));

	switch (backend) {
#ifdef XESS_ENABLED_VULKAN
		case BACKEND_VULKAN:
			return _create_context_vulkan(p_internal_size, p_target_size);
#endif
#ifdef XESS_ENABLED_D3D12
		case BACKEND_D3D12:
			return _create_context_d3d12(p_internal_size, p_target_size);
#endif
		default:
			return nullptr;
	}
}

bool XeSSEffect::upscale(const Parameters &p_params) {
	if (p_params.context == nullptr || p_params.context->handle == nullptr) {
		return false;
	}

	switch (backend) {
#ifdef XESS_ENABLED_VULKAN
		case BACKEND_VULKAN:
			return _upscale_vulkan(p_params);
#endif
#ifdef XESS_ENABLED_D3D12
		case BACKEND_D3D12:
			return _upscale_d3d12(p_params);
#endif
		default:
			return false;
	}
}

#endif // XESS_ENABLED
