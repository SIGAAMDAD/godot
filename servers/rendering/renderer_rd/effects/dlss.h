/**************************************************************************/
/*  dlss.h                                                                */
/**************************************************************************/
#pragma once
#ifdef STREAMLINE_ENABLED
#include <atomic>
#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2.h"
#include "core/templates/rid.h"
#include "drivers/streamline/streamline_runtime.h"
#include "servers/rendering/rendering_device_driver.h"

namespace RendererRD {
class DLSSContext {
public:
	sl::ViewportHandle viewport;
	sl::DLSSOptions options = {};
	Size2i internal_size;
	Size2i target_size;
	std::atomic<bool> execution_failed{ false };
	bool history_initialized = false;
	DLSSContext(uint32_t p_viewport) : viewport(p_viewport) {}
	~DLSSContext();
};

class DLSSEffect {
public:
	struct Parameters {
		DLSSContext *context = nullptr;
		RID color;
		RID depth;
		RID velocity;
		RID exposure;
		RID output;
		Size2i internal_size;
		Vector2 jitter;
		float near_plane = 0.1f;
		float far_plane = 1000.0f;
		float fovy = 1.0f;
		Projection reprojection;
		Projection camera_projection;
		Transform3D camera_transform;
		bool reset_history = false;
	};
	bool is_supported() const { return StreamlineRuntime::get().is_ready(); }
	DLSSContext *create_context(Size2i p_internal, Size2i p_target);
	bool upscale(const Parameters &p_params);
};
}
#endif
