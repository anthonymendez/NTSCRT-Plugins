// ofx_params: look building from the real schema.
#include <doctest/doctest.h>
#include <ntscrt/ntscrt.h>

#include "ofx_params.h"

using namespace ntscrt_ofx;

namespace {
// The look the plug-in sends with every param at its default, except `crt.preset`.
json look_with_crt_preset(const Schema &s, const std::string &preset) {
    json look, fb;
    build_look(s, [&](size_t i) {
        return s.params[i].id == "crt.preset" ? json(preset) : s.params[i].def;
    }, look, fb);
    return look;
}
}  // namespace

TEST_CASE("build_look writes only the selected CRT preset's shader params") {
    Schema s = Schema::parse(ntscrt_param_schema_json());
    // aperture and easymode share GAMMA_INPUT / GAMMA_OUTPUT, easymode and hyllian share
    // MASK_STRENGTH, all with different defaults: a later preset must not overwrite them.
    json ap = look_with_crt_preset(s, "aperture");
    REQUIRE(ap["shader"]["preset"] == "aperture");
    CHECK(ap["shader"]["params"]["GAMMA_INPUT"].get<double>() == doctest::Approx(2.4));
    CHECK(ap["shader"]["params"]["GAMMA_OUTPUT"].get<double>() == doctest::Approx(2.4));

    json em = look_with_crt_preset(s, "easymode");
    CHECK(em["shader"]["params"]["GAMMA_INPUT"].get<double>() == doctest::Approx(2.0));
    CHECK(em["shader"]["params"]["MASK_STRENGTH"].get<double>() == doctest::Approx(0.3));

    // every written shader param belongs to the selected preset
    for (const auto &p : s.params) {
        if (p.group.rfind("crt.", 0) != 0 || p.path.rfind("/shader/params/", 0) != 0) continue;
        if (p.group == "crt.easymode") continue;
        bool shared = false;
        for (const auto &q : s.params)
            if (q.group == "crt.easymode" && q.path == p.path) shared = true;
        if (!shared) CHECK_MESSAGE(!em["shader"]["params"].contains(p.path.substr(15)), p.id);
    }
}
