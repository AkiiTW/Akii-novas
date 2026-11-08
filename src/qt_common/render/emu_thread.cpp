// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <memory>
#include <mutex>

#include <qdebug.h>
#include "core/core.h"
#include "core/cpu_manager.h"
#include "emu_thread.h"
#include "qt_common/qt_common.h"
#include "video_core/gpu.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"

namespace {
// Pipeline builds report every finished pipeline, from several worker threads, and each report
// was a queued signal the GUI thread had to lay out and repaint. With tens of thousands of cached
// pipelines the GUI fell behind its event queue, so a click or key press during the load went
// unanswered and Windows offered to end the program. Forward a stage change, the last pipeline and
// otherwise at most one report every 50 ms.
class ProgressThrottle {
public:
    bool ShouldEmit(VideoCore::LoadCallbackStage stage, std::size_t value, std::size_t total) {
        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lock{mutex};
        if (stage == last_stage && value != total && now - last_emit < std::chrono::milliseconds{50}) {
            return false;
        }
        last_stage = stage;
        last_emit = now;
        return true;
    }

private:
    std::mutex mutex;
    VideoCore::LoadCallbackStage last_stage{VideoCore::LoadCallbackStage::Complete};
    std::chrono::steady_clock::time_point last_emit{};
};
} // Anonymous namespace

EmuThread::EmuThread() {}

EmuThread::~EmuThread() = default;

void EmuThread::run() {
    Common::SetCurrentThreadName("EmuControlThread");

    auto& gpu = QtCommon::system->GPU();
    auto stop_token = m_stop_source.get_token();

    QtCommon::system->RegisterHostThread();

    // Main process has been loaded. Make the context current to this thread and begin GPU and CPU
    // execution.
    gpu.ObtainContext();

    emit LoadProgress(VideoCore::LoadCallbackStage::Prepare, 0, 0);
    if (Settings::values.use_disk_shader_cache.GetValue()) {
        QtCommon::system->Renderer().ReadRasterizer()->LoadDiskResources(
            QtCommon::system->GetApplicationProcessProgramID(), stop_token,
            [this, throttle = std::make_shared<ProgressThrottle>()](
                VideoCore::LoadCallbackStage stage, std::size_t value, std::size_t total) {
                if (throttle->ShouldEmit(stage, value, total)) {
                    emit LoadProgress(stage, value, total);
                }
            });
    }
    emit LoadProgress(VideoCore::LoadCallbackStage::Complete, 0, 0);

    gpu.ReleaseContext();
    gpu.Start();

    QtCommon::system->GetCpuManager().OnGpuReady();

    if (QtCommon::system->DebuggerEnabled()) {
        QtCommon::system->InitializeDebugger();
    }

    while (!stop_token.stop_requested()) {
        std::unique_lock lk{m_should_run_mutex};
        if (m_should_run) {
            QtCommon::system->Run();
            m_stopped.Reset();

            m_should_run_cv.wait(lk, stop_token, [&] {
                return !m_should_run || m_pending_shader_cache_title.has_value();
            });

            if (m_should_run && m_pending_shader_cache_title.has_value()) {
                const u64 program_id = *m_pending_shader_cache_title;
                m_pending_shader_cache_title.reset();

                lk.unlock();
                this->ReloadDiskShaderCache(program_id);
                lk.lock();
            }
        } else {
            QtCommon::system->Pause();
            m_stopped.Set();

            EmulationPaused(lk);
            m_should_run_cv.wait(lk, stop_token, [&] { return m_should_run; });
            EmulationResumed(lk);
        }
    }

    // Shutdown the main emulated process
    QtCommon::system->DetachDebugger();
    QtCommon::system->ShutdownMainProcess();
}

void EmuThread::ReloadDiskShaderCache(u64 program_id) {
    if (!Settings::values.use_disk_shader_cache.GetValue()) {
        return;
    }

    LOG_INFO(Frontend, "Reloading disk shader cache for {:016X}", program_id);

    auto& system = *QtCommon::system;
    auto& gpu = system.GPU();

    system.Pause();
    gpu.WaitForIdle();
    gpu.ObtainContext();

    emit ShaderCacheReloadStarted();
    emit LoadProgress(VideoCore::LoadCallbackStage::Prepare, 0, 0);
    system.Renderer().ReadRasterizer()->LoadDiskResources(program_id, m_stop_source.get_token(),
        [this, throttle = std::make_shared<ProgressThrottle>()](
            VideoCore::LoadCallbackStage stage, std::size_t value, std::size_t total) {
            if (throttle->ShouldEmit(stage, value, total)) {
                emit LoadProgress(stage, value, total);
            }
        });
    emit LoadProgress(VideoCore::LoadCallbackStage::Complete, 0, 0);
    emit ShaderCacheReloadFinished();

    gpu.ReleaseContext();
}

// Unlock while emitting signals so that the main thread can
// continue pumping events.

void EmuThread::EmulationPaused(std::unique_lock<std::mutex>& lk) {
    lk.unlock();
    emit DebugModeEntered();
    lk.lock();
}

void EmuThread::EmulationResumed(std::unique_lock<std::mutex>& lk) {
    lk.unlock();
    emit DebugModeLeft();
    lk.lock();
}
