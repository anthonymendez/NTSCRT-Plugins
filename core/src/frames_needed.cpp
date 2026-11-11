// frames_needed.cpp — ntscrt_source_frames_needed: which source frames the
// temporal stages can fetch for one render (hosts declare them up front).
#include <ntscrt/ntscrt.h>

#include <algorithm>

#include <nlohmann/json.hpp>

#include "../stages/digital/frames_needed.h"
#include "../stages/howlaround/howl_engine.h"
#include "../stages/howlaround/howl_math.h"

using nlohmann::json;

extern "C" ntscrt_status ntscrt_source_frames_needed(const ntscrt_render_args *args, int64_t *first) {
    if (!args || !first) return NTSCRT_ERR_INVALID_ARG;
    const int64_t n = args->frame_index;
    *first = n;
    if (n <= 0) return NTSCRT_OK;
    try {
        const double fps = args->fps > 0 ? args->fps : 30.0;
        if (args->feedback_json && *args->feedback_json) {
            const json fb = json::parse(args->feedback_json, nullptr, false);
            if (fb.is_object()) {
                auto e = fb.find("enabled");
                const bool on = e == fb.end() || !e->is_boolean() || e->get<bool>();  // Howlaround::active
                if (on) {
                    // Screen Loop on: Digital video doesn't run (render_once). The camera reads
                    // the source from its window start (Howlaround::camera / Engine::camera).
                    const ntscrt::howl::Feedback f = ntscrt::howl::parse_feedback(fb);
                    if (f.enabled && (f.settings.tv_in_view() || f.settings.counter())) {
                        int64_t window = ntscrt::howl::kDefaultWindow;
                        if (auto w = fb.find("window"); w != fb.end() && w->is_number_integer()) window = w->get<int64_t>();
                        *first = ntscrt::howl::anchor_for(n, window);
                    }
                    return NTSCRT_OK;
                }
            }
        }
        const json look = json::parse(args->look_json ? args->look_json : "{}", nullptr, false);
        if (!look.is_object()) return NTSCRT_OK;
        *first = std::clamp<int64_t>(ntscrt::digital::first_source_frame(look, fps, n, args->seed), 0, n);
        return NTSCRT_OK;
    } catch (...) {
        *first = n;
        return NTSCRT_ERR_INTERNAL;
    }
}
