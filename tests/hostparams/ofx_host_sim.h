// ofx_host_sim.h — what ofx/ntscrt_ofx.cpp does with the shared param model, on a
// simulated OpenFX host (one value per param, keyframes as a frame -> value map,
// interpolated linearly between keys, stepped for bools and choices, as Resolve's
// Edit page and the mock host do).
// Used by the OpenFX regression test and the cross-host test.
#pragma once

#include <ntscrt/ntscrt.h>

#include "ntscrt_host/params.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace hostsim {
using namespace ntscrt::host;

struct OfxHost {
    const Schema &s;
    std::vector<json> val;
    std::vector<std::map<int64_t, json>> keys;
    explicit OfxHost(const Schema &sc) : s(sc), val(sc.params.size()), keys(sc.params.size()) {
        for (size_t i = 0; i < s.params.size(); ++i) val[i] = s.params[i].def;
    }
    json at(size_t i, int64_t n) const {
        if (keys[i].empty()) return val[i];
        const std::vector<std::pair<int64_t, json>> k(keys[i].begin(), keys[i].end());
        return interpolated_key_value(s.params[i], k, double(n));
    }
    // ntscrt_ofx.cpp set_value: a keyed param gets a key at t, else its value.
    void set_value(size_t i, const json &v, int64_t t = 0) {
        if (!keys[i].empty()) keys[i][t] = v;
        else val[i] = v;
    }
    // ntscrt_ofx.cpp apply_preset (the clip starts at 0, `available` frames long).
    void apply_preset(const json &preset, double fps, int64_t available) {
        const PresetPlan plan = plan_preset(s, preset, fps, available);
        for (const auto &[i, entries] : plan.look.keys) {
            keys[i].clear();
            if (entries.size() == 1) set_value(i, entries.front().second);
            else
                for (const auto &[n, v] : entries) keys[i][n] = v;
        }
        for (const auto &[i, v] : plan.feedback) set_value(i, v);
    }
    void apply_values(const std::vector<std::pair<size_t, json>> &vals) {
        for (const auto &[i, v] : vals) set_value(i, v);
    }
    // The user sets param i to v (ntscrt_ofx.cpp changedParam -> apply_choice_effects).
    void user_edit(size_t i, const json &v) {
        set_value(i, v);
        apply_values(edit_effects(s, i, [&](size_t k) { return at(k, 0); }));
    }
    Built look(int64_t n) const {
        return build_look(s, [&](size_t i) { return at(i, n); });
    }
};

inline const Schema &schema() {
    static const Schema s = Schema::parse(ntscrt_param_schema_json());
    return s;
}

// The bundled presets the plug-ins list (the built bundle's Resources/presets).
inline const json &bundled() {
    static const json j = json::parse(bundled_presets_json(NTSCRT_BUILT_RESOURCES_DIR));
    return j;
}

inline json load_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return json::parse(ss.str());
}

inline size_t index_of(const Schema &s, const std::string &id) {
    if (auto i = s.index_of(id)) return *i;
    throw std::runtime_error("no param " + id);
}

// Equal, numbers to `tol` relative (timeline easing goes through pow on some platforms).
inline bool close(const json &a, const json &b, std::string &where, const std::string &at = "", double tol = 1e-9) {
    if (a.is_number() && b.is_number()) {
        const double x = a.get<double>(), y = b.get<double>();
        if (std::fabs(x - y) <= tol * std::max(1.0, std::max(std::fabs(x), std::fabs(y)))) {
            if (a.is_number_float() == b.is_number_float()) return true;  // 1 and 1.0 dump differently
        }
        where = at + ": " + a.dump() + " vs " + b.dump();
        return false;
    }
    if (a.type() != b.type()) {
        where = at + ": " + a.dump() + " vs " + b.dump();
        return false;
    }
    if (a.is_object()) {
        for (auto &[k, v] : a.items())
            if (!b.contains(k)) {
                where = at + "/" + k + ": only in the first";
                return false;
            }
        for (auto &[k, v] : b.items()) {
            if (!a.contains(k)) {
                where = at + "/" + k + ": only in the second";
                return false;
            }
            if (!close(a[k], v, where, at + "/" + k, tol)) return false;
        }
        return true;
    }
    if (a.is_array()) {
        if (a.size() != b.size()) {
            where = at + ": array sizes differ";
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i)
            if (!close(a[i], b[i], where, at + "/" + std::to_string(i), tol)) return false;
        return true;
    }
    if (a != b) {
        where = at + ": " + a.dump() + " vs " + b.dump();
        return false;
    }
    return true;
}
}  // namespace hostsim
