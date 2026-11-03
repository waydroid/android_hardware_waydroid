/*
 * Copyright (C) 2026 The Waydroid Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Display configs, one per refresh rate of the host's monitors.
//
// SurfaceFlinger paces every Android display, virtual ones included, from the
// primary display's vsync period. The primary display offers a config for each
// refresh rate a monitor has had since the composer started, and Android picks
// one like it would on a phone with a variable refresh rate (DisplayModeDirector,
// then setActiveConfig), within the min_refresh_rate and peak_refresh_rate
// settings.
//
// The list only grows. A rate not seen before is added with a hotplug of the
// primary display, which makes HWC2On1Adapter read the configs again and
// SurfaceFlinger recreate the display; a switch between known rates is a mode
// change, which also leaves the virtual displays alone.

#include "wayland-hwc.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <vector>

#include <log/log.h>

namespace {

constexpr int32_t kMinRefresh = 1000;     // 1 Hz
constexpr int32_t kMaxRefresh = 1000000;  // 1000 Hz
constexpr int32_t kFallbackRefresh = 60000;

struct output_state {
    uint32_t name = 0;
    int32_t refresh = 0;  // current mode, after the last wl_output.done
    int32_t pending = 0;
};

std::mutex mutex;
std::map<struct wl_output *, output_state> outputs;
// Refresh rates in mHz, fastest first; the config index is the position
std::vector<int32_t> configs;

bool valid(int32_t refresh) {
    return refresh >= kMinRefresh && refresh <= kMaxRefresh;
}

// Adds the outputs' rates; returns whether the list changed.
bool add_rates_locked(struct display *display) {
    std::vector<int32_t> rates = configs;
    for (const auto &[output, state] : outputs)
        if (valid(state.refresh) && std::find(rates.begin(), rates.end(), state.refresh) == rates.end())
            rates.push_back(state.refresh);
    if (rates.empty())
        rates.push_back(kFallbackRefresh);
    if (rates.size() == configs.size())
        return false;
    std::sort(rates.begin(), rates.end(), std::greater<int32_t>());
    int32_t active = display->refresh.load();
    configs = std::move(rates);
    if (std::find(configs.begin(), configs.end(), active) == configs.end())
        display->refresh = configs.front();
    for (int32_t rate : configs)
        ALOGI("display config %d.%03d Hz", rate / 1000, rate % 1000);
    return true;
}

void update(struct display *display) {
    bool changed;
    {
        std::scoped_lock lock(mutex);
        changed = add_rates_locked(display);
    }
    if (changed && display->procs && display->procs->invalidate) {
        display->needHotplug = true;
        display->procs->invalidate(display->procs);
    }
}

} // namespace

void
refresh_add_output(struct display *, uint32_t name, struct wl_output *output) {
    std::scoped_lock lock(mutex);
    outputs[output].name = name;
}

void
refresh_output_mode(struct display *, struct wl_output *output, int32_t refresh) {
    std::scoped_lock lock(mutex);
    if (auto it = outputs.find(output); it != outputs.end())
        it->second.pending = refresh;
}

void
refresh_output_done(struct display *display, struct wl_output *output) {
    {
        std::scoped_lock lock(mutex);
        auto it = outputs.find(output);
        if (it == outputs.end() || it->second.refresh == it->second.pending)
            return;
        it->second.refresh = it->second.pending;
    }
    update(display);
}

void
refresh_remove_global(struct display *display, uint32_t name) {
    std::scoped_lock lock(mutex);
    auto it = outputs.begin();
    while (it != outputs.end() && it->second.name != name)
        ++it;
    if (it == outputs.end())
        return;
    struct wl_output *output = it->first;
    outputs.erase(it);
    // display->output stays bound: the HDR state was created from it
    if (output != display->output) {
        if (wl_output_get_version(output) >= WL_OUTPUT_RELEASE_SINCE_VERSION)
            wl_output_release(output);
        else
            wl_output_destroy(output);
    }
}

void
refresh_start(struct display *display) {
    update(display);
}

size_t
refresh_config_count() {
    std::scoped_lock lock(mutex);
    return configs.size();
}

int32_t
refresh_config_period_ns(size_t config) {
    std::scoped_lock lock(mutex);
    int32_t refresh = config < configs.size() ? configs[config] : kFallbackRefresh;
    return int32_t(std::llround(1e12 / refresh));
}

int
refresh_active_config(const struct display *display) {
    std::scoped_lock lock(mutex);
    auto it = std::find(configs.begin(), configs.end(), display->refresh.load());
    return it == configs.end() ? 0 : int(it - configs.begin());
}

bool
refresh_set_active_config(struct display *display, size_t config) {
    std::scoped_lock lock(mutex);
    if (config >= configs.size())
        return false;
    if (display->refresh.exchange(configs[config]) != configs[config])
        ALOGI("refresh rate %d.%03d Hz", configs[config] / 1000, configs[config] % 1000);
    return true;
}

int32_t
refresh_vsync_period_ns(const struct display *display) {
    int32_t refresh = display->refresh.load();
    if (!valid(refresh))
        refresh = kFallbackRefresh;
    return int32_t(std::llround(1e12 / refresh));
}
