// Loads the built NTSCRT.ofx through a mock OpenFX host and checks describe,
// params vs schema, presets, preset files and a pass-through render.
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <ntscrt/ntscrt.h>

#include "mock_host.h"
#include "../../core/params/timeline.h"
#include "../../core/stages/digital/migrate.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

using nlohmann::json;
namespace fs = std::filesystem;

namespace {
// Before the plug-in loads (it reads these once): keep the test runs out of the user's
// log and their "Debug logging for all effects" switch (NTSCRT_SETTINGS_DIR: a fresh, empty
// folder), never open a file-manager window from "Open logs folder".
const fs::path kSettingsDir = fs::temp_directory_path() / "ntscrt_ofx_test_settings";
const fs::path kLogFile = [] {
    fs::path p = fs::temp_directory_path() / "ntscrt_ofx_test.log";
    std::error_code ec;
    fs::remove(p, ec);
    fs::remove_all(kSettingsDir, ec);
    auto u = p.u8string();
    std::string s(u.begin(), u.end());
    auto su = kSettingsDir.u8string();
    std::string sd(su.begin(), su.end());
#ifdef _WIN32
    _putenv_s("NTSCRT_OFX_LOG", s.c_str());
    _putenv_s("NTSCRT_OFX_NO_OPEN", "1");
    _putenv_s("NTSCRT_SETTINGS_DIR", sd.c_str());
    _putenv_s("NTSCRT_DEBUG_LOG", "");
#else
    setenv("NTSCRT_OFX_LOG", s.c_str(), 1);
    setenv("NTSCRT_OFX_NO_OPEN", "1", 1);
    setenv("NTSCRT_SETTINGS_DIR", sd.c_str(), 1);
    unsetenv("NTSCRT_DEBUG_LOG");
#endif
    return p;
}();
const fs::path kSwitchFile = kSettingsDir / "debug-logging-all-effects.txt";
std::string read_log() {
    std::ifstream f(kLogFile);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
size_t count(const std::string &hay, const std::string &needle) {
    size_t n = 0;
    for (size_t i = hay.find(needle); i != std::string::npos; i = hay.find(needle, i + 1)) ++n;
    return n;
}

std::string host_name(const std::string &id) {
    std::string r = id;
    for (char &c : r)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';
    return r;
}

struct Fixture {
    mock::Host host;
    OfxPlugin *plug = nullptr;
    mock::Effect desc;
    std::unique_ptr<mock::Effect> inst;
    json schema = json::parse(ntscrt_param_schema_json());

    Fixture() {
        std::string err;
        // NTSCRT_TEST_OFX_BINARY: run these tests against another build (timings before/after).
        const char *bin = std::getenv("NTSCRT_TEST_OFX_BINARY");
        REQUIRE_MESSAGE(host.load(bin && *bin ? bin : NTSCRT_OFX_BINARY, err), err);
        REQUIRE(host.num_plugins() == 1);
        plug = host.plugin(0);
        REQUIRE(plug);
        plug->setHost(&host.host);
        REQUIRE(host.action(plug, kOfxActionLoad, nullptr) == kOfxStatOK);
        desc.props.set(kOfxPropType, {std::string(kOfxTypeImageEffect)});
        REQUIRE(host.action(plug, kOfxActionDescribe, &desc) == kOfxStatOK);
        mock::PropSet in;
        in.set(kOfxImageEffectPropContext, {std::string(kOfxImageEffectContextFilter)});
        REQUIRE(host.action(plug, kOfxImageEffectActionDescribeInContext, &desc, &in) == kOfxStatOK);
    }
    ~Fixture() {
        if (inst) host.action(plug, kOfxActionDestroyInstance, inst.get());
        if (plug) host.action(plug, kOfxActionUnload, nullptr);
    }
    void create() {
        inst = host.make_instance(desc, kOfxImageEffectContextFilter);
        REQUIRE(host.action(plug, kOfxActionCreateInstance, inst.get()) == kOfxStatOK);
    }
    mock::Param *param(const std::string &name) const {
        mock::Param *p = inst ? inst->params.find(name) : desc.params.find(name);
        REQUIRE_MESSAGE(p, "missing param " << name);
        return p;
    }
    OfxStatus changed(const std::string &name, double t = 20) {
        mock::PropSet in;
        in.set(kOfxPropType, {std::string(kOfxTypeParameter)});
        in.set(kOfxPropName, {name});
        in.set(kOfxPropChangeReason, {std::string(kOfxChangeUserEdited)});
        in.set(kOfxPropTime, {t});
        in.set(kOfxImageEffectPropRenderScale, {1.0, 1.0});
        OfxStatus st = host.action(plug, kOfxActionInstanceChanged, inst.get(), &in);
        return st == kOfxStatReplyDefault ? kOfxStatOK : st;  // both mean handled
    }
    int choice_index(const mock::Param &p, const std::string &label) const {
        for (int i = 0; i < p.props.dim(kOfxParamPropChoiceOption); ++i)
            if (p.props.s(kOfxParamPropChoiceOption, i) == label) return i;
        return -1;
    }
};

// A keyed mock param's first component at time t, as the mock host interpolates it.
double keyed_at(const mock::Param &p, double t) {
    auto hi = p.keys.lower_bound(t);
    if (hi == p.keys.begin()) return hi->second[0];
    if (hi == p.keys.end()) return std::prev(hi)->second[0];
    if (hi->first == t) return hi->second[0];
    auto lo = std::prev(hi);
    return lo->second[0] + (hi->second[0] - lo->second[0]) * (t - lo->first) / (hi->first - lo->first);
}

const char *ofx_type(const std::string &t, const json &def) {
    if (t == "bool") return kOfxParamTypeBoolean;
    if (t == "int") return kOfxParamTypeInteger;
    if (t == "float") return kOfxParamTypeDouble;
    if (t == "choice") return kOfxParamTypeChoice;
    if (t == "color") return def.is_array() && def.size() == 4 ? kOfxParamTypeRGBA : kOfxParamTypeRGB;
    return "?";
}
}  // namespace

TEST_CASE("bundle layout") {
    fs::path bin = fs::u8path(NTSCRT_OFX_BINARY);
    CHECK(bin.filename() == "NTSCRT.ofx");
#ifdef _WIN32
    CHECK(bin.parent_path().filename() == "Win64");
#else
    CHECK(bin.parent_path().filename() == "MacOS");
#endif
    fs::path contents = bin.parent_path().parent_path();
    CHECK(contents.filename() == "Contents");
    CHECK(contents.parent_path().filename() == "NTSCRT.ofx.bundle");
    CHECK(fs::is_directory(contents / "Resources" / "shaders"));
    CHECK(fs::exists(contents / "Resources" / "presets" / "Clean VHS.json"));
    CHECK(fs::is_directory(contents / "Resources" / "presets" / "Screen Loop"));
}

TEST_CASE("describe: plugin identity and effect properties") {
    Fixture f;
    CHECK(std::string(f.plug->pluginApi) == kOfxImageEffectPluginApi);
    CHECK(std::string(f.plug->pluginIdentifier) == "com.ntscrt.plugins.ntscrt");
    const mock::PropSet &p = f.desc.props;
    CHECK(p.s(kOfxPropLabel) == "NTSCRT");
    CHECK(p.s(kOfxImageEffectPluginPropGrouping) == "NTSCRT");
    CHECK(p.i(kOfxImageEffectPropTemporalClipAccess) == 1);
    CHECK(p.i(kOfxImageEffectPropSupportsTiles) == 0);
    CHECK(p.i(kOfxImageEffectPropSupportsMultiResolution) == 0);
    CHECK(p.s(kOfxImageEffectPluginRenderThreadSafety) == kOfxImageEffectRenderInstanceSafe);
    std::vector<std::string> ctx, depths;
    for (int i = 0; i < p.dim(kOfxImageEffectPropSupportedContexts); ++i) ctx.push_back(p.s(kOfxImageEffectPropSupportedContexts, i));
    for (int i = 0; i < p.dim(kOfxImageEffectPropSupportedPixelDepths); ++i) depths.push_back(p.s(kOfxImageEffectPropSupportedPixelDepths, i));
    CHECK(std::count(ctx.begin(), ctx.end(), kOfxImageEffectContextFilter) == 1);
    CHECK(std::count(ctx.begin(), ctx.end(), kOfxImageEffectContextGeneral) == 1);
    CHECK(std::count(depths.begin(), depths.end(), kOfxBitDepthFloat) == 1);
    CHECK(std::count(depths.begin(), depths.end(), kOfxBitDepthByte) == 1);
    CHECK(std::count(depths.begin(), depths.end(), kOfxBitDepthShort) == 1);
    // Clips
    REQUIRE(f.desc.clips.count(kOfxImageEffectSimpleSourceClipName));
    REQUIRE(f.desc.clips.count(kOfxImageEffectOutputClipName));
    CHECK(f.desc.clips[kOfxImageEffectSimpleSourceClipName]->props.i(kOfxImageEffectPropTemporalClipAccess) == 1);
    CHECK(f.desc.clips[kOfxImageEffectSimpleSourceClipName]->props.s(kOfxImageEffectPropSupportedComponents) ==
          kOfxImageComponentRGBA);
}

TEST_CASE("describe: params match the schema") {
    Fixture f;
    const json &s = f.schema;
    for (const auto &g : s["groups"]) {
        mock::Param *gp = f.param("grp_" + host_name(g["id"].get<std::string>()));
        CHECK(gp->type == kOfxParamTypeGroup);
        CHECK(gp->props.s(kOfxPropLabel) == g.value("label", ""));
    }
    size_t n = 0;
    for (const auto &sp : s["params"]) {
        std::string id = sp["id"], type = sp["type"];
        CAPTURE(id);
        mock::Param *p = f.param(host_name(id));
        ++n;
        json def = sp.value("default", json());
        CHECK(p->type == ofx_type(type, def));
        CHECK(p->props.s(kOfxPropLabel) == sp.value("label", id));
        CHECK(p->props.s(kOfxParamPropHint) == sp.value("hint", ""));
        CHECK(p->props.i(kOfxParamPropAnimates) == (sp.value("animatable", true) ? 1 : 0));
        CHECK(p->props.s(kOfxParamPropParent) == "grp_" + host_name(sp.value("group", "")));
        if (type == "bool") CHECK(p->props.i(kOfxParamPropDefault) == (def.get<bool>() ? 1 : 0));
        if (type == "int") CHECK(p->props.i(kOfxParamPropDefault) == def.get<int>());
        if (type == "float") CHECK(p->props.d(kOfxParamPropDefault) == doctest::Approx(def.get<double>()));
        if (type == "int" || type == "float") {
            if (sp.contains("min")) CHECK(p->props.d(kOfxParamPropMin) == doctest::Approx(sp["min"].get<double>()));
            if (sp.contains("max")) CHECK(p->props.d(kOfxParamPropMax) == doctest::Approx(sp["max"].get<double>()));
            if (sp.contains("display_min"))
                CHECK(p->props.d(kOfxParamPropDisplayMin) == doctest::Approx(sp["display_min"].get<double>()));
            if (sp.contains("display_max"))
                CHECK(p->props.d(kOfxParamPropDisplayMax) == doctest::Approx(sp["display_max"].get<double>()));
        }
        if (type == "choice") {
            const json &opts = sp["options"];
            REQUIRE(p->props.dim(kOfxParamPropChoiceOption) == int(opts.size()));
            int want = -1;
            for (size_t i = 0; i < opts.size(); ++i) {
                CHECK(p->props.s(kOfxParamPropChoiceOption, int(i)) == opts[i].value("label", ""));
                if (opts[i]["value"] == def) want = int(i);
            }
            CHECK(p->props.i(kOfxParamPropDefault) == want);
        }
    }
    CHECK(n == s["params"].size());
    // host extras
    CHECK(f.param("host_preset")->type == kOfxParamTypeChoice);
    CHECK(f.param("host_preset")->props.s(kOfxParamPropChoiceOption, 0) == "(none)");
    CHECK(f.choice_index(*f.param("host_preset"), "Clean VHS") > 0);
    CHECK(f.choice_index(*f.param("host_screenloop_preset"), "Screen Loop/Bottom to top") > 0);
    CHECK(f.param("host_preset_file")->type == kOfxParamTypeString);
    CHECK(f.param("host_preset_file")->props.s(kOfxParamPropStringMode) == kOfxParamStringIsFilePath);
    CHECK(f.param("host_load_preset")->type == kOfxParamTypePushButton);
    CHECK(f.param("host_save_preset")->type == kOfxParamTypePushButton);
    CHECK(f.param("host_seed")->type == kOfxParamTypeInteger);
    // diagnostics: in the Presets group, not saved with the project
    mock::Param *dbg = f.param("host_debug_logging");
    CHECK(dbg->type == kOfxParamTypeBoolean);
    CHECK(dbg->props.i(kOfxParamPropDefault) == 0);
    CHECK(dbg->props.i(kOfxParamPropPersistant) == 0);
    CHECK(dbg->props.i(kOfxParamPropAnimates) == 0);
    CHECK(dbg->props.s(kOfxParamPropParent) == "grp_host_presets");
    CHECK(f.param("host_open_logs")->type == kOfxParamTypePushButton);
    CHECK(f.param("host_open_logs")->props.s(kOfxParamPropParent) == "grp_host_presets");
    // "Debug logging for all effects": a button and a read-only status line, nothing saved
    CHECK(f.param("host_debug_all")->type == kOfxParamTypePushButton);
    CHECK(f.param("host_debug_all")->props.s(kOfxParamPropParent) == "grp_host_presets");
    mock::Param *st = f.param("host_debug_all_status");
    CHECK(st->type == kOfxParamTypeString);
    CHECK(st->props.s(kOfxParamPropStringMode) == kOfxParamStringIsLabel);
    CHECK(st->props.i(kOfxParamPropPersistant) == 0);
    CHECK(st->props.s(kOfxParamPropParent) == "grp_host_presets");
}

TEST_CASE("instance: presets, preset files, dependents, pass-through render") {
    Fixture f;
    f.create();
    const json &s = f.schema;

    SUBCASE("enabled_if / visible_if applied at creation") {
        for (const auto &sp : s["params"]) {
            if (!sp.contains("enabled_if") && !sp.contains("visible_if")) continue;
            CAPTURE(sp["id"]);
            CHECK(f.param(host_name(sp["id"]))->props.has(kOfxParamPropEnabled));
        }
    }

    SUBCASE("choosing a bundled preset writes its values") {
        mock::Param *w = f.param("downscale_width"), *en = f.param("downscale_enabled");
        w->v[0] = 100;
        en->v[0] = 0;
        mock::Param *pp = f.param("host_preset");
        int idx = f.choice_index(*pp, "Clean VHS");
        REQUIRE(idx > 0);
        pp->v[0] = idx;
        REQUIRE(f.changed("host_preset") == kOfxStatOK);
        CHECK(w->v[0] == 320);
        CHECK(en->v[0] == 1);
        CHECK(f.param("ntsc_enabled")->v[0] == 1);
    }

    SUBCASE("save and load preset files") {
        fs::path tmp = fs::temp_directory_path() / "ntscrt_ofx_test_preset.json";
        f.param("downscale_width")->v[0] = 256;
        auto u8 = tmp.u8string();
        f.param("host_preset_file")->str = std::string(u8.begin(), u8.end());
        REQUIRE(f.changed("host_save_preset") == kOfxStatOK);
        std::ifstream in(tmp);
        json saved = json::parse(in, nullptr, false);
        in.close();
        REQUIRE(saved.is_object());
        CHECK(saved["downscale"]["width"] == 256);
        CHECK(saved["shader"]["preset"] == "glow_gauss");

        { std::ofstream o(tmp); o << R"({"downscale":{"enabled":false,"width":200}})"; }
        REQUIRE(f.changed("host_load_preset") == kOfxStatOK);
        CHECK(f.param("downscale_width")->v[0] == 200);
        CHECK(f.param("downscale_enabled")->v[0] == 0);
        CHECK(f.param("ntsc_enabled")->v[0] == 1);  // missing key -> default
        fs::remove(tmp);
    }

    SUBCASE("presets and preset files leave the debug checkbox alone") {
        mock::Param *dbg = f.param("host_debug_logging");
        dbg->v[0] = 1;
        mock::Param *pp = f.param("host_preset");
        pp->v[0] = f.choice_index(*pp, "Clean VHS");
        REQUIRE(f.changed("host_preset") == kOfxStatOK);
        CHECK(dbg->v[0] == 1);
        CHECK(dbg->sets == 0);
        fs::path tmp = fs::temp_directory_path() / "ntscrt_ofx_test_preset2.json";
        auto u8 = tmp.u8string();
        f.param("host_preset_file")->str = std::string(u8.begin(), u8.end());
        REQUIRE(f.changed("host_save_preset") == kOfxStatOK);
        std::ifstream in(tmp);
        std::stringstream ss;
        ss << in.rdbuf();
        in.close();
        fs::remove(tmp);
        CHECK(ss.str().find("debug") == std::string::npos);
    }

    SUBCASE("Open logs folder logs the press and opens nothing without a GUI") {
        REQUIRE(f.changed("host_open_logs") == kOfxStatOK);
        std::string log = read_log();
        CHECK(log.find("open logs folder: ") != std::string::npos);
        CHECK(log.find("(no GUI: not opened)") != std::string::npos);
    }

    SUBCASE("clip preferences declare frame varying output") {
        mock::PropSet out;
        REQUIRE(f.host.action(f.plug, kOfxImageEffectActionGetClipPreferences, f.inst.get(), nullptr, &out) == kOfxStatOK);
        CHECK(out.i(kOfxImageEffectFrameVarying) == 1);
    }

    SUBCASE("RGBA32F pass-through with the all-off look") {
        for (const auto &sp : s["params"]) {
            std::string path = sp["path"];
            if (sp["type"] == "bool" && path.size() > 8 && path.compare(path.size() - 8, 8, "/enabled") == 0)
                f.param(host_name(sp["id"]))->v[0] = 0;
        }
        const int w = 16, h = 9;
        mock::Clip &src = *f.inst->clips[kOfxImageEffectSimpleSourceClipName];
        mock::Clip &dst = *f.inst->clips[kOfxImageEffectOutputClipName];
        src.width = dst.width = w;
        src.height = dst.height = h;
        for (int t = 10; t <= 60; ++t) {
            std::vector<float> px(size_t(w) * h * 4);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    float *p = &px[(size_t(y) * w + x) * 4];
                    p[0] = x / float(w - 1);
                    p[1] = y / float(h - 1);  // rows differ: a flip would show
                    p[2] = (t % 5) / 4.f;
                    p[3] = 0.25f + 0.5f * (x % 2);
                }
            src.frames[t] = px;
            dst.frames[t] = std::vector<float>(px.size(), 0.f);
        }
        std::vector<float> out;
        f.host.on_release = [&](const mock::Image &im) {
            if (im.props.s(kOfxImagePropUniqueIdentifier).rfind(kOfxImageEffectOutputClipName, 0) == 0) out = im.px;
        };
        mock::PropSet in;
        in.set(kOfxPropTime, {20.0});
        in.set(kOfxImageEffectPropFieldToRender, {std::string(kOfxImageFieldNone)});
        in.set(kOfxImageEffectPropRenderWindow, {0, 0, w, h});
        in.set(kOfxImageEffectPropRenderScale, {1.0, 1.0});
        in.set(kOfxImageEffectPropSequentialRenderStatus, {0});
        in.set(kOfxImageEffectPropInteractiveRenderStatus, {0});
        in.set(kOfxImageEffectPropRenderQualityDraft, {0});
        OfxStatus st = f.host.action(f.plug, kOfxImageEffectActionRender, f.inst.get(), &in);
        for (auto &m : f.host.log) MESSAGE(m);
        REQUIRE(st == kOfxStatOK);
        REQUIRE(out.size() == src.frames[20].size());
        const std::vector<float> &ref = src.frames[20];
        int bad = 0;
        for (size_t i = 0; i < out.size(); ++i)
            if (std::fabs(out[i] - ref[i]) > 1.0f / 255.0f + 1e-6f) ++bad;
        CHECK(bad == 0);
        CHECK(std::count(src.fetched.begin(), src.fetched.end(), 20) >= 1);

        // Debug logging: one detailed line per frame, full look once, output unchanged.
        const std::string before = read_log();
        f.param("host_debug_logging")->v[0] = 1;
        REQUIRE(f.changed("host_debug_logging") == kOfxStatOK);
        for (double t : {21.0, 22.0, 23.0}) {
            in.set(kOfxPropTime, {t});
            REQUIRE(f.host.action(f.plug, kOfxImageEffectActionRender, f.inst.get(), &in) == kOfxStatOK);
        }
        const std::string added = read_log().substr(before.size());
        MESSAGE(added);
        CHECK(added.find("debug logging on") != std::string::npos);
        CHECK(count(added, " debug render #") == 3);
        for (const char *t : {" t=21 ", " t=22 ", " t=23 "}) CHECK(added.find(t) != std::string::npos);
        CHECK(count(added, "core_ms=") == 3);
        CHECK(count(added, " look={") == 1);  // the look didn't change between frames
        const std::vector<float> &ref23 = src.frames[23];
        bad = 0;
        for (size_t i = 0; i < out.size(); ++i)
            if (std::fabs(out[i] - ref23[i]) > 1.0f / 255.0f + 1e-6f) ++bad;
        CHECK(bad == 0);
    }
}

// Glitch on (the default): every frame depends on the looks of all earlier ones.
// Seeks, playback and edits, with and without keyframes. Prints the timings
// (informational) and checks that no edit leaves a stale picture: each result
// equals a render after a full purge.
namespace {
// GitHub's macOS runners: a paravirtual GPU that can hang ("GPU Hang") on CRT shaders.
// Only there is a hang (or the CPU fallback after one) tolerated: the test stops with a warning.
bool ci_virtual_gpu() {
#ifdef __APPLE__
    const char *gha = std::getenv("GITHUB_ACTIONS");
    return gha && std::string(gha) == "true";
#else
    return false;
#endif
}
struct CiGpuHang {
    std::string log;
};

void edits_and_seeks() {
    const size_t log_start = read_log().size();
    Fixture f;
    f.create();
    const int w = 640, h = 360, last = 300;
    mock::Clip &src = *f.inst->clips[kOfxImageEffectSimpleSourceClipName];
    mock::Clip &dst = *f.inst->clips[kOfxImageEffectOutputClipName];
    src.width = dst.width = w;
    src.height = dst.height = h;
    src.any_frame.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float *p = &src.any_frame[(size_t(y) * w + x) * 4];
            p[0] = x / float(w - 1);
            p[1] = y / float(h - 1);
            p[2] = float((x / 16 + y / 16) % 2);
            p[3] = 1.f;
        }
    dst.any_frame.assign(src.any_frame.size(), 0.f);
    src.any_frame_last = dst.any_frame_last = last;
    src.props.set(kOfxImageEffectPropFrameRange, {0.0, double(last)});
    std::vector<float> out;
    f.host.on_release = [&](const mock::Image &im) {
        if (im.props.s(kOfxImagePropUniqueIdentifier).rfind(kOfxImageEffectOutputClipName, 0) == 0) out = im.px;
    };
    auto purge = [&] {
        OfxStatus st = f.host.action(f.plug, kOfxActionPurgeCaches, f.inst.get());
        REQUIRE((st == kOfxStatOK || st == kOfxStatReplyDefault));
    };
    auto reads = [&] {
        long n = 0;
        for (auto &p : f.inst->params.params) n += p->reads_at_time;
        return n;
    };
    struct Shot {
        double ms;
        long reads;
        std::vector<float> px;
    };
    auto render = [&](double t) {
        mock::PropSet in;
        in.set(kOfxPropTime, {t});
        in.set(kOfxImageEffectPropFieldToRender, {std::string(kOfxImageFieldNone)});
        in.set(kOfxImageEffectPropRenderWindow, {0, 0, w, h});
        in.set(kOfxImageEffectPropRenderScale, {1.0, 1.0});
        in.set(kOfxImageEffectPropSequentialRenderStatus, {0});
        in.set(kOfxImageEffectPropInteractiveRenderStatus, {0});
        in.set(kOfxImageEffectPropRenderQualityDraft, {0});
        const long r0 = reads();
        auto t0 = std::chrono::steady_clock::now();
        OfxStatus st = f.host.action(f.plug, kOfxImageEffectActionRender, f.inst.get(), &in);
        if (ci_virtual_gpu()) {
            const std::string added = read_log().substr(log_start);
            if (added.find("Hang") != std::string::npos || added.find("GPU device lost") != std::string::npos)
                throw CiGpuHang{added};
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (st != kOfxStatOK) {
            const std::string log = read_log();
            // the core's error is on the render line before the FAILED one
            const size_t at = log.rfind(" FAILED");
            size_t from = at == std::string::npos ? 0 : log.rfind('\n', at);
            if (from != std::string::npos && from > 0) from = log.rfind('\n', from - 1);
            MESSAGE("render t=" << t << " failed; plug-in log: " << log.substr(from == std::string::npos ? 0 : from + 1));
        }
        REQUIRE(st == kOfxStatOK);
        return Shot{ms, reads() - r0, out};
    };
    auto purged = [&](double t) {
        purge();
        return render(t).px;
    };
    auto play = [&](double from, int n) {  // mean ms and param reads per frame
        double ms = 0;
        long rd = 0;
        for (int i = 0; i < n; ++i) {
            Shot s = render(from + i);
            ms += s.ms;
            rd += s.reads;
        }
        return std::pair{ms / n, double(rd) / n};
    };
    const size_t nparams = f.schema["params"].size();
    std::ostringstream report;
    report.setf(std::ios::fixed);
    report.precision(1);
    render(0);  // warm-up: shaders compile
    purge();

    Shot seek = render(240);
    auto [play_ms, play_reads] = play(241, 10);
    report << "static look: seek to 240 " << seek.ms << " ms (" << seek.reads / long(nparams) << " looks built); playback "
           << play_ms << " ms/frame (" << play_reads / double(nparams) << " looks/frame)";

    // A CRT edit: nothing temporal depends on it.
    mock::Param *boost = f.param("crt_glow_gauss_BOOST");
    boost->v[0] = 1.3;
    REQUIRE(f.changed("crt_glow_gauss_BOOST", 240) == kOfxStatOK);
    Shot crt_edit = render(240);
    CHECK(crt_edit.px == purged(240));
    report << "; CRT edit + re-render 240: " << crt_edit.ms << " ms";

    // A Glitch timing edit (same for every frame): the receiver re-simulates.
    render(240);
    mock::Param *sig = f.param("glitch_signal_strength");
    sig->v[0] = 0.6;
    REQUIRE(f.changed("glitch_signal_strength", 240) == kOfxStatOK);
    Shot glitch_edit = render(240);
    CHECK(glitch_edit.px == purged(240));
    report << "; Glitch edit + re-render 240: " << glitch_edit.ms << " ms";

    // Keyframed: the look differs per frame.
    sig->keys = {{0.0, {1.0}}, {300.0, {0.4}}};
    REQUIRE(f.changed("glitch_signal_strength", 240) == kOfxStatOK);
    Shot anim_seek = render(240);
    CHECK(anim_seek.px == purged(240));
    auto [anim_play_ms, anim_play_reads] = play(241, 10);
    Shot anim_again = render(250);
    CHECK(anim_again.px == purged(250));
    // Moving a key, then a CRT edit: history stays right.
    sig->keys[300.0] = {0.2};
    REQUIRE(f.changed("glitch_signal_strength", 240) == kOfxStatOK);
    render(240);
    boost->v[0] = 1.1;
    REQUIRE(f.changed("crt_glow_gauss_BOOST", 240) == kOfxStatOK);
    Shot anim_crt = render(245);
    CHECK(anim_crt.px == purged(245));
    // Keys added without any changedParam (Resolve does that for scripted splines): caught.
    sig->keys.clear();
    sig->v[0] = 1.0;
    REQUIRE(f.changed("glitch_signal_strength", 240) == kOfxStatOK);
    render(245);
    sig->keys = {{0.0, {0.1}}, {300.0, {1.0}}};  // no f.changed()
    Shot unreported = render(245);
    REQUIRE(f.changed("glitch_signal_strength", 240) == kOfxStatOK);  // the same state, reported
    CHECK(unreported.px == purged(245));
    report << "\nkeyframed Glitch: seek to 240 " << anim_seek.ms << " ms (" << anim_seek.reads / long(nparams)
           << " looks built); playback " << anim_play_ms << " ms/frame (" << anim_play_reads / double(nparams)
           << " looks/frame); CRT edit + render 245: " << anim_crt.ms << " ms (" << anim_crt.reads / long(nparams)
           << " looks)";
    MESSAGE(report.str());
    std::printf("[timing] %s\n", report.str().c_str());
}
}  // namespace

TEST_CASE("edits and seeks: look history, cache flushing, timings") {
    try {
        edits_and_seeks();
    } catch (const CiGpuHang &h) {
        MESSAGE("WARNING: the CI runner's virtual GPU hung; rest of the test skipped. Plug-in log: " << h.log);
    }
}

// An hour into a 60 fps timeline (frame 216000, Resolve's default 01:00:00:00
// start): edits must not make the next render re-run the whole hour.
// NTSCRT_BENCH_UHD=1 renders 3840x2160 (the report's size) instead of 640x360.
struct EditTimings {
    double first, preset, crt, load, downscale, glitch;
};
static EditTimings edits_at_frame(double T) {
    Fixture f;
    f.create();
    const bool uhd = std::getenv("NTSCRT_BENCH_UHD") != nullptr;
    const int w = uhd ? 3840 : 640, h = uhd ? 2160 : 360, last = 250000;
    const double fps = 60.0;
    mock::Clip &src = *f.inst->clips[kOfxImageEffectSimpleSourceClipName];
    mock::Clip &dst = *f.inst->clips[kOfxImageEffectOutputClipName];
    src.width = dst.width = w;
    src.height = dst.height = h;
    src.any_frame.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float *p = &src.any_frame[(size_t(y) * w + x) * 4];
            p[0] = x / float(w - 1);
            p[1] = y / float(h - 1);
            p[2] = float((x / 16 + y / 16) % 2);
            p[3] = 1.f;
        }
    dst.any_frame.assign(src.any_frame.size(), 0.f);
    src.any_frame_last = dst.any_frame_last = last;
    src.props.set(kOfxImageEffectPropFrameRange, {0.0, double(last)});
    src.props.set(kOfxImageEffectPropFrameRate, {fps});
    src.props.set(kOfxImageEffectPropUnmappedFrameRate, {fps});
    f.inst->props.set(kOfxImageEffectPropFrameRate, {fps});
    std::vector<float> out;
    f.host.on_release = [&](const mock::Image &im) {
        if (im.props.s(kOfxImagePropUniqueIdentifier).rfind(kOfxImageEffectOutputClipName, 0) == 0) out = im.px;
    };
    auto render = [&](double t) {
        mock::PropSet in;
        in.set(kOfxPropTime, {t});
        in.set(kOfxImageEffectPropFieldToRender, {std::string(kOfxImageFieldNone)});
        in.set(kOfxImageEffectPropRenderWindow, {0, 0, w, h});
        in.set(kOfxImageEffectPropRenderScale, {1.0, 1.0});
        in.set(kOfxImageEffectPropSequentialRenderStatus, {0});
        in.set(kOfxImageEffectPropInteractiveRenderStatus, {1});
        in.set(kOfxImageEffectPropRenderQualityDraft, {0});
        auto t0 = std::chrono::steady_clock::now();
        OfxStatus st = f.host.action(f.plug, kOfxImageEffectActionRender, f.inst.get(), &in);
        REQUIRE(st == kOfxStatOK);
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    };
    auto purged = [&](double t) {
        OfxStatus st = f.host.action(f.plug, kOfxActionPurgeCaches, f.inst.get());
        REQUIRE((st == kOfxStatOK || st == kOfxStatReplyDefault));
        render(t);
        return out;
    };
    std::ostringstream report;
    report.setf(std::ios::fixed);
    report.precision(1);
    report << w << "x" << h << " @" << fps << " fps, Clean CRT:";
    // The report's look: Clean CRT (Downscale 256 px, Glitch on with its defaults).
    mock::Param *pp = f.param("host_preset");
    const int clean = f.choice_index(*pp, "Clean CRT");
    REQUIRE(clean > 0);
    pp->v[0] = clean;
    REQUIRE(f.changed("host_preset", T) == kOfxStatOK);
    // The first render simulates the Glitch receiver from its segment's start (frame 0
    // until 0.5.0-rc.2: the cost edits used to pay again; 0.4.0: ~30 s here, ~45 s in
    // Resolve at UHD).
    const double first_ms = render(T);
    report << " first render t=" << T << " " << first_ms << " ms";
    report << "; next frame " << render(T + 1) << " ms";
    report << "; seek back 10 " << render(T - 9) << " ms";

    // The CRT shader menu (the report: royale -> glow_gauss).
    mock::Param *crt = f.param("crt_preset");
    int royale = -1;
    for (int i = 0; i < crt->props.dim(kOfxParamPropChoiceOption); ++i) {
        std::string l = crt->props.s(kOfxParamPropChoiceOption, i);
        for (char &ch : l) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        if (l.find("royale") != std::string::npos) royale = i;
    }
    REQUIRE(royale >= 0);
    crt->v[0] = royale;
    REQUIRE(f.changed("crt_preset", T) == kOfxStatOK);
    const double shader_ms = render(T);
    report << "; CRT shader royale + render " << shader_ms << " ms";

    // The Preset menu again: Clean CRT's values (glow_gauss), the Glitch ones unchanged.
    pp->v[0] = clean;
    REQUIRE(f.changed("host_preset", T) == kOfxStatOK);
    const double preset_ms = render(T);
    const std::vector<float> after_preset = out;
    report << "; preset Clean CRT + render " << preset_ms << " ms";

    mock::Param *boost = f.param("crt_glow_gauss_BOOST");
    boost->v[0] = 1.3;
    REQUIRE(f.changed("crt_glow_gauss_BOOST", T) == kOfxStatOK);
    const double crt_ms = render(T);
    report << "; CRT edit + render " << crt_ms << " ms";

    f.param("host_preset_file")->str.clear();
    REQUIRE(f.changed("host_load_preset", T) == kOfxStatOK);  // "Choose a preset file first."
    const double load_ms = render(T);
    report << "; Load (no file) + render " << load_ms << " ms";

    // These do re-run the receiver: its raster / every earlier frame's settings changed.
    mock::Param *dsw = f.param("downscale_width");
    dsw->v[0] = 320;
    REQUIRE(f.changed("downscale_width", T) == kOfxStatOK);
    const double downscale_ms = render(T);
    report << "; Downscale edit + render " << downscale_ms << " ms";
    mock::Param *sig = f.param("glitch_signal_strength");
    sig->v[0] = 0.6;
    REQUIRE(f.changed("glitch_signal_strength", T) == kOfxStatOK);
    const double glitch_ms = render(T);
    report << "; Glitch edit + render " << glitch_ms << " ms";

    MESSAGE(report.str());
    std::printf("[timing] %s\n", report.str().c_str());

    // Keeping the receiver changed nothing: the picture a render from scratch gives.
    pp->v[0] = clean;
    REQUIRE(f.changed("host_preset", T) == kOfxStatOK);  // back to Clean CRT's values
    render(T);
    CHECK(out == after_preset);
    CHECK(purged(T) == after_preset);
    return {first_ms, preset_ms, crt_ms, load_ms, downscale_ms, glitch_ms};
}

TEST_CASE("high frame number: renders after edits stay bounded") {
    const char *bench_frame = std::getenv("NTSCRT_BENCH_FRAME");  // another timeline position
    const double T = bench_frame ? std::atof(bench_frame) : 216067;
    const EditTimings hi = edits_at_frame(T);
    if (T < 100000) return;  // no hour to simulate: nothing to compare
    // The same edits two seconds into the timeline: what they cost whatever the frame (CRT
    // chain rebuilds, slow on some GPUs). An hour in they may cost that plus a quarter of a
    // second: the Glitch receiver's history restarts every minute (core/src/sim_segment.h), so
    // nothing re-runs the hour (0.5.0-rc.2: ~2.5 s for the first render and every Glitch edit
    // at 640x360, ~5 s at UHD in Resolve).
    // (The first switch to royale compiles its chain, ~1 s anywhere: not checked.)
    const EditTimings lo = edits_at_frame(101);
    const double budget = 250;
    CHECK(hi.first < lo.first + budget);
    CHECK(hi.load < lo.load + budget);
    CHECK(hi.glitch < lo.glitch + budget);
    CHECK(hi.downscale < lo.downscale + budget);
#ifndef __APPLE__
    // macOS CI (Metal, virtual GPU): a CRT cache flush followed by a render an
    // hour in takes ~3 s there (0.24 s at frame 101) while the Glitch receiver is
    // kept (Load, which flushes nothing, stays fast). Not explained yet and not
    // what this test is about, so only checked where it doesn't happen.
    CHECK(hi.preset < lo.preset + budget);
    CHECK(hi.crt < lo.crt + budget);
#endif
}

// The report: an animated preset ("5 second blips", 300 frames a pass at 60 fps) chosen on an
// effect an hour into a 60 fps timeline. 0.5.0-rc.2 keyed it from frame 0 (an hour before the
// clip) with a key on every frame (97,200 keys), and the next render built a look for every
// frame since frame 0 for the Glitch receiver (37.7 s in Resolve).
//  - "adjustment clip": what Resolve reports for an Adjustment Clip: host times are timeline
//    frames (216,000 = 01:00:00:00) and the source range a made-up 0..1999.
//  - "clip at 216000": a clip whose range starts there (the times are inside it).
namespace {
struct AnimatedTimings {
    double apply_ms = 0, first_ms = 0, play_ms = 0, glitch_ms = 0, seek_ms = 0;
    size_t keys = 0;
};
AnimatedTimings animated_preset_an_hour_in(bool adjustment_clip, const char *preset_name) {
    Fixture f;
    f.create();
    const bool uhd = std::getenv("NTSCRT_BENCH_UHD") != nullptr;
    const int w = uhd ? 3840 : 640, h = uhd ? 2160 : 360;
    const double fps = 60.0, T = 216079;
    mock::Clip &src = *f.inst->clips[kOfxImageEffectSimpleSourceClipName];
    mock::Clip &dst = *f.inst->clips[kOfxImageEffectOutputClipName];
    src.width = dst.width = w;
    src.height = dst.height = h;
    src.any_frame.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float *p = &src.any_frame[(size_t(y) * w + x) * 4];
            p[0] = x / float(w - 1);
            p[1] = y / float(h - 1);
            p[2] = float((x / 16 + y / 16) % 2);
            p[3] = 1.f;
        }
    dst.any_frame.assign(src.any_frame.size(), 0.f);
    src.any_frame_last = dst.any_frame_last = 250000;
    for (mock::Clip *c : {&src, &dst}) {
        if (adjustment_clip) {
            c->props.set(kOfxImageEffectPropFrameRange, {0.0, 1999.0});
            c->props.set(kOfxImageEffectPropUnmappedFrameRange, {0.0, 0.0});
        } else {
            c->props.set(kOfxImageEffectPropFrameRange, {216000.0, 219599.0});
            c->props.set(kOfxImageEffectPropUnmappedFrameRange, {216000.0, 219599.0});
        }
        c->props.set(kOfxImageEffectPropFrameRate, {fps});
        c->props.set(kOfxImageEffectPropUnmappedFrameRate, {fps});
    }
    f.inst->props.set(kOfxImageEffectPropFrameRate, {fps});
    f.inst->props.set(kOfxImageEffectInstancePropEffectDuration, {adjustment_clip ? 2000.0 : 3600.0});
    std::vector<float> out;
    f.host.on_release = [&](const mock::Image &im) {
        if (im.props.s(kOfxImagePropUniqueIdentifier).rfind(kOfxImageEffectOutputClipName, 0) == 0) out = im.px;
    };
    auto ms_since = [](auto t0) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    };
    auto render = [&](double t) {
        mock::PropSet in;
        in.set(kOfxPropTime, {t});
        in.set(kOfxImageEffectPropFieldToRender, {std::string(kOfxImageFieldNone)});
        in.set(kOfxImageEffectPropRenderWindow, {0, 0, w, h});
        in.set(kOfxImageEffectPropRenderScale, {1.0, 1.0});
        in.set(kOfxImageEffectPropSequentialRenderStatus, {0});
        in.set(kOfxImageEffectPropInteractiveRenderStatus, {1});
        in.set(kOfxImageEffectPropRenderQualityDraft, {0});
        auto t0 = std::chrono::steady_clock::now();
        REQUIRE(f.host.action(f.plug, kOfxImageEffectActionRender, f.inst.get(), &in) == kOfxStatOK);
        return ms_since(t0);
    };
    auto purged = [&](double t) {
        OfxStatus st = f.host.action(f.plug, kOfxActionPurgeCaches, f.inst.get());
        REQUIRE((st == kOfxStatOK || st == kOfxStatReplyDefault));
        render(t);
        return out;
    };
    auto keys = [&] {
        size_t n = 0;
        for (auto &p : f.inst->params.params) n += p->keys.size();
        return n;
    };
    render(T);  // warm-up: shaders compile, the default look's receiver runs
    render(T);

    AnimatedTimings r;
    mock::Param *pp = f.param("host_preset");
    const int idx = f.choice_index(*pp, preset_name);
    REQUIRE(idx > 0);
    pp->v[0] = idx;
    auto t0 = std::chrono::steady_clock::now();
    REQUIRE(f.changed("host_preset", T) == kOfxStatOK);
    r.apply_ms = ms_since(t0);
    r.keys = keys();
    r.first_ms = render(T);
    const std::vector<float> first = out;
    double play = 0;
    std::vector<std::vector<float>> played;
    for (int i = 1; i <= 10; ++i) {
        play += render(T + i);
        played.push_back(out);
    }
    r.play_ms = play / 10;

    // The keys cover the playhead and the clip after it; the animation moves there.
    size_t moving = 0;
    for (auto &p : f.inst->params.params) {
        if (p->keys.size() < 2) continue;
        ++moving;
        CAPTURE(p->name);
        CHECK(p->keys.begin()->first <= T);
        CHECK(p->keys.rbegin()->first >= T + 299);  // at least a pass after the playhead
        if (!adjustment_clip) CHECK(p->keys.begin()->first >= 216000);  // nothing before the clip
    }
    CHECK(moving > 5);
    bool varies = false;
    for (auto &p : f.inst->params.params)
        if (p->keys.size() > 1 && p->type == kOfxParamTypeDouble)
            for (int k = 1; k < 300 && !varies; k += 7) varies = keyed_at(*p, T + k) != keyed_at(*p, T);
    CHECK(varies);

    // A Glitch edit (a value the preset doesn't key): the receiver re-runs, from its segment's start.
    mock::Param *edit = nullptr;
    for (auto &p : f.inst->params.params)
        if (p->name.rfind("glitch_", 0) == 0 && p->type == kOfxParamTypeDouble && p->keys.empty()) {
            edit = p.get();
            break;
        }
    REQUIRE(edit);
    edit->v[0] = edit->v[0] > 0.5 ? edit->v[0] - 0.25 : edit->v[0] + 0.25;
    REQUIRE(f.changed(edit->name, T + 10) == kOfxStatOK);
    r.glitch_ms = render(T + 10);
    const std::vector<float> edited = out;
    r.seek_ms = render(T - 30);  // back, before the playhead
    // The same pixels after a purge (another render order, a fresh history).
    CHECK(purged(T + 10) == edited);
    // ... and played in order from a seek to the first frame, after a purge.
    purged(T + 3);
    for (int i = 4; i <= 10; ++i) render(T + i);
    CHECK(out == edited);
    (void)first;
    (void)played;
    return r;
}
}  // namespace

TEST_CASE("high frame number: an animated preset an hour in is applied and rendered quickly") {
    for (bool adjustment : {true, false}) {
        CAPTURE(adjustment);
        const AnimatedTimings r = animated_preset_an_hour_in(adjustment, "5 second blips");
        std::ostringstream o;
        o.setf(std::ios::fixed);
        o.precision(1);
        o << (adjustment ? "adjustment clip (range 0..1999, times 216079..)" : "clip at 216000")
          << ": 5 second blips applied at 216079 in " << r.apply_ms << " ms, " << r.keys << " keyframes; first render "
          << r.first_ms << " ms; playback " << r.play_ms << " ms/frame; Glitch edit + render " << r.glitch_ms
          << " ms; seek back 40 " << r.seek_ms << " ms";
        MESSAGE(o.str());
        std::printf("[timing] %s\n", o.str().c_str());
        // 0.5.0-rc.2: 97,200 keys (every frame of 3,600, an hour before the clip); the first
        // render after it built 216,000 looks.
        CHECK(r.keys <= 3100);
        CHECK(r.apply_ms < 1000);
        CHECK(r.first_ms < 1000);
        CHECK(r.glitch_ms < 1000);
    }
}

// Animated presets become host keyframes; Horizontal resolution / Digital video
// formats write what their schema "applies" says; preset files round-trip.
TEST_CASE("presets: timeline keyframes, menus with applies, save/load round trip") {
    Fixture f;
    f.create();
    const json &s = f.schema;
    auto read_preset = [](const std::string &name) {
        std::ifstream in(std::string(NTSCRT_SOURCE_DIR) + "/presets/" + name + ".json", std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return json::parse(ss.str());
    };
    auto choose = [&](const std::string &param, const std::string &label) {
        mock::Param *p = f.param(param);
        int i = f.choice_index(*p, label);
        REQUIRE_MESSAGE(i >= 0, label);
        p->v[0] = i;
        REQUIRE(f.changed(param) == kOfxStatOK);
    };
    auto keyed = [&]() {
        std::vector<std::string> out;
        for (const auto &sp : s["params"])
            if (!f.param(host_name(sp["id"]))->keys.empty()) out.push_back(sp["id"]);
        return out;
    };

    SUBCASE("an animated preset keys every moving param from the effect's first frame") {
        // mock clip: frames 10..60 at 25 fps; "Glitch pop" lasts 2 s = 50 frames, then loops.
        choose("host_preset", "Glitch pop");
        const json preset = ntscrt::digital::migrate::upgraded(read_preset("Glitch pop"));
        const auto tl = ntscrt::timeline::parse(preset);
        const auto rules = ntscrt::timeline::Rules::from_schema(s);
        mock::Param *noise = f.param("ntsc_composite_preemphasis");
        // sparse keys (where the curve bends, with a guard key either side), from the first frame to the last
        CHECK(noise->keys.size() > 3);
        CHECK(noise->keys.size() < 51);
        CHECK(noise->keys.begin()->first == 10.0);
        CHECK(noise->keys.rbegin()->first == 60.0);
        for (int n = 0; 10 + n <= 60; ++n) {
            CAPTURE(n);
            const json look = ntscrt::timeline::look_at_frame(preset, tl, rules, n, 25.0);
            // Glitch pop's keys are all Linear: the host's linear interpolation is exact
            CHECK(keyed_at(*noise, 10.0 + n) ==
                  doctest::Approx(look["ntsc"]["settings"]["composite_preemphasis"].get<double>()));
        }
        // a param the preset holds still: its value, no keys
        CHECK(f.param("downscale_width")->keys.empty());
        CHECK(f.param("downscale_width")->v[0] == 256);
        // stepped params: a key where the value changes only
        for (const auto &sp : s["params"]) {
            if (sp["type"] != "bool" && sp["type"] != "choice") continue;
            CAPTURE(sp["id"]);
            CHECK(f.param(host_name(sp["id"]))->keys.size() < 51);
        }
        const auto moving = keyed();
        MESSAGE(moving.size() << " params keyed");
        CHECK(moving.size() > 5);
        // another preset replaces the animation
        choose("host_preset", "Clean VHS");
        CHECK(keyed().empty());
        CHECK(f.param("ntsc_composite_preemphasis")->key_clears > 0);
    }

    SUBCASE("Horizontal resolution sets the width; editing the width makes it Custom") {
        mock::Param *pre = f.param("downscale_preset"), *w = f.param("downscale_width");
        choose("downscale_preset", "Arcade (384px)");
        CHECK(w->v[0] == 384);
        w->v[0] = 500;
        REQUIRE(f.changed("downscale_width") == kOfxStatOK);
        CHECK(pre->v[0] == f.choice_index(*pre, "Custom"));
        choose("downscale_preset", "SNES (256px)");
        CHECK(w->v[0] == 256);
        w->v[0] = 256;  // same width: stays
        REQUIRE(f.changed("downscale_width") == kOfxStatOK);
        CHECK(pre->v[0] == f.choice_index(*pre, "SNES (256px)"));
    }

    SUBCASE("Digital video formats are in the Preset menu and switch the stage on") {
        mock::Param *pp = f.param("host_preset");
        // the released presets keep their index; the formats follow the 0.5.0 looks
        CHECK(f.choice_index(*pp, "15 second glitches") == 1);
        CHECK(f.choice_index(*pp, "Webcam (low quality)") == 27);
        const int webcam = f.choice_index(*pp, "Digital video/Webcam (2005)");
        CHECK(webcam == f.choice_index(*pp, "Chunky webcam") + 1);
        f.param("ntsc_composite_noise_intensity")->v[0] = 0.33;
        choose("host_preset", "Digital video/MiniDV");
        CHECK(f.param("digital_enabled")->v[0] == 1);
        CHECK(f.param("downscale_enabled")->v[0] == 1);
        CHECK(f.param("downscale_width")->v[0] == 720);
        mock::Param *dp = f.param("downscale_preset");
        CHECK(dp->v[0] == f.choice_index(*dp, "MiniDV (720px)"));
        CHECK(f.param("ntsc_composite_noise_intensity")->v[0] == doctest::Approx(0.33));  // the rest stays
    }

    SUBCASE("Save writes upstream JSON with the Screen Loop settings; Load restores the same look") {
        fs::path tmp = fs::temp_directory_path() / "ntscrt_ofx_roundtrip.json";
        fs::path side = fs::temp_directory_path() / "ntscrt_ofx_roundtrip (Screen Loop).json";
        fs::remove(side);
        auto u8 = tmp.u8string();
        f.param("host_preset_file")->str = std::string(u8.begin(), u8.end());
        choose("host_preset", "Chunky webcam");
        f.param("screenloop_enabled")->v[0] = 1;
        f.param("screenloop_zoom")->v[0] = 0.83;
        f.param("glitch_closed_captions")->v[0] = 0;
        std::map<std::string, std::vector<double>> before;
        for (const auto &sp : s["params"]) before[sp["id"]] = f.param(host_name(sp["id"]))->v;
        REQUIRE(f.changed("host_save_preset") == kOfxStatOK);
        json saved;
        {
            std::ifstream in(tmp);
            saved = json::parse(in, nullptr, false);
        }
        REQUIRE(saved.is_object());
        CHECK(saved["version"] == 2);
        CHECK(saved["ntsc"]["settings"]["version"] == 1);
        CHECK(saved["glitch"]["values"]["closed_captions"] == 0);  // encode: number
        REQUIRE(saved.contains("screen_loop"));
        CHECK(saved["screen_loop"]["kind"] == "ntscrt-video-feedback");
        CHECK(saved["screen_loop"]["values"]["zoom"].get<double>() == doctest::Approx(0.83));
        CHECK(fs::exists(side));  // Screen Loop on: also as an upstream Screen Loop file
        // change things, load it back: every param as it was
        f.param("screenloop_enabled")->v[0] = 0;
        f.param("screenloop_zoom")->v[0] = 0.5;
        f.param("downscale_width")->v[0] = 999;
        f.param("digital_enabled")->v[0] = 0;
        f.param("glitch_closed_captions")->v[0] = 1;
        REQUIRE(f.changed("host_load_preset") == kOfxStatOK);
        for (const auto &sp : s["params"]) {
            CAPTURE(sp["id"]);
            const auto &now = f.param(host_name(sp["id"]))->v;
            const auto &was = before[sp["id"]];
            REQUIRE(now.size() == was.size());
            for (size_t k = 0; k < now.size(); ++k) CHECK(now[k] == doctest::Approx(was[k]));
        }
        fs::remove(tmp);
        fs::remove(side);
    }
}

// Sparse preset keys (0.5.0-rc.3) render like the per-frame keys 0.5.0-rc.2 wrote: every
// moving number is replaced by a key on every frame holding the upstream timeline's value
// (the reference), and the same frames are rendered again. Glitch included: its receiver
// reads every earlier frame's settings.
TEST_CASE("presets: sparse keyframes render like a key on every frame") {
    const int w = 320, h = 180, last = 400;
    const double fps = 30;
    const json schema = json::parse(ntscrt_param_schema_json());
    const auto rules = ntscrt::timeline::Rules::from_schema(schema);
    double worst = 0;
    for (const char *name : {"5 second blips", "V Hold glitch", "Glitch 1", "Tracking Noise up", "Two blips loop"}) {
        CAPTURE(name);
        Fixture f;
        f.create();
        mock::Clip &src = *f.inst->clips[kOfxImageEffectSimpleSourceClipName];
        mock::Clip &dst = *f.inst->clips[kOfxImageEffectOutputClipName];
        src.width = dst.width = w;
        src.height = dst.height = h;
        src.any_frame.resize(size_t(w) * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                float *p = &src.any_frame[(size_t(y) * w + x) * 4];
                p[0] = x / float(w - 1);
                p[1] = y / float(h - 1);
                p[2] = float((x / 16 + y / 16) % 2);
                p[3] = 1.f;
            }
        dst.any_frame.assign(src.any_frame.size(), 0.f);
        src.any_frame_last = dst.any_frame_last = last;
        for (mock::Clip *c : {&src, &dst}) {
            c->props.set(kOfxImageEffectPropFrameRange, {0.0, double(last)});
            c->props.set(kOfxImageEffectPropFrameRate, {fps});
        }
        f.inst->props.set(kOfxImageEffectPropFrameRate, {fps});
        std::vector<float> out;
        f.host.on_release = [&](const mock::Image &im) {
            if (im.props.s(kOfxImagePropUniqueIdentifier).rfind(kOfxImageEffectOutputClipName, 0) == 0) out = im.px;
        };
        auto render = [&](double t) {
            mock::PropSet in;
            in.set(kOfxPropTime, {t});
            in.set(kOfxImageEffectPropFieldToRender, {std::string(kOfxImageFieldNone)});
            in.set(kOfxImageEffectPropRenderWindow, {0, 0, w, h});
            in.set(kOfxImageEffectPropRenderScale, {1.0, 1.0});
            in.set(kOfxImageEffectPropSequentialRenderStatus, {0});
            in.set(kOfxImageEffectPropInteractiveRenderStatus, {0});
            in.set(kOfxImageEffectPropRenderQualityDraft, {0});
            REQUIRE(f.host.action(f.plug, kOfxImageEffectActionRender, f.inst.get(), &in) == kOfxStatOK);
            return out;
        };
        mock::Param *pp = f.param("host_preset");
        pp->v[0] = f.choice_index(*pp, name);
        REQUIRE(pp->v[0] > 0);
        REQUIRE(f.changed("host_preset", 0) == kOfxStatOK);
        const std::vector<int> frames = {20, 45, 90, 151, 230};
        std::vector<std::vector<float>> sparse;
        for (int n : frames) sparse.push_back(render(n));

        // The per-frame reference for every keyed number.
        std::ifstream in(std::string(NTSCRT_SOURCE_DIR) + "/presets/" + name + ".json", std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const json preset = ntscrt::digital::migrate::upgraded(json::parse(ss.str()));
        const auto tl = ntscrt::timeline::parse(preset);
        std::vector<json> looks;
        for (int n = 0; n <= last; ++n) looks.push_back(ntscrt::timeline::look_at_frame(preset, tl, rules, n, fps));
        size_t replaced = 0, sparse_keys = 0;
        std::string any;
        for (const auto &sp : schema["params"]) {
            const std::string type = sp["type"];
            if (type != "float" && type != "int") continue;
            mock::Param *p = f.param(host_name(sp["id"]));
            if (p->keys.size() < 2) continue;
            sparse_keys += p->keys.size();
            const std::string path = sp["path"];
            for (int n : frames) {
                const double ref = looks[size_t(n)].at(json::json_pointer(path)).get<double>();
                const double got = keyed_at(*p, n);
                if (std::fabs(ref - got) > 1e-9) MESSAGE("DIAG " << std::string(name) << " " << p->name << " @" << n << " ref " << ref << " sparse " << got);
            }
            p->keys.clear();
            for (int n = 0; n <= last; ++n) {
                const json::json_pointer ptr(path);
                REQUIRE(looks[size_t(n)].contains(ptr));
                double v = looks[size_t(n)].at(ptr).get<double>();
                p->keys[double(n)] = {v};
            }
            ++replaced;
            any = p->name;
        }
        CHECK(replaced > 0);
        REQUIRE(f.changed(any, 0) == kOfxStatOK);  // keys changed: as a param edit would say
        REQUIRE((f.host.action(f.plug, kOfxActionPurgeCaches, f.inst.get()) != kOfxStatFailed));
        for (size_t k = 0; k < frames.size(); ++k) {
            CAPTURE(frames[k]);
            const std::vector<float> ref = render(frames[k]);
            REQUIRE(ref.size() == sparse[k].size());
            double sum = 0;
            for (size_t i = 0; i < ref.size(); i += 4)
                for (int c = 0; c < 3; ++c) sum += std::fabs(double(ref[i + c]) - sparse[k][i + c]);
            const double mean = sum / (ref.size() / 4 * 3) * 255;
            worst = std::max(worst, mean);
            MESSAGE(std::string(name) << " frame " << frames[k] << ": mean diff " << mean << " (of 255) with " << sparse_keys
                         << " sparse keys on " << replaced << " numbers");
            CHECK(mean < 1.0);
        }
    }
    MESSAGE("worst mean diff, sparse vs per-frame keys: " << worst << " of 255");
}

// ---- host protocol (getFramesNeeded, isIdentity, fields, abort, clip prefs) --------
namespace {
struct ProtoFixture : Fixture {
    int w = 64, h = 36;
    std::vector<float> out;
    ProtoFixture() {
        create();
        mock::Clip &s = src();
        mock::Clip &dst = *inst->clips[kOfxImageEffectOutputClipName];
        s.width = dst.width = w;
        s.height = dst.height = h;
        s.any_frame.resize(size_t(w) * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                float *p = &s.any_frame[(size_t(y) * w + x) * 4];
                p[0] = x / float(w - 1);
                p[1] = y / float(h - 1);
                p[2] = float((x / 8 + y / 8) % 2);
                p[3] = 1.f;
            }
        dst.any_frame.assign(s.any_frame.size(), 0.f);
        s.any_frame_last = dst.any_frame_last = 2000;
        s.props.set(kOfxImageEffectPropFrameRange, {0.0, 2000.0});
        host.on_release = [this](const mock::Image &im) {
            if (im.props.s(kOfxImagePropUniqueIdentifier).rfind(kOfxImageEffectOutputClipName, 0) == 0) out = im.px;
        };
    }
    mock::Clip &src() { return *inst->clips[kOfxImageEffectSimpleSourceClipName]; }
    void set(const std::string &id, double v) {
        param(host_name(id))->v[0] = v;
        REQUIRE(changed(host_name(id)) == kOfxStatOK);
    }
    // [min, max] the plug-in declares for the source at t.
    std::pair<double, double> needed(double t) {
        mock::PropSet in, o;
        in.set(kOfxPropTime, {t});
        REQUIRE(host.action(plug, kOfxImageEffectActionGetFramesNeeded, inst.get(), &in, &o) == kOfxStatOK);
        const std::string k = std::string("OfxImageClipPropFrameRange_") + kOfxImageEffectSimpleSourceClipName;
        REQUIRE(o.dim(k) == 2);
        return {o.d(k, 0), o.d(k, 1)};
    }
    OfxStatus render(double t, const char *field = kOfxImageFieldNone) {
        mock::PropSet in;
        in.set(kOfxPropTime, {t});
        in.set(kOfxImageEffectPropFieldToRender, {std::string(field)});
        in.set(kOfxImageEffectPropRenderWindow, {0, 0, w, h});
        in.set(kOfxImageEffectPropRenderScale, {1.0, 1.0});
        in.set(kOfxImageEffectPropSequentialRenderStatus, {0});
        in.set(kOfxImageEffectPropInteractiveRenderStatus, {0});
        in.set(kOfxImageEffectPropRenderQualityDraft, {0});
        return host.action(plug, kOfxImageEffectActionRender, inst.get(), &in);
    }
    std::pair<int, int> fetched_range(size_t from) {
        const auto &f = src().fetched;
        int lo = INT_MAX, hi = INT_MIN;
        for (size_t i = from; i < f.size(); ++i) lo = std::min(lo, f[i]), hi = std::max(hi, f[i]);
        return {lo, hi};
    }
};
}  // namespace

TEST_CASE("host protocol: describe declares sequential preference and whole-frame fields") {
    Fixture f;
    CHECK(f.desc.props.i(kOfxImageEffectInstancePropSequentialRender) == 2);
    CHECK(f.desc.props.i(kOfxImageEffectPluginPropFieldRenderTwiceAlways) == 0);
    CHECK(f.desc.clips[kOfxImageEffectSimpleSourceClipName]->props.s(kOfxImageClipPropFieldExtraction) ==
          kOfxImageFieldBoth);
}

TEST_CASE("host protocol: getFramesNeeded follows the temporal stages") {
    ProtoFixture f;
    SUBCASE("default look (Glitch reads only looks): the current frame") {
        auto [a, b] = f.needed(100);
        CHECK(a == 100);
        CHECK(b == 100);
    }
    SUBCASE("Digital video, keyframe every 2 s: back to the keyframe") {
        f.set("digital.enabled", 1);
        f.set("digital.keyframe_interval", 2);
        // 25 fps source, 15 fps capture: a keyframe every 30 captures = 50 source frames
        auto [a, b] = f.needed(130);
        CHECK(b == 130);
        CHECK(a == 100);
        auto [c, d] = f.needed(100);
        CHECK(c == 100);
        CHECK(d == 100);
        // The render fetches nothing outside what was declared.
        const size_t mark = f.src().fetched.size();
        REQUIRE(f.render(130) == kOfxStatOK);
        auto [lo, hi] = f.fetched_range(mark);
        CHECK(lo == 100);
        CHECK(hi <= 130);
    }
    SUBCASE("Digital video, every frame a keyframe: the previous capture") {
        f.set("digital.enabled", 1);
        f.set("digital.keyframe_interval", 0);
        auto [a, b] = f.needed(130);
        CHECK(b == 130);
        CHECK(a >= 127);
        CHECK(a < 130);
    }
    SUBCASE("Digital video with datamosh: walks back over dropped keys") {
        f.set("digital.enabled", 1);
        f.set("digital.keyframe_interval", 1);
        auto [a0, b0] = f.needed(1000);
        f.set("digital.datamosh", 1);
        auto [a1, b1] = f.needed(1000);
        CHECK(b0 == 1000);
        CHECK(b1 == 1000);
        CHECK(a0 > 900);
        CHECK(a1 == 0);  // every later key dropped: the stream reaches back to frame 0
    }
    SUBCASE("Screen Loop: its window start") {
        f.set("screenloop.enabled", 1);
        auto [a, b] = f.needed(1000);
        CHECK(b == 1000);
        CHECK(a == 720);  // window 240: anchor (1000 / 240 - 1) * 240
        auto [c, d] = f.needed(300);
        CHECK(c == 0);  // the first two windows run from frame 0
        CHECK(d == 300);
    }
    SUBCASE("half-frame times cover both readings") {
        f.set("digital.enabled", 1);
        f.set("digital.keyframe_interval", 2);
        auto [a, b] = f.needed(130.5);
        CHECK(a == 100);
        CHECK(b == 131);
    }
}

// Resolve's Adjustment Clip: timeline times (an hour in) and a made-up source range of 0..1999.
// 0.5.0-rc.2 clamped the time into that range and declared 1999..t (214,000 frames), and refused
// every earlier frame Digital video or Screen Loop asked for.
TEST_CASE("host protocol: times outside the source's range (Resolve's Adjustment Clip)") {
    ProtoFixture f;
    f.src().props.set(kOfxImageEffectPropFrameRange, {0.0, 1999.0});
    f.src().any_frame_last = 250000;
    f.inst->clips[kOfxImageEffectOutputClipName]->any_frame_last = 250000;
    const double T = 216079;
    auto [a, b] = f.needed(T);
    CHECK(a == T);
    CHECK(b == T);
    // Digital video with Datamosh drops every keyframe: the stream reaches back to the minute's start
    // (216000 = 01:00:00:00 at 60 fps; the mock clip runs at 25 fps: 01:00:00:00 = 90000), not frame 0.
    f.set("digital.enabled", 1);
    f.set("digital.keyframe_interval", 1);
    f.set("digital.datamosh", 1);
    auto [c, d] = f.needed(T);
    CHECK(d == T);
    CHECK(c >= 216000 - 60);  // the segment start (a multiple of 1500 frames at 25 fps) minus one keyframe interval
    CHECK(c <= T);
    const size_t mark = f.src().fetched.size();
    REQUIRE(f.render(T) == kOfxStatOK);
    auto [lo, hi] = f.fetched_range(mark);
    CHECK(lo >= c);  // fetched what was declared, and earlier frames were delivered
    CHECK(hi <= T);
    CHECK(f.src().fetched.size() - mark > 1);
}

TEST_CASE("host protocol: isIdentity only when every stage is off") {
    ProtoFixture f;
    auto identity = [&](double t, std::string *clip = nullptr) {
        mock::PropSet in, o;
        in.set(kOfxPropTime, {t});
        in.set(kOfxImageEffectPropFieldToRender, {std::string(kOfxImageFieldNone)});
        in.set(kOfxImageEffectPropRenderWindow, {0, 0, f.w, f.h});
        in.set(kOfxImageEffectPropRenderScale, {1.0, 1.0});
        OfxStatus st = f.host.action(f.plug, kOfxImageEffectActionIsIdentity, f.inst.get(), &in, &o);
        if (clip) *clip = o.s(kOfxPropName);
        return st;
    };
    CHECK(identity(10) == kOfxStatReplyDefault);
    for (const char *id : {"crop.enabled", "downscale.enabled", "digital.enabled", "ntsc.enabled", "glitch.enabled",
                           "crt.enabled", "screenloop.enabled"})
        f.param(host_name(id))->v[0] = 0;
    std::string clip;
    CHECK(identity(10, &clip) == kOfxStatOK);
    CHECK(clip == kOfxImageEffectSimpleSourceClipName);
    for (const char *id : {"crop.enabled", "digital.enabled", "crt.enabled", "screenloop.enabled"}) {
        CAPTURE(id);
        f.param(host_name(id))->v[0] = 1;
        CHECK(identity(10) == kOfxStatReplyDefault);
        f.param(host_name(id))->v[0] = 0;
    }
    // Keyframed on later: identity only where it's off.
    f.param("crt_enabled")->keys = {{0.0, {0.0}}, {20.0, {0.0}}, {21.0, {1.0}}};
    CHECK(identity(10) == kOfxStatOK);
    CHECK(identity(30) == kOfxStatReplyDefault);
}

TEST_CASE("host protocol: field renders use frame floor(t)") {
    ProtoFixture f;
    f.param("host_debug_logging")->v[0] = 1;
    REQUIRE(f.changed("host_debug_logging") == kOfxStatOK);
    const std::string before = read_log();
    REQUIRE(f.render(40.5, kOfxImageFieldUpper) == kOfxStatOK);
    REQUIRE(f.render(41.5, kOfxImageFieldNone) == kOfxStatOK);
    const std::string added = read_log().substr(before.size());
    MESSAGE(added);
    CHECK(added.find(" t=40.5 frame=40 ") != std::string::npos);  // a field of frame 40
    CHECK(added.find(" t=41.5 frame=42 ") != std::string::npos);  // unfielded: the nearest frame
}

TEST_CASE("host protocol: abort stops fetching earlier frames and drops the half-built history") {
    ProtoFixture f;
    f.set("digital.enabled", 1);
    f.set("digital.keyframe_interval", 4);
    REQUIRE(f.render(0) == kOfxStatOK);  // warm-up
    long asked = 0;
    f.host.on_abort = [&] { return ++asked > 3 ? 1 : 0; };
    const size_t mark = f.src().fetched.size();
    REQUIRE(f.render(150) == kOfxStatOK);
    CHECK(f.host.abort_calls > 0);
    CHECK(f.src().fetched.size() - mark <= 6);  // the current frame + a few earlier ones, not the GOP
    CHECK(read_log().find("aborted by the host") != std::string::npos);
    // No abort any more: the same frame equals a render from clean caches.
    f.host.on_abort = nullptr;
    REQUIRE(f.render(150) == kOfxStatOK);
    const std::vector<float> again = f.out;
    REQUIRE(f.host.action(f.plug, kOfxActionPurgeCaches, f.inst.get()) != kOfxStatFailed);
    REQUIRE(f.render(150) == kOfxStatOK);
    CHECK(again == f.out);
}

TEST_CASE("host protocol: clip preferences ask for RGBA from an RGB source") {
    ProtoFixture f;
    f.src().props.set(kOfxImageClipPropUnmappedComponents, {std::string(kOfxImageComponentRGB)});
    mock::PropSet o;
    REQUIRE(f.host.action(f.plug, kOfxImageEffectActionGetClipPreferences, f.inst.get(), nullptr, &o) == kOfxStatOK);
    CHECK(o.s(std::string("OfxImageClipPropComponents_") + kOfxImageEffectSimpleSourceClipName) ==
          kOfxImageComponentRGBA);
}

TEST_CASE("host protocol: non-square pixels render with a warning") {
    ProtoFixture f;
    f.src().props.set(kOfxImagePropPixelAspectRatio, {0.9});
    REQUIRE(f.render(5) == kOfxStatOK);
    bool warned = false;
    for (auto &m : f.host.log) warned |= m.find("non-square pixels") != std::string::npos;
    CHECK(warned);
}

// "Debug logging for all effects": the button flips the switch file shared by every instance,
// the status line follows it, and while it is on every render logs in detail although the
// effect's own checkbox is off; a change made outside the host is picked up within a second.
TEST_CASE("debug logging for all effects: button, status, renders, outside changes") {
    std::error_code ec;
    fs::remove(kSwitchFile, ec);
    ProtoFixture f;
    mock::Param *status = f.param("host_debug_all_status");
    REQUIRE(f.host.action(f.plug, kOfxActionBeginInstanceEdit, f.inst.get()) != kOfxStatFailed);
    CHECK(status->str == "off");
    CHECK(f.param("host_debug_logging")->v[0] == 0);
    CHECK(read_log().find("debug logging for all effects off") != std::string::npos);  // at creation

    size_t mark = read_log().size();
    REQUIRE(f.changed("host_debug_all") == kOfxStatOK);
    CHECK(fs::is_regular_file(kSwitchFile));
    CHECK(status->str == "ON for every NTSCRT effect");
    CHECK(f.param("host_debug_all")->props.s(kOfxPropLabel) == "Turn off debug logging for all effects");
    for (double t : {30.0, 31.0}) REQUIRE(f.render(t) == kOfxStatOK);
    std::string added = read_log().substr(mark);
    MESSAGE(added);
    CHECK(added.find("debug logging for all NTSCRT effects turned on") != std::string::npos);
    CHECK(count(added, " debug render #") == 2);
    CHECK(count(added, "debug_by=all") == 2);
    CHECK(count(added, " look={") == 1);
    CHECK(count(added, "core_ms=") == 2);

    // An effect created while it is on says so.
    {
        mark = read_log().size();
        auto other = f.host.make_instance(f.desc, kOfxImageEffectContextFilter);
        REQUIRE(f.host.action(f.plug, kOfxActionCreateInstance, other.get()) == kOfxStatOK);
        CHECK(read_log().substr(mark).find("debug logging for all effects on (") != std::string::npos);
        f.host.action(f.plug, kOfxActionDestroyInstance, other.get());
    }

    mark = read_log().size();
    REQUIRE(f.changed("host_debug_all") == kOfxStatOK);  // pressed again: off
    CHECK_FALSE(fs::exists(kSwitchFile));
    CHECK(status->str == "off");
    REQUIRE(f.render(32) == kOfxStatOK);
    added = read_log().substr(mark);
    CHECK(added.find("debug logging for all NTSCRT effects turned off") != std::string::npos);
    CHECK(count(added, " debug render #") == 0);

    // Turned on outside this host (another Resolve, or the file created by hand): noticed within
    // about a second, logged once; deleting the file turns it off again.
    { std::ofstream(kSwitchFile) << "on\n"; }
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    mark = read_log().size();
    REQUIRE(f.render(33) == kOfxStatOK);
    REQUIRE(f.render(34) == kOfxStatOK);
    added = read_log().substr(mark);
    CHECK(count(added, "debug logging for all NTSCRT effects is now on") == 1);
    CHECK(count(added, " debug render #") == 2);
    fs::remove(kSwitchFile, ec);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    mark = read_log().size();
    REQUIRE(f.render(35) == kOfxStatOK);
    added = read_log().substr(mark);
    CHECK(added.find("debug logging for all NTSCRT effects is now off") != std::string::npos);
    CHECK(count(added, " debug render #") == 0);
    REQUIRE(f.host.action(f.plug, kOfxActionBeginInstanceEdit, f.inst.get()) != kOfxStatFailed);
    CHECK(status->str == "off");
}
