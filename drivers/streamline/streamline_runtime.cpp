/**************************************************************************/
/*  streamline_runtime.cpp                                                */
/**************************************************************************/
#include "streamline_runtime.h"
#ifdef STREAMLINE_ENABLED

#include "core/error/error_macros.h"
#include "core/string/print_string.h"
#include <windows.h>

StreamlineRuntime &StreamlineRuntime::get() {
	static StreamlineRuntime runtime;
	return runtime;
}

bool StreamlineRuntime::initialize(bool p_vulkan) {
	if (initialized) {
		return vulkan == p_vulkan;
	}
	vulkan = p_vulkan;
	HMODULE module = LoadLibraryW(L"sl.interposer.dll");
	if (module == nullptr) {
		return false;
	}
	library = module;
#define IMPORT(member, symbol, type) member = reinterpret_cast<type *>(GetProcAddress(module, symbol))
	IMPORT(sl_init, "slInit", PFun_slInit);
	IMPORT(sl_shutdown, "slShutdown", PFun_slShutdown);
	IMPORT(set_d3d_device, "slSetD3DDevice", PFun_slSetD3DDevice);
#ifdef VULKAN_ENABLED
	IMPORT(set_vulkan_info, "slSetVulkanInfo", PFun_slSetVulkanInfo);
#endif
	IMPORT(get_requirements, "slGetFeatureRequirements", PFun_slGetFeatureRequirements);
	IMPORT(new_frame_token, "slGetNewFrameToken", PFun_slGetNewFrameToken);
	IMPORT(set_tag_for_frame, "slSetTagForFrame", PFun_slSetTagForFrame);
	IMPORT(set_constants, "slSetConstants", PFun_slSetConstants);
	IMPORT(evaluate, "slEvaluateFeature", PFun_slEvaluateFeature);
	IMPORT(free_resources, "slFreeResources", PFun_slFreeResources);
	IMPORT(feature_supported, "slIsFeatureSupported", PFun_slIsFeatureSupported);
	IMPORT(get_feature_function, "slGetFeatureFunction", PFun_slGetFeatureFunction);
	IMPORT(upgrade_api, "slUpgradeInterface", PFun_slUpgradeInterface);
#undef IMPORT
	if (!sl_init || !sl_shutdown || !new_frame_token || !set_tag_for_frame || !set_constants || !evaluate || !get_feature_function || !feature_supported || (!vulkan && !set_d3d_device)
#ifdef VULKAN_ENABLED
			|| (vulkan && !set_vulkan_info)
#else
			|| vulkan
#endif
	) {
		shutdown();
		return false;
	}
	const sl::Feature feature = sl::kFeatureDLSS;
	sl::Preferences preferences{};
	preferences.featuresToLoad = &feature;
	preferences.numFeaturesToLoad = 1;
	preferences.renderAPI = vulkan ? sl::RenderAPI::eVulkan : sl::RenderAPI::eD3D12;
	preferences.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
	preferences.engine = sl::EngineType::eCustom;
	preferences.engineVersion = "4.8";
	// The application identity can be supplied by the consuming title.  Never
	// borrow an application ID assigned to another engine or game.
	if (sl_init(preferences, sl::kSDKVersion) != sl::Result::eOk) {
		shutdown();
		return false;
	}
	initialized = true;
	return true;
}

bool StreamlineRuntime::bind_vulkan(const sl::VulkanInfo &p_info) {
#ifdef VULKAN_ENABLED
	if (!initialized || !vulkan || !set_vulkan_info || set_vulkan_info(p_info) != sl::Result::eOk) {
		return false;
	}
	device_bound = true;
	sl::AdapterInfo adapter{};
	adapter.vkPhysicalDevice = p_info.physicalDevice;
	dlss_supported = feature_supported(sl::kFeatureDLSS, adapter) == sl::Result::eOk;
	if (dlss_supported) {
		optimal_settings = nullptr;
		set_options = nullptr;
		get_feature_function(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", (void *&)optimal_settings);
		get_feature_function(sl::kFeatureDLSS, "slDLSSSetOptions", (void *&)set_options);
		dlss_supported = optimal_settings && set_options;
	}
	return is_ready();
#else
	return false;
#endif
}

bool StreamlineRuntime::bind_d3d12(void *p_device, void *p_adapter_luid) {
	if (!initialized || vulkan || !set_d3d_device || set_d3d_device(p_device) != sl::Result::eOk) {
		return false;
	}
	device_bound = true;
	// The concrete adapter LUID must be checked by the device owner; do not
	// report support solely from successful Streamline initialization.
	sl::AdapterInfo adapter{};
	adapter.deviceLUID = (uint8_t *)p_adapter_luid;
	adapter.deviceLUIDSizeInBytes = 8;
	dlss_supported = feature_supported(sl::kFeatureDLSS, adapter) == sl::Result::eOk;
	if (dlss_supported) {
		get_feature_function(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", (void *&)optimal_settings);
		get_feature_function(sl::kFeatureDLSS, "slDLSSSetOptions", (void *&)set_options);
		dlss_supported = optimal_settings && set_options;
	}
	return is_ready();
}

bool StreamlineRuntime::upgrade_interface(void **p_interface) {
	return initialized && upgrade_api && upgrade_api(p_interface) == sl::Result::eOk;
}

#ifdef VULKAN_ENABLED
PFN_vkGetDeviceProcAddr StreamlineRuntime::get_vulkan_device_proc_addr() const {
	return library ? (PFN_vkGetDeviceProcAddr)GetProcAddress((HMODULE)library, "vkGetDeviceProcAddr") : nullptr;
}

PFN_vkGetInstanceProcAddr StreamlineRuntime::get_vulkan_instance_proc_addr() const {
	return library ? (PFN_vkGetInstanceProcAddr)GetProcAddress((HMODULE)library, "vkGetInstanceProcAddr") : nullptr;
}
#endif

void StreamlineRuntime::shutdown() {
	if (initialized && sl_shutdown) {
		sl_shutdown();
	}
	initialized = false;
	device_bound = false;
	dlss_supported = false;
	optimal_settings = nullptr;
	set_options = nullptr;
	if (library) {
		FreeLibrary((HMODULE)library);
		library = nullptr;
	}
}
#endif
