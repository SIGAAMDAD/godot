/**************************************************************************/
/*  streamline_runtime.h                                                  */
/**************************************************************************/
#pragma once

#ifdef STREAMLINE_ENABLED

#include "core/string/ustring.h"
#include <sl.h>
#include <sl_dlss.h>
#ifdef VULKAN_ENABLED
#include "drivers/vulkan/godot_vulkan.h"
#include <sl_helpers_vk.h>
#else
namespace sl { struct VulkanInfo; }
#endif

// One device-scoped Streamline runtime.  All APIs are dynamically resolved
// from the matching SDK release's sl.interposer.dll.
class StreamlineRuntime {
public:
	static StreamlineRuntime &get();
	bool initialize(bool p_vulkan);
	bool bind_vulkan(const sl::VulkanInfo &p_info);
	bool bind_d3d12(void *p_device, void *p_adapter_luid);
	bool upgrade_interface(void **p_interface);
#ifdef VULKAN_ENABLED
	PFN_vkGetDeviceProcAddr get_vulkan_device_proc_addr() const;
	PFN_vkGetInstanceProcAddr get_vulkan_instance_proc_addr() const;
#endif
	bool is_ready() const { return initialized && device_bound && dlss_supported; }
	bool is_initialized() const { return initialized; }
	bool is_vulkan() const { return vulkan; }
	void shutdown();

	PFun_slGetFeatureRequirements *get_requirements = nullptr;
	PFun_slGetNewFrameToken *new_frame_token = nullptr;
	PFun_slSetTagForFrame *set_tag_for_frame = nullptr;
	PFun_slSetConstants *set_constants = nullptr;
	PFun_slEvaluateFeature *evaluate = nullptr;
	PFun_slFreeResources *free_resources = nullptr;
	PFun_slDLSSGetOptimalSettings *optimal_settings = nullptr;
	PFun_slDLSSSetOptions *set_options = nullptr;
	PFun_slIsFeatureSupported *feature_supported = nullptr;

private:
	bool initialized = false;
	bool device_bound = false;
	bool dlss_supported = false;
	bool vulkan = false;
	void *library = nullptr;
	PFun_slInit *sl_init = nullptr;
	PFun_slShutdown *sl_shutdown = nullptr;
	PFun_slSetD3DDevice *set_d3d_device = nullptr;
	#ifdef VULKAN_ENABLED
	PFun_slSetVulkanInfo *set_vulkan_info = nullptr;
#endif
	PFun_slGetFeatureFunction *get_feature_function = nullptr;
	PFun_slUpgradeInterface *upgrade_api = nullptr;
};
#endif
