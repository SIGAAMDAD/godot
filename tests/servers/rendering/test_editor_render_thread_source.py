#!/usr/bin/env python3
"""Focused source-level checks for the editor render-thread invariants."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

def source(path):
    return (ROOT / path).read_text(encoding="utf-8")

def verify():
    rd = source("servers/rendering/rendering_device.cpp")
    rs = source("servers/rendering/rendering_server_default.cpp")
    rs_h = source("servers/rendering/rendering_server_default.h")
    win = source("platform/windows/display_server_windows.cpp")
    bridge = source("servers/rendering/renderer_rd/forward_clustered/terrain3d_forward_plus_bridge.cpp")
    dbg = source("servers/debugger/servers_debugger.cpp")
    main = source("main/main.cpp")
    assert "LifecycleState::FINALIZED" in rd
    assert rd.index("driver->fence_wait(frames[p_frame].fence)") < rd.index("last_gpu_completed_frame.compare_exchange_weak")
    assert "get_last_completed_gpu_frame" in rs
    assert rs.index("_finish);", rs.index("void RenderingServerDefault::finish()")) < rs.index("_finalize_main_rendering_device);", rs.index("void RenderingServerDefault::finish()")) < rs.index("_thread_exit);", rs.index("void RenderingServerDefault::finish()"))
    assert "CommandAdmissionState::DRAINING" in rs
    assert "request->completed.wait()" in rs
    assert "std::atomic<uint64_t> change_generation" in rs_h
    for query in ("texture_debug_usage", "mesh_debug_usage"):
        start = rs_h.index(f"virtual void {query}(")
        body = rs_h[start : rs_h.index("\n\t}", start)]
        assert "ResourceOperationGuard resource_guard(this)" in body
        assert "command_queue.push_and_sync" in body
    shutdown = rs[rs.index("void RenderingServerDefault::_finish()") : rs.index("void RenderingServerDefault::_finalize_main_rendering_device()")]
    assert "rendering_info_available = false" in shutdown
    assert "gpu_memory_snapshot = RenderingServerTypes::GpuMemorySnapshot()" in shutdown
    assert "std::atomic<uint64_t> last_submitted_generation" in rs_h
    assert "last_submitted_generation.store(submitted_generation, std::memory_order_release)" in rs
    assert "get_frame_profile_snapshot()" in dbg
    assert "_run_gpu_window_op(WindowGpuOpKind::RELEASE" in win
    assert win.index("temp_relock();", win.index("WindowGpuOpKind::RELEASE, p_window")) < win.index("ERR_FAIL_COND_MSG(err != OK", win.index("WindowGpuOpKind::RELEASE, p_window"))
    assert "call_on_render_thread_sync" in bridge
    assert "request->completed.wait()" in rs
    assert "render_completed_frame_id" in main
    assert "GPU completion and input-to-display latency are unsupported" in main
    assert "--editor-render-thread-experimental" in main
    print("Source invariant checks PASS")

if __name__ == "__main__":
    verify()
