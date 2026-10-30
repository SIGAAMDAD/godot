/**************************************************************************/
/*  script_editor_debugger.cpp                                            */
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

#include "script_editor_debugger.h"

#include "core/config/project_settings.h"
#include "core/debugger/debugger_marshalls.h"
#include "core/debugger/remote_debugger.h"
#include "core/input/input.h"
#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/string/ustring.h"
#include "core/variant/typed_dictionary.h"
#include "core/version.h"
#include "editor/debugger/editor_debugger_plugin.h"
#include "editor/debugger/editor_expression_evaluator.h"
#include "editor/debugger/editor_performance_profiler.h"
#include "editor/debugger/editor_profiler.h"
#include "editor/debugger/editor_visual_profiler.h"
#include "editor/docks/filesystem_dock.h"
#include "editor/docks/inspector_dock.h"
#include "editor/editor_log.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/inspector/editor_property_name_processor.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/scene/3d/node_3d_editor_viewport.h"
#include "editor/scene/canvas_item_editor_plugin.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "main/performance.h"
#include "scene/3d/camera_3d.h"
#include "scene/debugger/scene_debugger_object.h"
#include "scene/gui/button.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/grid_container.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/margin_container.h"
#include "scene/gui/separator.h"
#include "scene/gui/split_container.h"
#include "scene/gui/tab_container.h"
#include "scene/gui/tree.h"
#include "servers/debugger/servers_debugger.h"
#include "servers/display/display_server.h"

using CameraOverride = EditorDebuggerNode::CameraOverride;

void ScriptEditorDebugger::_put_msg(const String &p_message, const Array &p_data, uint64_t p_thread_id) {
	ERR_FAIL_COND(p_thread_id == Thread::UNASSIGNED_ID);
	if (is_session_active()) {
		Array msg = { p_message, p_thread_id, p_data };
		Error err = peer->put_message(msg);
		ERR_FAIL_COND_MSG(err != OK, vformat("Failed to send message %d", err));
	}
}

void ScriptEditorDebugger::debug_copy() {
	String msg = reason->get_text();
	if (msg.is_empty()) {
		return;
	}
	DisplayServer::get_singleton()->clipboard_set(msg);
}

void ScriptEditorDebugger::debug_skip_breakpoints() {
	skip_breakpoints_value = !skip_breakpoints_value;
	if (skip_breakpoints_value) {
		skip_breakpoints->set_button_icon(get_editor_theme_icon(SNAME("DebugSkipBreakpointsOn")));
	} else {
		skip_breakpoints->set_button_icon(get_editor_theme_icon(SNAME("DebugSkipBreakpointsOff")));
	}

	Array msg = { skip_breakpoints_value };
	_put_msg("set_skip_breakpoints", msg, debugging_thread_id != Thread::UNASSIGNED_ID ? debugging_thread_id : Thread::MAIN_ID);
}

void ScriptEditorDebugger::debug_ignore_error_breaks() {
	ignore_error_breaks_value = !ignore_error_breaks_value;
	if (ignore_error_breaks_value) {
		ignore_error_breaks->set_button_icon(get_theme_icon(SNAME("NotificationDisabled"), SNAME("EditorIcons")));
	} else {
		ignore_error_breaks->set_button_icon(get_theme_icon(SNAME("Notification"), SNAME("EditorIcons")));
	}

	Array msg = { ignore_error_breaks_value };
	_put_msg("set_ignore_error_breaks", msg);
}

void ScriptEditorDebugger::debug_out() {
	ERR_FAIL_COND(!is_breaked());

	_put_msg("out", Array(), debugging_thread_id);
	_clear_execution();
}

void ScriptEditorDebugger::debug_next() {
	ERR_FAIL_COND(!is_breaked());

	_put_msg("next", Array(), debugging_thread_id);
	_clear_execution();
}

void ScriptEditorDebugger::debug_step() {
	ERR_FAIL_COND(!is_breaked());

	_put_msg("step", Array(), debugging_thread_id);
	_clear_execution();
}

void ScriptEditorDebugger::debug_break() {
	ERR_FAIL_COND(is_breaked());

	_put_msg("break", Array());
	_mute_audio_on_break(true);
}

void ScriptEditorDebugger::debug_continue() {
	ERR_FAIL_COND(!is_breaked());

	// Allow focus stealing only if we actually run this client for security.
	if (remote_pid && EditorNode::get_singleton()->has_child_process(remote_pid)) {
		DisplayServer::get_singleton()->enable_for_stealing_focus(remote_pid);
	}

	_clear_execution();
	_put_msg("continue", Array(), debugging_thread_id);
	_put_msg("servers:foreground", Array());
	_mute_audio_on_break(false);
}

void ScriptEditorDebugger::update_tabs() {
	if (error_count == 0 && warning_count == 0) {
		errors_tab->set_name(TTRC("Errors"));
		tabs->set_tab_icon(tabs->get_tab_idx_from_control(errors_tab), Ref<Texture2D>());
	} else {
		errors_tab->set_name(TTR("Errors") + " (" + itos(error_count + warning_count) + ")");
		if (error_count >= 1 && warning_count >= 1) {
			tabs->set_tab_icon(tabs->get_tab_idx_from_control(errors_tab), get_editor_theme_icon(SNAME("ErrorWarning")));
		} else if (error_count >= 1) {
			tabs->set_tab_icon(tabs->get_tab_idx_from_control(errors_tab), get_editor_theme_icon(SNAME("Error")));
		} else {
			tabs->set_tab_icon(tabs->get_tab_idx_from_control(errors_tab), get_editor_theme_icon(SNAME("Warning")));
		}
	}
}

void ScriptEditorDebugger::add_style() {
	tabs->add_theme_style_override(SceneStringName(panel), get_theme_stylebox(SNAME("DebuggerPanel"), EditorStringName(EditorStyles)));
}

void ScriptEditorDebugger::clear_style() {
	tabs->remove_theme_style_override(SceneStringName(panel));
}

void ScriptEditorDebugger::save_node(ObjectID p_id, const String &p_file) {
	Array msg = { p_id, p_file };
	_put_msg("scene:save_node", msg);
}

void ScriptEditorDebugger::_file_selected(const String &p_file) {
	switch (file_dialog_purpose) {
		case SAVE_MONITORS_CSV: {
			Error err;
			Ref<FileAccess> file = FileAccess::open(p_file, FileAccess::WRITE, &err);

			if (err != OK) {
				ERR_PRINT("Failed to open " + p_file);
				return;
			}
			Vector<String> line;
			line.resize(Performance::MONITOR_MAX);

			// signatures
			for (int i = 0; i < Performance::MONITOR_MAX; i++) {
				line.write[i] = Performance::get_singleton()->get_monitor_name(Performance::Monitor(i));
			}
			file->store_csv_line(line);

			// values
			Vector<List<float>::Element *> iterators;
			iterators.resize(Performance::MONITOR_MAX);
			bool continue_iteration = false;
			for (int i = 0; i < Performance::MONITOR_MAX; i++) {
				iterators.write[i] = performance_profiler->get_monitor_data(Performance::get_singleton()->get_monitor_name(Performance::Monitor(i)))->back();
				continue_iteration = continue_iteration || iterators[i];
			}
			while (continue_iteration) {
				continue_iteration = false;
				for (int i = 0; i < Performance::MONITOR_MAX; i++) {
					if (iterators[i]) {
						line.write[i] = String::num_real(iterators[i]->get());
						iterators.write[i] = iterators[i]->prev();
					} else {
						line.write[i] = "";
					}
					continue_iteration = continue_iteration || iterators[i];
				}
				file->store_csv_line(line);
			}
			file->store_string("\n");

			Vector<Vector<String>> profiler_data = profiler->get_data_as_csv();
			for (int i = 0; i < profiler_data.size(); i++) {
				file->store_csv_line(profiler_data[i]);
			}
		} break;
		case SAVE_VRAM_CSV: {
			Error err;
			Ref<FileAccess> file = FileAccess::open(p_file, FileAccess::WRITE, &err);

			if (err != OK) {
				ERR_PRINT("Failed to open " + p_file);
				return;
			}

			Vector<String> headers;
			headers.resize(vmem_tree->get_columns());
			for (int i = 0; i < vmem_tree->get_columns(); ++i) {
				headers.write[i] = vmem_tree->get_column_title(i);
			}
			file->store_csv_line(headers);

			if (vmem_tree->get_root()) {
				TreeItem *ti = vmem_tree->get_root()->get_first_child();
				while (ti) {
					Vector<String> values;
					values.resize(vmem_tree->get_columns());
					for (int i = 0; i < vmem_tree->get_columns(); ++i) {
						values.write[i] = ti->get_text(i);
					}
					file->store_csv_line(values);

					ti = ti->get_next();
				}
			}
		} break;
	}
}

void ScriptEditorDebugger::request_remote_tree() {
	_put_msg("scene:request_scene_tree", Array());
}

const SceneDebuggerTree *ScriptEditorDebugger::get_remote_tree() {
	return scene_tree;
}

void ScriptEditorDebugger::request_remote_evaluate(const String &p_expression, int p_stack_frame) {
	Array msg = { p_expression, p_stack_frame };
	_put_msg("evaluate", msg);
}

void ScriptEditorDebugger::update_remote_object(ObjectID p_obj_id, const String &p_prop, const Variant &p_value, const String &p_field) {
	Array msg = { p_obj_id, p_prop };

	Ref<Resource> res = p_value;
	if (res.is_valid() && !res->get_path().is_empty()) {
		msg.append(res->get_path());
	} else {
		msg.append(p_value);
	}

	if (p_field.is_empty()) {
		_put_msg("scene:set_object_property", msg);
	} else {
		msg.push_back(p_field);
		_put_msg("scene:set_object_property_field", msg);
	}
}

void ScriptEditorDebugger::change_canvas_item_objects(const Dictionary &p_states) {
	ERR_FAIL_COND(p_states.is_empty());
	Array msg = { p_states.duplicate() };
	_put_msg("scene:change_canvas_item_states", msg);
}

void ScriptEditorDebugger::request_remote_objects(const TypedArray<uint64_t> &p_obj_ids, bool p_update_selection) {
	ERR_FAIL_COND(p_obj_ids.is_empty());
	Array msg = { p_obj_ids.duplicate(), p_update_selection };
	_put_msg("scene:inspect_objects", msg);
}

void ScriptEditorDebugger::clear_inspector(bool p_send_msg) {
	inspector->clear_remote_inspector();
	if (p_send_msg) {
		_put_msg("scene:clear_selection", Array());
	}
}

void ScriptEditorDebugger::_remote_object_selected(ObjectID p_id) {
	emit_signal(SNAME("remote_objects_requested"), Array{ p_id });
}

void ScriptEditorDebugger::_remote_objects_edited(const String &p_prop, const TypedDictionary<uint64_t, Variant> &p_values, const String &p_field) {
	for (const KeyValue<Variant, Variant> &kv : p_values) {
		update_remote_object(ObjectID(static_cast<uint64_t>(kv.key)), p_prop, kv.value, p_field);
	}
	request_remote_objects(p_values.keys(), false);
}

void ScriptEditorDebugger::_remote_object_property_updated(ObjectID p_id, const String &p_property) {
	emit_signal(SNAME("remote_object_property_updated"), p_id, p_property);
}

void ScriptEditorDebugger::_video_mem_request() {
	_put_msg("servers:memory", Array());
}

void ScriptEditorDebugger::_video_mem_capture_start() {
	vmem_capture_id = 0;
	vmem_alloc_next_offset = 0;
	vmem_alloc_tree->clear();
	vmem_capture_status->set_text(TTR("Starting native allocation capture..."));
	_put_msg("servers:memprof_start", Array());
}

void ScriptEditorDebugger::_video_mem_capture_stop() {
	_put_msg("servers:memprof_stop", Array());
}

void ScriptEditorDebugger::_video_mem_capture_more() {
	if (vmem_capture_id != 0) {
		_put_msg("servers:memprof_records", Array{ vmem_alloc_next_offset, vmem_capture_id });
	}
}

void ScriptEditorDebugger::_video_mem_export() {
	file_dialog->set_file_mode(EditorFileDialog::FILE_MODE_SAVE_FILE);
	file_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
	file_dialog->clear_filters();
	file_dialog_purpose = SAVE_VRAM_CSV;
	file_dialog->popup_file_dialog();
}

Size2 ScriptEditorDebugger::get_minimum_size() const {
	Size2 ms = MarginContainer::get_minimum_size();
	ms.y = MAX(ms.y, 250 * EDSCALE);
	return ms;
}

void ScriptEditorDebugger::_thread_debug_enter(uint64_t p_thread_id) {
	ERR_FAIL_COND(!threads_debugged.has(p_thread_id));
	ThreadDebugged &td = threads_debugged[p_thread_id];
	_set_reason_text(td.error, MESSAGE_ERROR);
	emit_signal(SNAME("breaked"), true, td.can_debug, td.error, td.has_stackdump);
	_mute_audio_on_break(true);
	if (!td.error.is_empty() && EDITOR_GET("debugger/auto_switch_to_stack_trace")) {
		tabs->set_current_tab(0);
	}
	inspector->clear_cache(); // Take a chance to force remote objects update.
	_put_msg("get_stack_dump", Array(), p_thread_id);
}

void ScriptEditorDebugger::_select_thread(int p_index) {
	debugging_thread_id = threads->get_item_metadata(threads->get_selected());
	_thread_debug_enter(debugging_thread_id);
}

void ScriptEditorDebugger::_msg_debug_enter(uint64_t p_thread_id, const Array &p_data) {
	ERR_FAIL_COND(p_data.size() != 4);

	const Thread::ID caller_id = p_data[3];

	ThreadDebugged td;
	td.name = (caller_id == Thread::get_main_id()) ? TTR("Main Thread") : itos(caller_id);
	td.error = p_data[1];
	td.can_debug = p_data[0];
	td.has_stackdump = p_data[2];
	td.thread_id = p_thread_id;
	static uint32_t order_inc = 0;
	td.debug_order = order_inc++;

	threads_debugged.insert(p_thread_id, td);

	if (threads_debugged.size() == 1) {
		// First thread that requests debug
		debugging_thread_id = p_thread_id;
		_thread_debug_enter(p_thread_id);
		can_request_idle_draw = true;
		if (is_move_to_foreground()) {
			DisplayServer::get_singleton()->window_move_to_foreground();
		}
		profiler->set_enabled(false, false);
		visual_profiler->set_enabled(false);
	}
	_update_buttons_state();
}

void ScriptEditorDebugger::_msg_debug_exit(uint64_t p_thread_id, const Array &p_data) {
	threads_debugged.erase(p_thread_id);
	if (p_thread_id == debugging_thread_id) {
		_clear_execution();
		if (threads_debugged.is_empty()) {
			debugging_thread_id = Thread::UNASSIGNED_ID;
		} else {
			// Find next thread to debug.
			uint32_t min_order = 0xFFFFFFFF;
			uint64_t next_thread = Thread::UNASSIGNED_ID;
			for (KeyValue<uint64_t, ThreadDebugged> T : threads_debugged) {
				if (T.value.debug_order < min_order) {
					min_order = T.value.debug_order;
					next_thread = T.key;
				}
			}

			debugging_thread_id = next_thread;
		}

		if (debugging_thread_id == Thread::UNASSIGNED_ID) {
			// Nothing else to debug.
			profiler->set_enabled(true, false);
			profiler->disable_seeking();

			visual_profiler->set_enabled(true);

			_set_reason_text(TTRC("Execution resumed."), MESSAGE_SUCCESS);
			emit_signal(SNAME("breaked"), false, false, "", false);
			_mute_audio_on_break(false);

			_update_buttons_state();
		} else {
			_thread_debug_enter(debugging_thread_id);
		}
	} else {
		_update_buttons_state();
	}
}

void ScriptEditorDebugger::_msg_set_pid(uint64_t p_thread_id, const Array &p_data) {
	ERR_FAIL_COND(p_data.is_empty());
	remote_pid = p_data[0];
	// We emit the started signal after we've set the PID.
	emit_signal(SNAME("started"));
}

void ScriptEditorDebugger::_msg_scene_click_ctrl(uint64_t p_thread_id, const Array &p_data) {
	ERR_FAIL_COND(p_data.size() < 2);
	clicked_ctrl->set_text(p_data[0]);
	clicked_ctrl_type->set_text(p_data[1]);
}

void ScriptEditorDebugger::_msg_scene_scene_tree(uint64_t p_thread_id, const Array &p_data) {
	scene_tree->nodes.clear();
	scene_tree->deserialize(p_data);
	emit_signal(SNAME("remote_tree_updated"));
	_update_buttons_state();
}

void ScriptEditorDebugger::_msg_scene_inspect_objects(uint64_t p_thread_id, const Array &p_data) {
	ERR_FAIL_COND(p_data.is_empty());
	EditorDebuggerRemoteObjects *robjs = inspector->set_objects(p_data, get_current_debugger_tab());
	if (robjs && EditorDebuggerNode::get_singleton()->match_remote_selection(robjs->remote_object_ids)) {
		EditorDebuggerNode::get_singleton()->stop_waiting_inspection();
		emit_signal(SNAME("remote_objects_updated"), robjs);
	}
}

#ifndef DISABLE_DEPRECATED
void ScriptEditorDebugger::_msg_scene_inspect_object(uint64_t p_thread_id, const Array &p_data) {
	ERR_FAIL_COND(p_data.is_empty());
	// Legacy compatibility: convert single object response to new format.
	// p_data is [id, className, properties] - wrap it as first element of array for new handler.
	Array wrapped_data;
	wrapped_data.push_back(p_data);
	_msg_scene_inspect_objects(p_thread_id, wrapped_data);
}
#endif // DISABLE_DEPRECATED

void ScriptEditorDebugger::_msg_scene_debug_mute_audio(uint64_t p_thread_id, const Array &p_data) {
	ERR_FAIL_COND(p_data.is_empty());
	// This is handled by SceneDebugger, we need to ignore here to not show a warning.
}

void ScriptEditorDebugger::_msg_servers_memory_usage(uint64_t p_thread_id, const Array &p_data) {
	vmem_tree->clear();
	TreeItem *root = vmem_tree->create_item();
	ServersDebugger::ResourceUsage usage;
	usage.deserialize(p_data);

	uint64_t total = 0;

	for (const ServersDebugger::ResourceInfo &E : usage.infos) {
		TreeItem *it = vmem_tree->create_item(root);
		String type = E.type;
		int bytes = E.vram;
		it->set_text(0, E.path);
		it->set_text(1, type);
		it->set_text(2, E.format);
		it->set_text(3, String::humanize_size(bytes));
		total += bytes;

		// If it does not have a theme icon, just go up the inheritance tree until we find one.
		if (!has_theme_icon(type, EditorStringName(EditorIcons))) {
			StringName base_type = type;
			while (base_type != "Resource" && base_type != "") {
				base_type = ClassDB::get_parent_class(base_type);
				if (has_theme_icon(base_type, EditorStringName(EditorIcons))) {
					type = base_type;
					break;
				}
			}
		}

		it->set_icon(0, get_editor_theme_icon(type));
	}

	vmem_total->set_tooltip_text(TTR("Bytes:") + " " + itos(total));
	vmem_total->set_text(String::humanize_size(total));
}

void ScriptEditorDebugger::_msg_servers_memprof_status(uint64_t p_thread_id, const Array &p_data) {
	if (p_data.size() != 11) {
		return;
	}
	const int64_t id = p_data[0];
	const bool active = p_data[8];
	const bool incomplete = p_data[9];
	const bool success = p_data[10];
	if (!success) {
		vmem_capture_status->set_text(TTR("Capture rejected or already running"));
		return;
	}
	vmem_capture_id = id;
	vmem_capture_status->set_text(vformat("Capture %d: %s, live %s in %d allocations, peak %s%s",
			id, active ? TTR("recording") : TTR("stopped"),
			String::humanize_size(int64_t(p_data[2])), int64_t(p_data[4]),
			String::humanize_size(int64_t(p_data[3])), incomplete ? TTR(" (incomplete trace)") : String()));
	if (!active) {
		vmem_alloc_next_offset = 0;
		vmem_alloc_tree->clear();
		_video_mem_capture_more();
	}
}

void ScriptEditorDebugger::_msg_servers_memprof_records(uint64_t p_thread_id, const Array &p_data) {
	if (p_data.size() < 3 || int64_t(p_data[0]) != vmem_capture_id) {
		return;
	}
	const int64_t offset = p_data[1];
	const int64_t count = p_data[2];
	if (offset != vmem_alloc_next_offset || count < 0 || count > 64 || p_data.size() != 3 + count * 5) {
		return;
	}
	TreeItem *root = vmem_alloc_tree->get_root();
	if (!root) {
		root = vmem_alloc_tree->create_item();
	}
	for (int64_t i = 0; i < count; i++) {
		const int index = 3 + int(i) * 5;
		TreeItem *item = vmem_alloc_tree->create_item(root);
		item->set_text(0, itos(int64_t(p_data[index])));
		item->set_text(1, itos(int64_t(p_data[index + 1])));
		item->set_text(2, String::humanize_size(int64_t(p_data[index + 2])));
		item->set_text(3, itos(int64_t(p_data[index + 3])));
		item->set_text(4, itos(int64_t(p_data[index + 4])));
	}
	vmem_alloc_next_offset += count;
	vmem_alloc_more->set_disabled(count < 64);
}

void ScriptEditorDebugger::_msg_servers_drawn(uint64_t p_thread_id, const Array &p_data) {
	can_request_idle_draw = true;
}

void ScriptEditorDebugger::_msg_stack_dump(uint64_t p_thread_id, const Array &p_data) {
	DebuggerMarshalls::ScriptStackDump stack;
	stack.deserialize(p_data);

	stack_dump->clear();
	inspector->clear_stack_variables();
	TreeItem *r = stack_dump->create_item();

	Array stack_dump_info;

	int i = 0;
	for (List<ScriptLanguage::StackInfo>::Iterator itr = stack.frames.begin(); itr != stack.frames.end(); ++itr, ++i) {
		TreeItem *s = stack_dump->create_item(r);
		Dictionary d;
		d["frame"] = i;
		d["file"] = itr->file;
		d["function"] = itr->func;
		d["line"] = itr->line;
		stack_dump_info.push_back(d);
		s->set_metadata(0, d);

		String line = itos(i) + " - " + String(d["file"]) + ":" + itos(d["line"]) + " - at function: " + String(d["function"]);
		s->set_text(0, line);

		if (i == 0) {
			s->select(0);
		}
	}
	emit_signal(SNAME("stack_dump"), stack_dump_info);
}

void ScriptEditorDebugger::_msg_stack_frame_vars(uint64_t p_thread_id, const Array &p_data) {
	inspector->clear_stack_variables();
	ERR_FAIL_COND(p_data.size() != 1);
	emit_signal(SNAME("stack_frame_vars"), p_data[0]);
}

void ScriptEditorDebugger::_msg_stack_frame_var(uint64_t p_thread_id, const Array &p_data) {
	inspector->add_stack_variable(p_data);
	emit_signal(SNAME("stack_frame_var"), p_data);
}

void ScriptEditorDebugger::_msg_output(uint64_t p_thread_id, const Array &p_data) {
	ERR_FAIL_COND(p_data.size() != 2);

	ERR_FAIL_COND(p_data[0].get_type() != Variant::PACKED_STRING_ARRAY);
	Vector<String> output_strings = p_data[0];

	ERR_FAIL_COND(p_data[1].get_type() != Variant::PACKED_INT32_ARRAY);
	Vector<int> output_types = p_data[1];

	ERR_FAIL_COND(output_strings.size() != output_types.size());

	for (int i = 0; i < output_strings.size(); i++) {
		RemoteDebugger::MessageType type = (RemoteDebugger::MessageType)(int)(output_types[i]);
		EditorLog::MessageType msg_type;
		switch (type) {
			case RemoteDebugger::MESSAGE_TYPE_LOG: {
				msg_type = EditorLog::MSG_TYPE_STD;
			} break;
			case RemoteDebugger::MESSAGE_TYPE_LOG_RICH: {
				msg_type = EditorLog::MSG_TYPE_STD_RICH;
			} break;
			case RemoteDebugger::MESSAGE_TYPE_ERROR: {
				msg_type = EditorLog::MSG_TYPE_ERROR;
			} break;
			case RemoteDebugger::MESSAGE_TYPE_EDITOR: {
				msg_type = EditorLog::MSG_TYPE_EDITOR;
			} break;
			default: {
				WARN_PRINT("Unhandled script debugger message type: " + itos(type));
				msg_type = EditorLog::MSG_TYPE_STD;
			} break;
		}
		EditorNode::get_log()->add_message(output_strings[i], msg_type);
		emit_signal(SceneStringName(output), output_strings[i], msg_type);
	}
}

void ScriptEditorDebugger::_msg_performance_profile_frame(uint64_t p_thread_id, const Array &p_data) {
	Vector<float> frame_data;
	frame_data.resize(p_data.size());
	for (int i = 0; i < p_data.size(); i++) {
		frame_data.write[i] = p_data[i];
	}
	performance_profiler->add_profile_frame(frame_data);
}

void ScriptEditorDebugger::_msg_visual_hardware_info(uint64_t p_thread_id, const Array &p_data) {
	const String cpu_name = p_data[0];
	const String gpu_name = p_data[1];
	visual_profiler->set_hardware_info(cpu_name, gpu_name);
}

void ScriptEditorDebugger::_msg_visual_profile_frame(uint64_t p_thread_id, const Array &p_data) {
	ServersDebugger::VisualProfilerFrame frame;
	frame.deserialize(p_data);

	EditorVisualProfiler::Metric metric;
	metric.areas.resize(frame.areas.size());
	metric.frame_number = frame.frame_number;
	metric.valid = true;

	{
		EditorVisualProfiler::Metric::Area *areas_ptr = metric.areas.ptrw();
		for (int i = 0; i < frame.areas.size(); i++) {
			areas_ptr[i].name = frame.areas[i].name;
			areas_ptr[i].cpu_time = frame.areas[i].cpu_msec;
			areas_ptr[i].gpu_time = frame.areas[i].gpu_msec;
		}
	}
	visual_profiler->add_frame_metric(metric);
}

void ScriptEditorDebugger::_msg_error(uint64_t p_thread_id, const Array &p_data) {
	DebuggerMarshalls::OutputError oe;
	ERR_FAIL_COND_MSG(oe.deserialize(p_data) == false, "Failed to deserialize error message");

	// Format time.
	Array time_vals = { oe.hr, oe.min, oe.sec, oe.msec };
	bool e;
	String time = String("%d:%02d:%02d:%03d").sprintf(time_vals, &e);

	// Rest of the error data.
	bool source_is_project_file = oe.source_file.begins_with("res://");

	// Metadata to highlight error line in scripts.
	Array source_meta = { oe.source_file, oe.source_line };

	// Create error tree to display above error or warning details.
	TreeItem *r = error_tree->get_root();
	if (!r) {
		r = error_tree->create_item();
	}

	// Also provide the relevant details as tooltip to quickly check without
	// uncollapsing the tree.
	String tooltip = oe.warning ? TTRC("Warning:") : TTRC("Error:");

	TreeItem *error = error_tree->create_item(r);
	if (oe.warning) {
		error->set_meta("_is_warning", true);
	} else {
		error->set_meta("_is_error", true);
	}
	error->set_collapsed(true);

	error->set_text_overrun_behavior(0, TextServer::OVERRUN_NO_TRIMMING);
	error->set_icon(0, get_editor_theme_icon(oe.warning ? SNAME("Warning") : SNAME("Error")));
	error->set_text(0, time);
	error->set_text_alignment(0, HORIZONTAL_ALIGNMENT_LEFT);

	const Color color = get_theme_color(oe.warning ? SNAME("warning_color") : SNAME("error_color"), EditorStringName(Editor));
	error->set_custom_color(0, color);
	error->set_custom_color(1, color);

	String error_title;
	if (!oe.source_func.is_empty() && source_is_project_file) {
		// If source function is inside the project file.
		error_title += oe.source_func + ": ";
	} else if (oe.callstack.size() > 0) {
		// Otherwise, if available, use the script's stack in the error title.
		error_title = _format_frame_text(&oe.callstack[0]) + ": ";
	} else if (!oe.source_func.is_empty()) {
		// Otherwise try to use the C++ source function.
		error_title += oe.source_func + ": ";
	}
	// If we have a (custom) error message, use it as title, and add a C++ Error
	// item with the original error condition.
	error_title += oe.error_descr.is_empty() ? oe.error : oe.error_descr;
	error->set_text(1, error_title);
	error->set_autowrap_mode(1, TextServer::AUTOWRAP_WORD_SMART);
	error->set_autowrap_trim_flags(1, 0);
	tooltip += " " + error_title + "\n";

	// Find the language of the error's source file.
	String source_language_name = "C++"; // Default value is the old hard-coded one.
	const String source_file_extension = oe.source_file.get_extension();
	for (int i = 0; i < ScriptServer::get_language_count(); ++i) {
		ScriptLanguage *script_language = ScriptServer::get_language(i);
		if (source_file_extension == script_language->get_extension()) {
			source_language_name = script_language->get_name();
			break;
		}
	}

	if (!oe.error_descr.is_empty()) {
		// Add item for C++ error condition.
		TreeItem *cpp_cond = error_tree->create_item(error);
		// TRANSLATORS: %s is the name of a language, e.g. C++.
		cpp_cond->set_text(0, "<" + vformat(TTR("%s Error"), source_language_name) + ">");
		cpp_cond->set_text(1, oe.error);
		cpp_cond->set_text_alignment(0, HORIZONTAL_ALIGNMENT_LEFT);
		tooltip += vformat(TTR("%s Error:"), source_language_name) + " " + oe.error + "\n";
		if (source_is_project_file) {
			cpp_cond->set_metadata(0, source_meta);
		}
	}
	Vector<uint8_t> v;
	v.resize(100);

	// Source of the error.
	String source_txt = (source_is_project_file ? oe.source_file.get_file() : oe.source_file) + ":" + itos(oe.source_line);
	if (!oe.source_func.is_empty()) {
		source_txt += " @ " + oe.source_func;
		if (!oe.source_func.ends_with(")")) {
			source_txt += "()";
		}
	}

	TreeItem *cpp_source = error_tree->create_item(error);
	// TRANSLATORS: %s is the name of a language, e.g. C++.
	cpp_source->set_text(0, "<" + vformat(TTR("%s Source"), source_language_name) + ">");
	cpp_source->set_text(1, source_txt);
	cpp_source->set_text_alignment(0, HORIZONTAL_ALIGNMENT_LEFT);
	tooltip += vformat(TTR("%s Source:"), source_language_name) + " " + source_txt + "\n";

	// Set metadata to highlight error line in scripts.
	if (source_is_project_file) {
		error->set_metadata(0, source_meta);
		cpp_source->set_metadata(0, source_meta);
	}

	// Format stack trace.
	// stack_items_count is the number of elements to parse, with 3 items per frame
	// of the stack trace (script, method, line).
	const ScriptLanguage::StackInfo *infos = oe.callstack.ptr();
	for (unsigned int i = 0; i < (unsigned int)oe.callstack.size(); i++) {
		TreeItem *stack_trace = error_tree->create_item(error);

		Array meta = { infos[i].file, infos[i].line };
		stack_trace->set_metadata(0, meta);

		if (i == 0) {
			stack_trace->set_text(0, "<" + TTR("Stack Trace") + ">");
			stack_trace->set_text_alignment(0, HORIZONTAL_ALIGNMENT_LEFT);
			if (!source_is_project_file) {
				// Only override metadata if the source is not inside the project.
				error->set_metadata(0, meta);
			}
			tooltip += TTR("Stack Trace:") + "\n";
		}

		String frame_txt = _format_frame_text(&infos[i]);
		tooltip += frame_txt + "\n";
		stack_trace->set_text(1, frame_txt);
	}

	error->set_tooltip_text(0, tooltip);
	error->set_tooltip_text(1, tooltip);

	if (warning_count == 0 && error_count == 0) {
		expand_all_button->set_disabled(false);
		collapse_all_button->set_disabled(false);
		clear_button->set_disabled(false);
	}

	if (oe.warning) {
		warning_count++;
	} else {
		error_count++;
	}
}

void ScriptEditorDebugger::_msg_servers_function_signature(uint64_t p_thread_id, const Array &p_data) {
	// Cache a profiler signature.
	ServersDebugger::ScriptFunctionSignature sig;
	sig.deserialize(p_data);
	profiler_signature[sig.id] = sig.name;
}

void ScriptEditorDebugger::_msg_servers_profile_common(const Array &p_data, const bool p_final) {
	EditorProfiler::Metric metric;
	ServersDebugger::ServersProfilerFrame frame;
	frame.deserialize(p_data);
	metric.valid = true;
	metric.frame_number = frame.frame_number;
	metric.frame_time = frame.frame_time;
	metric.process_time = frame.process_time;
	metric.physics_time = frame.physics_time;
	metric.physics_frame_time = frame.physics_frame_time;

	if (frame.servers.size()) {
		EditorProfiler::Metric::Category frame_time;
		frame_time.signature = "category_frame_time";
		frame_time.name = "Frame Time";
		frame_time.total_time = metric.frame_time;

		EditorProfiler::Metric::Category::Item item;
		item.calls = 1;
		item.line = 0;

		item.name = "Physics Time";
		item.total = metric.physics_time;
		item.self = item.total;
		item.signature = "physics_time";

		frame_time.items.push_back(item);

		item.name = "Process Time";
		item.total = metric.process_time;
		item.self = item.total;
		item.signature = "process_time";

		frame_time.items.push_back(item);

		item.name = "Physics Frame Time";
		item.total = metric.physics_frame_time;
		item.self = item.total;
		item.signature = "physics_frame_time";

		frame_time.items.push_back(item);

		metric.categories.push_back(frame_time);
	}

	for (const ServersDebugger::ServerInfo &srv : frame.servers) {
		EditorProfiler::Metric::Category c;
		const String name = srv.name;
		c.name = EditorPropertyNameProcessor::get_singleton()->process_name(name, EditorPropertyNameProcessor::STYLE_CAPITALIZED);
		c.items.resize(srv.functions.size());
		c.total_time = 0;
		c.signature = "categ::" + name;
		int j = 0;
		for (List<ServersDebugger::ServerFunctionInfo>::ConstIterator itr = srv.functions.begin(); itr != srv.functions.end(); ++itr, ++j) {
			EditorProfiler::Metric::Category::Item item;
			item.calls = 1;
			item.line = 0;
			item.name = itr->name;
			item.self = itr->time;
			item.total = item.self;
			item.signature = "categ::" + name + "::" + item.name;
			item.name = EditorPropertyNameProcessor::get_singleton()->process_name(item.name, EditorPropertyNameProcessor::STYLE_CAPITALIZED);
			c.total_time += item.total;
			c.items.write[j] = item;
		}
		metric.categories.push_back(c);
	}

	EditorProfiler::Metric::Category funcs;
	funcs.total_time = frame.script_time;
	funcs.items.resize(frame.script_functions.size());
	funcs.name = "Script Functions";
	funcs.signature = "script_functions";
	for (int i = 0; i < frame.script_functions.size(); i++) {
		int signature = frame.script_functions[i].sig_id;
		int calls = frame.script_functions[i].call_count;
		float total = frame.script_functions[i].total_time;
		float self = frame.script_functions[i].self_time;
		float internal = frame.script_functions[i].internal_time;

		EditorProfiler::Metric::Category::Item item;
		if (profiler_signature.has(signature)) {
			item.signature = profiler_signature[signature];

			String name = profiler_signature[signature];
			Vector<String> strings = name.split("::");
			if (strings.size() == 3) {
				item.name = strings[2];
				item.script = strings[0];
				item.line = strings[1].to_int();
			} else if (strings.size() == 4) { //Built-in scripts have an :: in their name
				item.name = strings[3];
				item.script = strings[0] + "::" + strings[1];
				item.line = strings[2].to_int();
			}

		} else {
			item.name = "SigErr " + itos(signature);
		}

		item.calls = calls;
		item.self = self;
		item.total = total;
		item.internal = internal;
		funcs.items.write[i] = item;
	}

	metric.categories.push_back(funcs);

	profiler->add_frame_metric(metric, p_final);
}

void ScriptEditorDebugger::_msg_servers_profile_frame(uint64_t p_thread_id, const Array &p_data) {
	_msg_servers_profile_common(p_data, false);
}
void ScriptEditorDebugger::_msg_servers_profile_total(uint64_t p_thread_id, const Array &p_data) {
	_msg_servers_profile_common(p_data, true);
}

void ScriptEditorDebugger::_msg_request_quit(uint64_t p_thread_id, const Array &p_data) {
	emit_signal(SNAME("stop_requested"));
	_stop_and_notify();
}

void ScriptEditorDebugger::_msg_remote_objects_selected(uint64_t p_thread_id, const Array &p_data) {
	ERR_FAIL_COND(p_data.is_empty());
	EditorDebuggerNode *dbg = EditorDebuggerNode::get_singleton();
	EditorDebuggerRemoteObjects *robjs = inspector->set_objects(p_data, dbg->get_debugger_id(this));
	if (robjs) {
		dbg->stop_waiting_inspection();
		if (dbg->get_current_debugger() != this) {
			dbg->set_current_debugger(robjs->debugger_id);
		}