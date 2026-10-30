/**************************************************************************/
/*  xess.h                                                                */
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

#include <atomic>
#include "core/math/vector2.h"
#include "core/math/vector2i.h"
#include "core/templates/rid.h"
#include "servers/rendering/rendering_device_driver.h"

// Only the API-independent XeSS header, so that including this from the renderer
// does not pull in the Vulkan or Direct3D 12 headers. The backend translation
// units include drivers/xess/xess_context.h for the rest.
#include <thirdparty/intel-xess/include/xess/xess.h>

namespace RendererRD {

// Per-viewport XeSS context, owning the handle created for one internal/target
// resolution pair. Named apart from the global runtime loader ::XeSSContext.
class XeSSScalerContext {
public:
	xess_context_handle_t handle = nullptr;
	Size2i internal_size;
	Size2i target_size;
	bool initialized = false;
	bool history_initialized = false;
	std::atomic<bool> execution_failed{ false };

	~XeSSScalerContext();
};

class XeSSEffect {
public:
	struct Parameters {
		XeSSScalerContext *context = nullptr;
		Size2i internal_size;
		RID color;
		RID depth;
		RID velocity;
		RID exposure;
		RID output;
		Vector2 jitter;
		bool reset_accumulation = false;
	};

	XeSSEffect();
	~XeSSEffect();

	// True when the XeSS runtime is present and exposes the backend matching the
	// active rendering driver.
	bool is_supported() const { return supported; }

	// Returns nullptr on failure, in which case the caller falls back to another
	// upscaler.
	XeSSScalerContext *create_context(Size2i p_internal_size, Size2i p_target_size);

	// Records the XeSS upscaling pass into the render graph.
	bool upscale(const Parameters &p_params);

private:
	// XeSS exposes a separate entry point set per graphics API, and they take native
	// handles, so the backend must match the driver the renderer is running on.
	enum Backend {
		BACKEND_NONE,
		BACKEND_VULKAN,
		BACKEND_D3D12,
	};

	static xess_quality_settings_t _quality_from_resolutions(Size2i p_internal_size, Size2i p_target_size);

	// Routes the runtime's own diagnostics to the verbose log.
	static void _attach_logging(xess_context_handle_t p_handle);

#ifdef XESS_ENABLED_VULKAN
	// Defined in xess_vulkan.cpp.
	XeSSScalerContext *_create_context_vulkan(Size2i p_internal_size, Size2i p_target_size);
	bool _upscale_vulkan(const Parameters &p_params);
#endif

#ifdef XESS_ENABLED_D3D12
	// Defined in xess_d3d12.cpp.
	XeSSScalerContext *_create_context_d3d12(Size2i p_internal_size, Size2i p_target_size);
	bool _upscale_d3d12(const Parameters &p_params);
#endif

	Backend backend = BACKEND_NONE;
	bool supported = false;
};

} // namespace RendererRD

#endif // XESS_ENABLED
