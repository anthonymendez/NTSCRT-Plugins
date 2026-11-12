// The same preset renders the same in Resolve and in Final Cut: for every bundled
// preset (looks, animated looks at several frames, Screen Loop presets, Digital video
// formats) and the default instance, the look and feedback built through the OpenFX
// plug-in's path (ntscrt::host on look-typed values, ofx_host_sim.h) and through the
// FxPlug plug-in's (fxplug/model Model on menu indices / RGB, as NTSCRTPlugIn.mm drives
// it) are identical, and both Preset menus list the same entries in the same order.
#include <doctest/doctest.h>

#include "ofx_host_sim.h"
#include "param_model.h"

using namespace hostsim;
namespace fx = ntscrt_fx;

namespace {
const fx::Model &fx_model() {
    static const fx::Model m = fx::Model::from_schema(json::parse(ntscrt_param_schema_json()),
                                                       load_file(std::string(NTSCRT_SOURCE_DIR) + "/fxplug/param_ids.json"),
                                                       nullptr);
    return m;
}

// FxPlug host: host values per host id, keyframes as frame -> value.
struct FxHost {
    const fx::Model &m;
    std::map<uint32_t, json> val;
    std::map<uint32_t, std::map<int64_t, json>> keys;
    explicit FxHost(const fx::Model &model) : m(model) {
        for (const auto &p : m.params) val[p.host_id] = m.default_host_value(p);
    }
    json at(const fx::Param &p, int64_t n) const {
        auto k = keys.find(p.host_id);
        if (k == keys.end() || k->second.empty()) return val.at(p.host_id);
        // FxKeyframeAPI keys: linear segments (constant for bools and menus).
        auto hi = k->second.lower_bound(n);
        if (hi == k->second.begin()) return hi->second;
        if (hi == k->second.end()) return std::prev(hi)->second;
        if (hi->first == n) return hi->second;
        const auto &lo = *std::prev(hi);
        if (p.type == fx::Kind::Bool || p.type == fx::Kind::Choice) return lo.second;
        const double u = double(n - lo.first) / double(hi->first - lo.first);
        auto mix = [&](const json &a, const json &b) { return a.get<double>() + (b.get<double>() - a.get<double>()) * u; };
        if (lo.second.is_array()) {
            json out = json::array();
            for (size_t i = 0; i < lo.second.size(); ++i) out.push_back(mix(lo.second[i], hi->second[i]));
            return out;
        }
        if (p.type == fx::Kind::Int) return json(int64_t(std::llround(mix(lo.second, hi->second))));
        return json(mix(lo.second, hi->second));
    }
    void set(uint32_t id, const json &v) {
        auto k = keys.find(id);
        if (k != keys.end() && !k->second.empty()) k->second[0] = v;
        else val[id] = v;
    }
    // NTSCRTPlugIn.mm applyPreset / applyLookPreset
    void apply_preset(const json &preset, double fps, int64_t frames) {
        const fx::HostPlan plan = m.plan_preset(preset, fps, frames);
        for (const auto &[id, entries] : plan.look) {
            keys[id].clear();
            if (entries.size() == 1) set(id, entries.front().second);
            else
                for (const auto &[n, v] : entries) keys[id][n] = v;
        }
        for (const auto &[id, v] : plan.feedback) set(id, v);
    }
    void apply(const fx::HostValues &v) {
        for (const auto &[id, x] : v) set(id, x);
    }
    Built look(int64_t n) const {
        return m.build([&](const fx::Param &p) { return at(p, n); });
    }
};

void check_same(const Built &ofx, const Built &fxp) {
    std::string where;
    // 1e-12: both interpolate the same keys, one in look values, one in host values.
    CHECK_MESSAGE(close(ofx.look, fxp.look, where, "", 1e-12), "look " << where);
    CHECK_MESSAGE(close(ofx.feedback, fxp.feedback, where, "", 0.0), "feedback " << where);
}
}  // namespace

TEST_CASE("cross-host: both Preset menus list the same presets in the same order") {
    const fx::Model &m = fx_model();
    std::vector<std::string> ofx_looks, ofx_loops, fx_looks, fx_loops;
    for (const auto &p : parse_presets(bundled(), schema().digital_formats)) (p.feedback ? ofx_loops : ofx_looks).push_back(p.name);
    for (const auto &p : m.look_menu(bundled())) fx_looks.push_back(p.name);
    for (const auto &p : m.loop_menu(bundled())) fx_loops.push_back(p.name);
    CHECK(fx_looks == ofx_looks);
    CHECK(fx_loops == ofx_loops);
    CHECK(ofx_looks.size() > 30);
    CHECK(ofx_loops.size() == 5);
}

TEST_CASE("cross-host: the default instance builds the same look") {
    check_same(OfxHost(schema()).look(0), FxHost(fx_model()).look(0));
}

TEST_CASE("cross-host: every bundled preset builds the same look and feedback in both plug-ins") {
    const fx::Model &m = fx_model();
    const Schema &s = schema();
    const int64_t kAvailable = 300;
    size_t animated = 0;
    for (const auto &p : parse_presets(bundled(), s.digital_formats)) {
        CAPTURE(p.name);
        OfxHost o(s);
        FxHost f(m);
        if (p.format) {
            o.apply_values(applies_values(s, p.data));
            f.apply(m.applies_host_values(p.data));
            check_same(o.look(0), f.look(0));
            continue;
        }
        o.apply_preset(p.data, 30, kAvailable);
        f.apply_preset(p.data, 30, kAvailable);
        const bool anim = preset_keys(s, p.data, 30, kAvailable).animated();
        animated += anim;
        for (int64_t n : anim ? std::vector<int64_t>{0, 1, 13, 29, 59, 60, 149, 299} : std::vector<int64_t>{0}) {
            CAPTURE(n);
            check_same(o.look(n), f.look(n));
        }
        if (p.feedback) CHECK(screen_loop_on(f.look(0).feedback));  // a Screen Loop preset turns it on
    }
    CHECK(animated >= 16);  // the bundled looks with timeline keys
}

TEST_CASE("cross-host: Screen Loop presets over a look, the Off / (Custom) switch, choice applies") {
    const fx::Model &m = fx_model();
    const Schema &s = schema();
    const auto presets = parse_presets(bundled(), s.digital_formats);
    auto find = [&](const std::string &n) -> const json & {
        for (const auto &p : presets)
            if (p.name == n) return p.data;
        throw std::runtime_error("no preset " + n);
    };
    OfxHost o(s);
    FxHost f(m);
    o.apply_preset(find("Clean VHS"), 30, 300);
    f.apply_preset(find("Clean VHS"), 30, 300);
    o.apply_preset(find("Screen Loop/22 copies bottom left"), 30, 300);
    f.apply_preset(find("Screen Loop/22 copies bottom left"), 30, 300);
    check_same(o.look(0), f.look(0));
    CHECK(screen_loop_on(o.look(0).feedback));
    // FxPlug's Screen Loop menu "Off" = the OpenFX Screen Loop checkbox off.
    o.set_value(index_of(s, "screenloop.enabled"), false);
    f.apply(m.screen_loop_switch(false));
    check_same(o.look(0), f.look(0));
    CHECK_FALSE(screen_loop_on(f.look(0).feedback));
    f.apply(m.screen_loop_switch(true));
    o.set_value(index_of(s, "screenloop.enabled"), true);
    check_same(o.look(0), f.look(0));
    // Horizontal resolution writes the width; a width edit demotes it to Custom.
    const fx::Param &pre = *m.find("downscale.preset"), &w = *m.find("downscale.width");
    o.user_edit(pre.index, "Arcade (384px)");
    f.set(pre.host_id, *m.to_host_value(pre, "Arcade (384px)"));
    f.apply(m.edit_effects(pre, [&](const fx::Param &p) { return f.at(p, 0); }));
    check_same(o.look(0), f.look(0));
    o.user_edit(w.index, int64_t(500));
    f.set(w.host_id, int64_t(500));
    f.apply(m.edit_effects(w, [&](const fx::Param &p) { return f.at(p, 0); }));
    check_same(o.look(0), f.look(0));
    CHECK(o.look(0).look["downscale"]["preset"] == "Custom");
}
