// SPDX-License-Identifier: GPL-2.0-or-later
// Android owns the SurfaceView. Keep a reference for the entire Vulkan surface
// lifetime; neither SDL nor an external presentation process is involved.
#include <android/native_window.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include "../bbgpu.h"
#include "sdl_window.h"

namespace {
std::mutex surface_mutex;
ANativeWindow* activity_window{};
std::atomic<u32> display_hz{60};

ANativeWindow* AcquireWindow() {
    std::lock_guard lock(surface_mutex);
    if (activity_window) ANativeWindow_acquire(activity_window);
    return activity_window;
}
}

extern "C" void bbgpu_android_set_window(ANativeWindow* window) {
    std::lock_guard lock(surface_mutex);
    if (window == activity_window) return;
    if (window) ANativeWindow_acquire(window);
    auto* old = activity_window;
    activity_window = window;
    if (old) ANativeWindow_release(old);
}

extern "C" void bbgpu_android_set_refresh_rate(float hz) {
    if (std::isfinite(hz) && hz >= 24.0f && hz <= 360.0f)
        display_hz.store(u32(std::lround(hz)), std::memory_order_relaxed);
}

namespace BbAndroid {
u32 DisplayRefreshHz() { return display_hz.load(std::memory_order_relaxed); }
}

namespace Frontend {
WindowSDL::WindowSDL(s32, s32, const char* title) : width{0}, height{0}, base_title{title} {
    auto* native = AcquireWindow();
    if (!native) throw std::runtime_error("Android game surface is unavailable");
    window_info.type = WindowSystemType::Android;
    window_info.render_surface = native;
    width = std::max(ANativeWindow_getWidth(native), 0);
    height = std::max(ANativeWindow_getHeight(native), 0);
    if (!width || !height) {
        ANativeWindow_release(native);
        window_info.render_surface = nullptr;
        throw std::runtime_error("Android game surface has no drawable extent");
    }
    std::printf("GPU: Android native surface %dx%d; Vulkan swapchain presentation\n",
                GetWidth(), GetHeight());
}

WindowSDL::~WindowSDL() {
    if (auto* native = static_cast<ANativeWindow*>(window_info.render_surface))
        ANativeWindow_release(native);
}

bool WindowSDL::PollEvents() {
    auto* native = static_cast<ANativeWindow*>(window_info.render_surface);
    {
        std::lock_guard lock(surface_mutex);
        // A destroyed/replaced Surface requires a fresh runtime session. Never
        // race Vulkan against a new native window belonging to another Activity.
        if (activity_window != native) {
            width = height = 0;
            is_open = false;
            std::fputs("GPU: Android surface ended; closing runtime session\n", stderr);
            return false;
        }
    }
    width = std::max(ANativeWindow_getWidth(native), 0);
    height = std::max(ANativeWindow_getHeight(native), 0);
    return is_open;
}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    std::lock_guard lock(text_mutex);
    text = initial;
    text_prompt = prompt;
    // Android's launcher owns keyboard UI. Until its IME bridge is installed,
    // report cancellation instead of leaving the game's modal wait unresolved.
    text_state = 2;
    std::fputs("GPU: native text dialog cancelled (Android IME bridge unavailable)\n", stderr);
}
int WindowSDL::PollTextInput(std::string& value) {
    std::lock_guard lock(text_mutex);
    value = text;
    return text_state;
}
void WindowSDL::UpdateTextTitle() {}
}
