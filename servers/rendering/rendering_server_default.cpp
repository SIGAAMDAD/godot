/**************************************************************************/
/*  rendering_server_default.cpp                                          */
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

#include "rendering_server_default.h"

#include "core/memory_tracking/memory_tracker.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/profiling/profiling.h"
#include "servers/display/display_server.h"
#include "servers/rendering/renderer_canvas_cull.h"
#include "servers/rendering/renderer_scene_cull.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_globals.h"

#ifndef XR_DISABLED
#include "servers/xr/xr_server.h"
#endif

/* FREE */

void RenderingServerDefault::_free(RID p_rid) {
	if (unlikely(p_rid.is_null())) {
		return;
	}
	if (RSG::utilities->free(p_rid)) {
		return;
	}
	if (RSG::canvas->free(p_rid)) {
		return;
	}
	if (RSG::viewport->free(p_rid)) {
		return;
	}
	if (RSG::scene->free(p_rid)) {
		return;
	}
}

/* EVENT QUEUING */

void RenderingServerDefault::request_frame_drawn_callback(const Callable &p_callable) {
	frame_drawn_callbacks.push_back(p_callable);
}

void RenderingServerDefault::request_frame_stage_callback(RenderingServerTypes::RenderFrameCallbackStage p_stage, const Callable &p_callable) {
	if (!p_callable.is_valid()) {
		return;
	}
	MutexLock admission(command_admission_mutex);
	if (command_admission != CommandAdmissionState::RUNNING) {
		return;
	}
	MutexLock lock(frame_callback_mutex);
	pending_frame_callbacks.push_back({ p_stage, p_callable, {} });
}

void RenderingServerDefault::_draw(bool p_swap_buffers, double frame_step, uint64_t p_frame_id) {
	const uint64_t render_frame_begin_us = OS::get_singleton()->get_ticks_usec();
	GodotProfileZoneGroupedFirst(_profile_zone, "rasterizer->begin_frame");
	RSG::rasterizer->begin_frame(frame_step);

	TIMESTAMP_BEGIN()

	uint64_t time_usec = OS::get_singleton()->get_ticks_usec();

	RENDER_TIMESTAMP("Prepare Render Frame");

#ifndef XR_DISABLED
	GodotProfileZoneGrouped(_profile_zone, "xr_server->pre_render");
	XRServer *xr_server = XRServer::get_singleton();
	if (xr_server != nullptr) {
		// Let XR server know we're about to render a frame.
		xr_server->pre_render();
	}
#endif // XR_DISABLED

	GodotProfileZoneGrouped(_profile_zone, "scene->update");
	RSG::scene->update(); //update scenes stuff before updating instances
	GodotProfileZoneGrouped(_profile_zone, "canvas->update");
	RSG::canvas->update();

	frame_setup_time = double(OS::get_singleton()->get_ticks_usec() - time_usec) / 1000.0;

	GodotProfileZoneGrouped(_profile_zone, "particles_storage->update_particles");
	RSG::particles_storage->update_particles(); //need to be done after instances are updated (colliders and particle transforms), and colliders are rendered

	GodotProfileZoneGrouped(_profile_zone, "scene->render_probes");
	RSG::scene->render_probes();

	GodotProfileZoneGrouped(_profile_zone, "viewport->draw_viewports");
	RSG::viewport->draw_viewports(p_swap_buffers);

	GodotProfileZoneGrouped(_profile_zone, "canvas_render->update");
	RSG::canvas_render->update();

	GodotProfileZoneGrouped(_profile_zone, "rasterizer->end_frame");
	RSG::rasterizer->end_frame(p_swap_buffers);
	{
		// A frame's GPU submission and its eventual fence completion are distinct.
		RenderingServerTypes::RenderFrameToken token{ p_frame_id, 0 };
#ifdef RD_ENABLED
		RenderingDevice *rd = RenderingDevice::get_singleton();
		if (rd != nullptr && !rd->is_finalized()) {
			token.gpu_submission = rd->get_last_submitted_gpu_frame();
		}
		const uint64_t completed_gpu = rd != nullptr && !rd->is_finalized() ? rd->get_last_completed_gpu_frame() : 0;
#else
		const uint64_t completed_gpu = 0;
#endif
		MutexLock lock(frame_callback_mutex);
		for (List<FrameStageCallback>::Element *E = pending_frame_callbacks.front(); E;) {
			List<FrameStageCallback>::Element *next = E->next();
			if (E->get().stage != RenderingServerTypes::RenderFrameCallbackStage::BEFORE_DRAW) {
				FrameStageCallback callback = E->get();
				callback.token = token;
				if (callback.stage == RenderingServerTypes::RenderFrameCallbackStage::AFTER_GPU_COMPLETE && token.gpu_submission != 0) {
					awaiting_gpu_callbacks.push_back(callback);
				} else if (callback.stage == RenderingServerTypes::RenderFrameCallbackStage::AFTER_GPU_SUBMIT) {
					main_frame_callbacks.push_back(callback);
				}
				pending_frame_callbacks.erase(E);
			}
			E = next;
		}
		for (List<FrameStageCallback>::Element *E = awaiting_gpu_callbacks.front(); E;) {
			List<FrameStageCallback>::Element *next = E->next();
			if (completed_gpu >= E->get().token.gpu_submission) {
				main_frame_callbacks.push_back(E->get());
				awaiting_gpu_callbacks.erase(E);
			}
			E = next;
		}
	}

#ifndef XR_DISABLED
	if (xr_server != nullptr) {
		GodotProfileZone("xr_server->end_frame");
		// let our XR server know we're done so we can get our frame timing
		xr_server->end_frame();
	}
#endif // XR_DISABLED

	GodotProfileZoneGrouped(_profile_zone, "update_visibility_notifiers");
	RSG::canvas->update_visibility_notifiers();
	RSG::scene->update_visibility_notifiers();

	GodotProfileZoneGrouped(_profile_zone, "post_draw_steps");
	if (create_thread) {
		callable_mp(this, &RenderingServerDefault::_run_post_draw_steps).call_deferred();
	} else {
		_run_post_draw_steps();
	}

	if (RSG::utilities->get_captured_timestamps_count()) {
		GodotProfileZoneGrouped(_profile_zone, "frame_profile");
		Vector<RenderingServerTypes::FrameProfileArea> new_profile;
		if (RSG::utilities->capturing_timestamps) {
			new_profile.resize(RSG::utilities->get_captured_timestamps_count());
		}

		uint64_t base_cpu = RSG::utilities->get_captured_timestamp_cpu_time(0);
		uint64_t base_gpu = RSG::utilities->get_captured_timestamp_gpu_time(0);
		for (uint32_t i = 0; i < RSG::utilities->get_captured_timestamps_count(); i++) {
			uint64_t time_cpu = RSG::utilities->get_captured_timestamp_cpu_time(i);
			uint64_t time_gpu = RSG::utilities->get_captured_timestamp_gpu_time(i);

			String name = RSG::utilities->get_captured_timestamp_name(i);

			if (name.begins_with("vp_")) {
				RSG::viewport->handle_timestamp(name, time_cpu, time_gpu);
			}

			if (RSG::utilities->capturing_timestamps) {
				new_profile.write[i].gpu_msec = double((time_gpu - base_gpu) / 1000) / 1000.0;
				new_profile.write[i].cpu_msec = double(time_cpu - base_cpu) / 1000.0;
				new_profile.write[i].name = RSG::utilities->get_captured_timestamp_name(i);
			}
		}

		frame_profile = new_profile;
	}

	frame_profile_frame = RSG::utilities->get_captured_timestamps_frame();
	{
		MutexLock lock(profile_snapshot_mutex);
		profile_snapshot.frame_id = frame_profile_frame;
		profile_snapshot.areas = frame_profile;
	}

	if (print_gpu_profile) {
		GodotProfileZoneGrouped(_profile_zone, "gpu_profile");
		if (print_frame_profile_ticks_from == 0) {
			print_frame_profile_ticks_from = OS::get_singleton()->get_ticks_usec();
		}
		double total_time = 0.0;

		for (int i = 0; i < frame_profile.size() - 1; i++) {
			String name = frame_profile[i].name;
			if (name[0] == '<' || name[0] == '>') {
				continue;
			}

			double time = frame_profile[i + 1].gpu_msec - frame_profile[i].gpu_msec;

			if (print_gpu_profile_task_time.has(name)) {
				print_gpu_profile_task_time[name] += time;
			} else {
				print_gpu_profile_task_time[name] = time;
			}
		}

		if (frame_profile.size()) {
			total_time = frame_profile[frame_profile.size() - 1].gpu_msec;
		}

		uint64_t ticks_elapsed = OS::get_singleton()->get_ticks_usec() - print_frame_profile_ticks_from;
		print_frame_profile_frame_count++;
		if (ticks_elapsed > 1000000) {
			print_line("GPU PROFILE (total " + rtos(total_time) + "ms): ");

			float print_threshold = 0.01;
			for (const KeyValue<String, float> &E : print_gpu_profile_task_time) {
				double time = E.value / double(print_frame_profile_frame_count);
				if (time > print_threshold) {
					print_line("\t-" + E.key + ": " + rtos(time) + "ms");
				}
			}
			print_gpu_profile_task_time.clear();
			print_frame_profile_ticks_from = OS::get_singleton()->get_ticks_usec();
			print_frame_profile_frame_count = 0;
		}
	}

	GodotProfileZoneGrouped(_profile_zone, "memory_info");
	RSG::utilities->update_memory_info();
	uint64_t updated_info[RSE::RENDERING_INFO_MAX] = {};
	for (int i = 0; i < RSE::RENDERING_INFO_MAX; i++) {
		updated_info[i] = _read_rendering_info(RSE::RenderingInfo(i));
	}
	{
		MutexLock lock(rendering_info_mutex);
		for (int i = 0; i < RSE::RENDERING_INFO_MAX; i++) {
			rendering_info_snapshot[i] = updated_info[i];
		}
		gpu_memory_snapshot.textures = updated_info[RSE::RENDERING_INFO_TEXTURE_MEM_USED];
		gpu_memory_snapshot.buffers = updated_info[RSE::RENDERING_INFO_BUFFER_MEM_USED];
		gpu_memory_snapshot.total = updated_info[RSE::RENDERING_INFO_VIDEO_MEM_USED];
		gpu_memory_snapshot.frame_id = p_frame_id;
		published_frame_setup_time = frame_setup_time;
		rendering_info_available = true;
	}
	GodotMemoryTracking::publish_completed_render_frame(p_frame_id, OS::get_singleton()->get_ticks_usec() - render_frame_begin_us);
}

void RenderingServerDefault::_drain_frame_stage_callbacks_main() {
	DEV_ASSERT(Thread::is_main_thread());
	List<FrameStageCallback> ready;
	{
		MutexLock lock(frame_callback_mutex);
		while (main_frame_callbacks.front()) {
			ready.push_back(main_frame_callbacks.front()->get());
			main_frame_callbacks.pop_front();
		}
	}
	for (const FrameStageCallback &callback : ready) {
		if (callback.callback.is_valid()) {
			callback.callback.call(callback.token.frame_id);
		}
	}
}

void RenderingServerDefault::_run_post_draw_steps() {
	_drain_frame_stage_callbacks_main();
	while (frame_drawn_callbacks.front()) {
		Callable c = frame_drawn_callbacks.front()->get();
		Variant result;
		Callable::CallError ce;
		c.callp(nullptr, 0, result, ce);
		if (ce.error != Callable::CallError::CALL_OK) {
			String err = Variant::get_callable_error_text(c, nullptr, 0, ce);
			ERR_PRINT("Error calling frame drawn function: " + err);
		}

		frame_drawn_callbacks.pop_front();
	}

	emit_signal(SNAME("frame_post_draw"));
}

double RenderingServerDefault::get_frame_setup_time_cpu() const {
	MutexLock lock(rendering_info_mutex);
	return published_frame_setup_time;
}

bool RenderingServerDefault::has_changed() const {
	return change_generation.load(std::memory_order_acquire) != last_submitted_generation.load(std::memory_order_acquire);
}

void RenderingServerDefault::_init() {
	RSG::threaded = create_thread;

	RSG::canvas = memnew(RendererCanvasCull);
	RSG::viewport = memnew(RendererViewport);
	RendererSceneCull *sr = memnew(RendererSceneCull);
	RSG::camera_attributes = memnew(RendererCameraAttributes);
	RSG::scene = sr;
	RSG::rasterizer = RendererCompositor::create();
	RSG::utilities = RSG::rasterizer->get_utilities();
	RSG::rasterizer->initialize();
	RSG::light_storage = RSG::rasterizer->get_light_storage();
	RSG::material_storage = RSG::rasterizer->get_material_storage();
	RSG::mesh_storage = RSG::rasterizer->get_mesh_storage();
	RSG::particles_storage = RSG::rasterizer->get_particles_storage();
	RSG::texture_storage = RSG::rasterizer->get_texture_storage();
	RSG::gi = RSG::rasterizer->get_gi();
	RSG::fog = RSG::rasterizer->get_fog();
	RSG::canvas_render = RSG::rasterizer->get_canvas();
	sr->set_scene_render(RSG::rasterizer->get_scene());
	renderer_resources_active.store(true, std::memory_order_release);
	maximum_viewport_size = RSG::utilities->get_maximum_viewport_size();
#ifdef RD_ENABLED
	adapter_type = RSG::utilities->get_video_adapter_type();
#endif
}

void RenderingServerDefault::_finish() {
	// Cancel unsignaled GPU and shutdown-associated callbacks before freeing resources.
	{
		MutexLock lock(frame_callback_mutex);
		pending_frame_callbacks.clear();
		awaiting_gpu_callbacks.clear();
		main_frame_callbacks.clear();
	}
	renderer_resources_active.store(false, std::memory_order_release);
	{
		MutexLock lock(rendering_info_mutex);
		for (int i = 0; i < RSE::RENDERING_INFO_MAX; i++) {
			rendering_info_snapshot[i] = 0;
		}
		rendering_info_available = false;
		gpu_memory_snapshot = RenderingServerTypes::GpuMemorySnapshot();
		published_frame_setup_time = 0;
	}
	{
		MutexLock lock(profile_snapshot_mutex);
		profile_snapshot = RenderingServerTypes::RenderFrameProfileSnapshot();
	}
	if (test_cube.is_valid()) {
		free_rid(test_cube);
	}

	RSG::canvas->finalize();
	memdelete(RSG::canvas);
	RSG::rasterizer->finalize();
	memdelete(RSG::viewport);
	memdelete(RSG::rasterizer);
	memdelete(RSG::scene);
	memdelete(RSG::camera_attributes);
}

void RenderingServerDefault::_finalize_main_rendering_device() {
#ifdef RD_ENABLED
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd != nullptr && rd->is_main_rendering_device() && !rd->is_finalized()) {
		rd->finalize();
	}
#endif
}

void RenderingServerDefault::_execute_sync_request(SyncRenderRequest *p_request) {
	p_request->callable.call();
	p_request->completed.post();
}

Error RenderingServerDefault::call_on_render_thread_sync(const Callable &p_callable) {
	if (!p_callable.is_valid()) {
		return ERR_INVALID_PARAMETER;
	}
	SyncRenderRequest *request = nullptr;
	{
		MutexLock lock(command_admission_mutex);
		if (command_admission != CommandAdmissionState::RUNNING) {
			return ERR_UNAVAILABLE;
		}
		if (Thread::get_caller_id() == server_thread) {
			// Preserve FIFO ordering of commands already submitted from other threads.
			lock.temp_unlock();
			command_queue.flush_if_pending();
			p_callable.call();
			return OK;
		}
		request = memnew(SyncRenderRequest);
		request->callable = p_callable;
		// Queue admission and shutdown use one mutex; accepted requests precede teardown.
		command_queue.push(this, &RenderingServerDefault::_execute_sync_request, request);
	}
	request->completed.wait();
	memdelete(request);
	return OK;
}

void RenderingServerDefault::init() {
	if (create_thread) {
		print_verbose("RenderingServerWrapMT: Starting render thread");
		DisplayServer::get_singleton()->release_rendering_thread();
		WorkerThreadPool::TaskID tid = WorkerThreadPool::get_singleton()->add_task(callable_mp(this, &RenderingServerDefault::_thread_loop), true, "Rendering Server pump task", true);
		command_queue.set_pump_task_id(tid);
		command_queue.push(this, &RenderingServerDefault::_assign_mt_ids, tid);
		command_queue.push_and_sync(this, &RenderingServerDefault::_init);
		DEV_ASSERT(server_task_id == tid);
	} else {
		server_thread = Thread::MAIN_ID;
		_init();
	}
	{
		MutexLock lock(command_admission_mutex);
		command_admission = CommandAdmissionState::RUNNING;
	}
}

void RenderingServerDefault::finish() {
	{
		MutexLock lock(command_admission_mutex);
		if (command_admission != CommandAdmissionState::RUNNING) {
			return;
		}
		command_admission = CommandAdmissionState::DRAINING;
		// Resource initializers may run on import workers. They must return
		// before destroying renderer storage and the main RenderingDevice.
		while (pending_resource_operations != 0) {
			resource_operations_drained.wait(lock);
		}
	}
	if (create_thread) {
		command_queue.push(this, &RenderingServerDefault::_finish);
		command_queue.push(this, &RenderingServerDefault::_finalize_main_rendering_device);
		command_queue.push(this, &RenderingServerDefault::_thread_exit);
		if (server_task_id != WorkerThreadPool::INVALID_TASK_ID) {
			WorkerThreadPool::get_singleton()->wait_for_task_completion(server_task_id);
			server_task_id = WorkerThreadPool::INVALID_TASK_ID;
		}
		server_thread = Thread::MAIN_ID;
	} else {
		_finish();
		_finalize_main_rendering_device();
	}
	{
		MutexLock lock(command_admission_mutex);
		command_admission = CommandAdmissionState::STOPPED;
	}
}

/* STATUS INFORMATION */

uint64_t RenderingServerDefault::get_rendering_info(RSE::RenderingInfo p_info) {
	ERR_FAIL_INDEX_V(p_info, RSE::RENDERING_INFO_MAX, 0);
	if (renderer_resources_active.load(std::memory_order_acquire) && Thread::get_caller_id() == server_thread && RSG::utilities != nullptr) {
		return _read_rendering_info(p_info);
	}
	MutexLock lock(rendering_info_mutex);
	return rendering_info_available ? rendering_info_snapshot[p_info] : 0;
}

uint64_t RenderingServerDefault::_read_rendering_info(RSE::RenderingInfo p_info) {
	if (p_info == RSE::RENDERING_INFO_TOTAL_OBJECTS_IN_FRAME) {
		return RSG::viewport->get_total_objects_drawn();
	} else if (p_info == RSE::RENDERING_INFO_TOTAL_PRIMITIVES_IN_FRAME) {
		return RSG::viewport->get_total_primitives_drawn();
	} else if (p_info == RSE::RENDERING_INFO_TOTAL_DRAW_CALLS_IN_FRAME) {
		return RSG::viewport->get_total_draw_calls_used();
	} else if (p_info == RSE::RENDERING_INFO_PIPELINE_COMPILATIONS_CANVAS) {
		return RSG::canvas_render->get_pipeline_compilations(RSE::PIPELINE_SOURCE_CANVAS);
	} else if (p_info == RSE::RENDERING_INFO_PIPELINE_COMPILATIONS_MESH) {
		return RSG::canvas_render->get_pipeline_compilations(RSE::PIPELINE_SOURCE_MESH) + RSG::scene->get_pipeline_compilations(RSE::PIPELINE_SOURCE_MESH);
	} else if (p_info == RSE::RENDERING_INFO_PIPELINE_COMPILATIONS_SURFACE) {
		return RSG::scene->get_pipeline_compilations(RSE::PIPELINE_SOURCE_SURFACE);
	} else if (p_info == RSE::RENDERING_INFO_PIPELINE_COMPILATIONS_DRAW) {
		return RSG::canvas_render->get_pipeline_compilations(RSE::PIPELINE_SOURCE_DRAW) + RSG::scene->get_pipeline_compilations(RSE::PIPELINE_SOURCE_DRAW);
	} else if (p_info == RSE::RENDERING_INFO_PIPELINE_COMPILATIONS_SPECIALIZATION) {
		return RSG::canvas_render->get_pipeline_compilations(RSE::PIPELINE_SOURCE_SPECIALIZATION) + RSG::scene->get_pipeline_compilations(RSE::PIPELINE_SOURCE_SPECIALIZATION);
	}
	return RSG::utilities->get_rendering_info(p_info);
}

#ifdef RD_ENABLED
RenderingDeviceEnums::DeviceType RenderingServerDefault::get_video_adapter_type() const {
	return adapter_type;
}
#endif // RD_ENABLED

void RenderingServerDefault::_set_frame_profiling_enabled(bool p_enable) {
	RSG::utilities->capturing_timestamps = p_enable;
}

void RenderingServerDefault::set_frame_profiling_enabled(bool p_enable) {
	call_on_render_thread(callable_mp(this, &RenderingServerDefault::_set_frame_profiling_enabled).bind(p_enable));
}

RenderingServerTypes::RenderFrameProfileSnapshot RenderingServerDefault::get_frame_profile_snapshot() {
	MutexLock lock(profile_snapshot_mutex);
	return profile_snapshot;
}

uint64_t RenderingServerDefault::get_frame_profile_frame() {
	return get_frame_profile_snapshot().frame_id;
}

Vector<RenderingServerTypes::FrameProfileArea> RenderingServerDefault::get_frame_profile() {
	return get_frame_profile_snapshot().areas;
}

/* TESTING */

void RenderingServerDefault::_create_native_texture_on_render_thread(int64_t p_request_address) {
	NativeTextureCreateRequest *request = reinterpret_cast<NativeTextureCreateRequest *>(uintptr_t(uint64_t(p_request_address)));
	request->result = RSG::texture_storage->texture_create_from_native_handle(request->type, request->format,
			request->handle, request->width, request->height, request->depth, request->layers, request->layered_type);
}

void RenderingServerDefault::_get_default_clear_color_sync(int64_t p_result_address) {
	Color *result = reinterpret_cast<Color *>(uintptr_t(uint64_t(p_result_address)));
	*result = RSG::texture_storage->get_default_clear_color();
}

Color RenderingServerDefault::get_default_clear_color() {
	Color result;
	if (!renderer_resources_active.load(std::memory_order_acquire)) {
		return result;
	}
	if (Thread::get_caller_id() == server_thread) {
		command_queue.flush_if_pending();
		_get_default_clear_color_sync(int64_t(uintptr_t(&result)));
	} else {
		call_on_render_thread_sync(callable_mp(this, &RenderingServerDefault::_get_default_clear_color_sync).bind(int64_t(uintptr_t(&result))));
	}
	return result;
}

void RenderingServerDefault::_set_default_clear_color_on_render_thread(const Color &p_color) {
	RSG::texture_storage->set_default_clear_color(p_color);
}

void RenderingServerDefault::set_default_clear_color(const Color &p_color) {
	call_on_render_thread(callable_mp(this, &RenderingServerDefault::_set_default_clear_color_on_render_thread).bind(p_color));
}

#ifndef DISABLE_DEPRECATED
bool RenderingServerDefault::has_feature(RSE::Features p_feature) const {
	return false;
}
#endif

void RenderingServerDefault::_sdfgi_set_debug_probe_select_on_render_thread(const Vector3 &p_position, const Vector3 &p_dir) {
	RSG::scene->sdfgi_set_debug_probe_select(p_position, p_dir);
}

void RenderingServerDefault::sdfgi_set_debug_probe_select(const Vector3 &p_position, const Vector3 &p_dir) {
	call_on_render_thread(callable_mp(this, &RenderingServerDefault::_sdfgi_set_debug_probe_select_on_render_thread).bind(p_position, p_dir));
}

void RenderingServerDefault::_set_print_gpu_profile(bool p_enable) {
	RSG::utilities->capturing_timestamps = p_enable;
	print_gpu_profile = p_enable;
}

void RenderingServerDefault::set_print_gpu_profile(bool p_enable) {
	call_on_render_thread(callable_mp(this, &RenderingServerDefault::_set_print_gpu_profile).bind(p_enable));
}

RID RenderingServerDefault::_get_test_cube_on_render_thread() {
	if (!test_cube.is_valid()) {
		test_cube = _make_test_cube();
	}
	return test_cube;
}

void RenderingServerDefault::_get_test_cube_sync(int64_t p_result_address) {
	RID *result = reinterpret_cast<RID *>(uintptr_t(uint64_t(p_result_address)));
	*result = _get_test_cube_on_render_thread();
}

RID RenderingServerDefault::get_test_cube() {
	if (!renderer_resources_active.load(std::memory_order_acquire)) {
		return RID();
	}
	if (Thread::get_caller_id() == server_thread) {
		command_queue.flush_if_pending();
		return _get_test_cube_on_render_thread();
	}
	RID result;
	call_on_render_thread_sync(callable_mp(this, &RenderingServerDefault::_get_test_cube_sync).bind(int64_t(uintptr_t(&result))));
	return result;
}

void RenderingServerDefault::_query_os_feature_sync(int64_t p_query_address) {
	OsFeatureQuery *query = reinterpret_cast<OsFeatureQuery *>(uintptr_t(uint64_t(p_query_address)));
	query->supported = RSG::utilities != nullptr && RSG::utilities->has_os_feature(query->feature);
}

bool RenderingServerDefault::has_os_feature(const String &p_feature) const {
	if (!renderer_resources_active.load(std::memory_order_acquire)) {
		return false;
	}
	OsFeatureQuery query;
	query.feature = p_feature;
	RenderingServerDefault *server = const_cast<RenderingServerDefault *>(this);
	if (Thread::get_caller_id() == server_thread) {
		server->command_queue.flush_if_pending();
		server->_query_os_feature_sync(int64_t(uintptr_t(&query)));
	} else {
		server->call_on_render_thread_sync(callable_mp(server, &RenderingServerDefault::_query_os_feature_sync).bind(int64_t(uintptr_t(&query))));
	}
	return query.supported;
}

void RenderingServerDefault::_set_debug_generate_wireframes_on_render_thread(bool p_enable) {
	RSG::utilities->set_debug_generate_wireframes(p_enable);
}

void RenderingServerDefault::set_debug_generate_wireframes(bool p_generate) {
	call_on_render_thread(callable_mp(this, &RenderingServerDefault::_set_debug_generate_wireframes_on_render_thread).bind(p_generate));
}

bool RenderingServerDefault::is_low_end() const {
	return RendererCompositor::is_low_end();
}

Size2i RenderingServerDefault::get_maximum_viewport_size() const {
	return maximum_viewport_size;
}

void RenderingServerDefault::_assign_mt_ids(WorkerThreadPool::TaskID p_pump_task_id) {
	server_thread = Thread::get_caller_id();
	server_task_id = p_pump_task_id;

#ifdef RD_ENABLED
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd) {
		// This is needed because the main RD is created on the main thread.
		rd->make_current();
	}
#endif // RD_ENABLED
}

void RenderingServerDefault::_thread_exit() {
	exit = true;
}

void RenderingServerDefault::_thread_loop() {
	DisplayServer::get_singleton()->gl_window_make_current(DisplayServerEnums::MAIN_WINDOW_ID); // Move GL to this thread.

	while (!exit) {
		WorkerThreadPool::get_singleton()->yield();
		command_queue.flush_all();
	}

	DisplayServer::get_singleton()->release_rendering_thread();
}

/* INTERPOLATION */

void RenderingServerDefault::_set_physics_interpolation_enabled(bool p_enabled) {
	RSG::canvas->set_physics_interpolation_enabled(p_enabled);
	RSG::scene->set_physics_interpolation_enabled(p_enabled);
}

void RenderingServerDefault::set_physics_interpolation_enabled(bool p_enabled) {
	ResourceOperationGuard resource_guard(this);
	if (!resource_guard) {
		return;
	}
	if (create_thread && Thread::get_caller_id() != server_thread) {
		command_queue.push(this, &RenderingServerDefault::_set_physics_interpolation_enabled, p_enabled);
	} else {
		if (create_thread) {
			command_queue.flush_if_pending();
		}
		_set_physics_interpolation_enabled(p_enabled);
	}
}

/* EVENT QUEUING */

void RenderingServerDefault::sync() {
	if (create_thread) {
		command_queue.sync();
	} else {
		command_queue.flush_all(); // Flush all pending from other threads.
	}
}

void RenderingServerDefault::draw(bool p_present, double frame_step) {
	ERR_FAIL_COND_MSG(!Thread::is_main_thread(), "Manually triggering the draw function from the RenderingServer can only be done on the main thread. Call this function from the main thread or use call_deferred().");
	const uint64_t frame_id = next_frame_token.fetch_add(1, std::memory_order_relaxed) + 1;
	// Before-draw callbacks execute on the main thread, with their frame token.
	List<FrameStageCallback> before_draw;
	{
		MutexLock lock(frame_callback_mutex);
		for (List<FrameStageCallback>::Element *E = pending_frame_callbacks.front(); E;) {
			List<FrameStageCallback>::Element *next = E->next();
			if (E->get().stage == RenderingServerTypes::RenderFrameCallbackStage::BEFORE_DRAW) {
				FrameStageCallback callback = E->get();
				callback.token.frame_id = frame_id;
				before_draw.push_back(callback);
				pending_frame_callbacks.erase(E);
			}
			E = next;
		}
	}
	for (const FrameStageCallback &callback : before_draw) {
		if (callback.callback.is_valid()) {
			callback.callback.call(frame_id);
		}
	}
	// Sample before frame_pre_draw so notifications from callbacks stay pending.
	const uint64_t submitted_generation = change_generation.load(std::memory_order_acquire);
	RS::get_singleton()->emit_signal(SNAME("frame_pre_draw"));
	// The generation is acknowledged only at the draw submission boundary.
	last_submitted_generation.store(submitted_generation, std::memory_order_release);
	if (create_thread) {
		command_queue.push(this, &RenderingServerDefault::_draw, p_present, frame_step, frame_id);
	} else {
		_draw(p_present, frame_step, frame_id);
	}
}

void RenderingServerDefault::_tick() {
	RSG::canvas->tick();
	RSG::scene->tick();
}

void RenderingServerDefault::tick() {
	ResourceOperationGuard resource_guard(this);
	if (!resource_guard) {
		return;
	}
	if (create_thread && Thread::get_caller_id() != server_thread) {
		command_queue.push(this, &RenderingServerDefault::_tick);
	} else {
		if (create_thread) {
			command_queue.flush_if_pending();
		}
		_tick();
	}
}

void RenderingServerDefault::_pre_draw(bool p_will_draw) {
	RSG::scene->pre_draw(p_will_draw);
}

void RenderingServerDefault::pre_draw(bool p_will_draw) {
	ResourceOperationGuard resource_guard(this);
	if (!resource_guard) {
		return;
	}
	if (create_thread && Thread::get_caller_id() != server_thread) {
		command_queue.push(this, &RenderingServerDefault::_pre_draw, p_will_draw);
	} else {
		if (create_thread) {
			command_queue.flush_if_pending();
		}
		_pre_draw(p_will_draw);
	}
}

void RenderingServerDefault::_call_on_render_thread(const Callable &p_callable) {
	p_callable.call();
}

RenderingServerDefault::RenderingServerDefault(bool p_create_thread) {
	RenderingServer::init();

	create_thread = p_create_thread;
}

RenderingServerDefault::~RenderingServerDefault() {
}
