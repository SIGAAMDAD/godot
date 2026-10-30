/**************************************************************************/
/*  xess_context.cpp                                                      */
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

#include "xess_context.h"

#ifdef XESS_ENABLED

#include "core/string/print_string.h"
#include "core/string/ustring.h"
#include "core/variant/variant.h" // For vformat().

#ifdef _WIN32
// Keep <windows.h> last and lean so its macros do not leak into the headers above.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

XeSSContext &XeSSContext::get() {
	static XeSSContext context;
	return context;
}

const char *XeSSContext::result_to_string(xess_result_t p_result) {
	switch (p_result) {
		case XESS_RESULT_WARNING_NONEXISTING_FOLDER:
			return "Warning: non-existing folder";
		case XESS_RESULT_WARNING_OLD_DRIVER:
			return "Warning: old driver";
		case XESS_RESULT_SUCCESS:
			return "Success";
		case XESS_RESULT_ERROR_UNSUPPORTED_DEVICE:
			return "Unsupported device";
		case XESS_RESULT_ERROR_UNSUPPORTED_DRIVER:
			return "Unsupported driver";
		case XESS_RESULT_ERROR_UNINITIALIZED:
			return "Uninitialized";
		case XESS_RESULT_ERROR_INVALID_ARGUMENT:
			return "Invalid argument";
		case XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY:
			return "Device out of memory";
		case XESS_RESULT_ERROR_DEVICE:
			return "Device error";
		case XESS_RESULT_ERROR_NOT_IMPLEMENTED:
			return "Not implemented";
		case XESS_RESULT_ERROR_INVALID_CONTEXT:
			return "Invalid context";
		case XESS_RESULT_ERROR_OPERATION_IN_PROGRESS:
			return "Operation in progress";
		case XESS_RESULT_ERROR_UNSUPPORTED:
			return "Unsupported";
		case XESS_RESULT_ERROR_CANT_LOAD_LIBRARY:
			return "Cannot load library";
		case XESS_RESULT_ERROR_WRONG_CALL_ORDER:
			return "Wrong call order";
		case XESS_RESULT_ERROR_UNKNOWN:
			return "Unknown error";
		default:
			return "Unrecognized result";
	}
}

bool XeSSContext::load_functions() {
	if (library_loaded) {
		return true;
	}

#ifdef _WIN32
	HMODULE lib = LoadLibraryA("libxess.dll");
	if (lib == nullptr) {
		print_verbose("Intel XeSS: libxess.dll could not be loaded; XeSS support is unavailable. Copy libxess.dll next to the executable to enable it.");
		return false;
	}

	// Backend-independent entry points; without them XeSS is unusable.
#define XESS_GETPROC(m_var, m_type, m_name) \
	m_var = (m_type)(void *)GetProcAddress(lib, m_name); \
	if (m_var == nullptr) { \
		print_verbose("Intel XeSS: entry point '" m_name "' missing from libxess.dll; XeSS support is unavailable."); \
		FreeLibrary(lib); \
		xessGetVersion = nullptr; \
		xessDestroyContext = nullptr; \
		xessGetInputResolution = nullptr; \
		xessSetVelocityScale = nullptr; \
		xessSetJitterScale = nullptr; \
		xessSetLoggingCallback = nullptr; \
		return false; \
	}

	// Backend entry points. libxess.dll carries both the Vulkan and the Direct3D 12
	// exports, but each backend is resolved on its own so that a runtime missing one
	// of them does not disable the other.
#define XESS_GETPROC_BACKEND(m_var, m_type, m_name, m_available) \
	m_var = (m_type)(void *)GetProcAddress(lib, m_name); \
	if (m_var == nullptr) { \
		print_verbose("Intel XeSS: entry point '" m_name "' missing from libxess.dll."); \
		m_available = false; \
	}

	XESS_GETPROC(xessGetVersion, PFN_xessGetVersion, "xessGetVersion");
	XESS_GETPROC(xessDestroyContext, PFN_xessDestroyContext, "xessDestroyContext");
	XESS_GETPROC(xessGetInputResolution, PFN_xessGetInputResolution, "xessGetInputResolution");
	XESS_GETPROC(xessSetVelocityScale, PFN_xessSetVelocityScale, "xessSetVelocityScale");
	XESS_GETPROC(xessSetJitterScale, PFN_xessSetJitterScale, "xessSetJitterScale");
	XESS_GETPROC(xessSetLoggingCallback, PFN_xessSetLoggingCallback, "xessSetLoggingCallback");

#ifdef XESS_ENABLED_VULKAN
	vulkan_available = true;
	XESS_GETPROC_BACKEND(xessVKGetRequiredInstanceExtensions, PFN_xessVKGetRequiredInstanceExtensions, "xessVKGetRequiredInstanceExtensions", vulkan_available);
	XESS_GETPROC_BACKEND(xessVKGetRequiredDeviceExtensions, PFN_xessVKGetRequiredDeviceExtensions, "xessVKGetRequiredDeviceExtensions", vulkan_available);
	XESS_GETPROC_BACKEND(xessVKGetRequiredDeviceFeatures, PFN_xessVKGetRequiredDeviceFeatures, "xessVKGetRequiredDeviceFeatures", vulkan_available);
	XESS_GETPROC_BACKEND(xessVKCreateContext, PFN_xessVKCreateContext, "xessVKCreateContext", vulkan_available);
	XESS_GETPROC_BACKEND(xessVKBuildPipelines, PFN_xessVKBuildPipelines, "xessVKBuildPipelines", vulkan_available);
	XESS_GETPROC_BACKEND(xessVKInit, PFN_xessVKInit, "xessVKInit", vulkan_available);
	XESS_GETPROC_BACKEND(xessVKExecute, PFN_xessVKExecute, "xessVKExecute", vulkan_available);
#endif

#ifdef XESS_ENABLED_D3D12
	d3d12_available = true;
	XESS_GETPROC_BACKEND(xessD3D12CreateContext, PFN_xessD3D12CreateContext, "xessD3D12CreateContext", d3d12_available);
	XESS_GETPROC_BACKEND(xessD3D12BuildPipelines, PFN_xessD3D12BuildPipelines, "xessD3D12BuildPipelines", d3d12_available);
	XESS_GETPROC_BACKEND(xessD3D12Init, PFN_xessD3D12Init, "xessD3D12Init", d3d12_available);
	XESS_GETPROC_BACKEND(xessD3D12Execute, PFN_xessD3D12Execute, "xessD3D12Execute", d3d12_available);
#endif

#undef XESS_GETPROC_BACKEND
#undef XESS_GETPROC

	if (!has_vulkan() && !has_d3d12()) {
		print_verbose("Intel XeSS: libxess.dll exposes no usable backend; XeSS support is unavailable.");
		FreeLibrary(lib);
		return false;
	}

	library_handle = (void *)lib;
	library_loaded = true;

	xess_version_t version = {};
	if (xessGetVersion(&version) == XESS_RESULT_SUCCESS) {
		print_verbose(vformat("Intel XeSS runtime loaded (version %d.%d.%d).", version.major, version.minor, version.patch));
	}

	return true;
#else
	// XeSS is only distributed for Windows.
	return false;
#endif
}

#endif // XESS_ENABLED
