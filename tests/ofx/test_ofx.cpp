// Loads the built NTSCRT.ofx through a mock OpenFX host and checks describe,
// params vs schema, presets, preset files and a pass-through render.
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <ntscrt/ntscrt.h>

#include "mock_host.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

using nlohmann::json;
namespace fs = std::filesystem;

namespace {
// Before the plug-in loads (it reads these once): keep the test runs out of the user's
// log and never open a file-manager window from "Open logs folder".
const fs::path kLogFile = [] {
    fs::path p = fs::temp_directory_path() / "ntscrt_ofx_test.log";
    std::error_code ec;
    fs::remove(p, ec);
    auto u = p.u8string();
    std::string s(u.begin(), u.end());
#ifdef _WIN32
    _putenv_s("NTSCRT_OFX_LOG", s.c_str());
    _putenv_s("NTSCRT_OFX_NO_OPEN", "1");
#else
    setenv("NTSCRT_OFX_LOG", s.c_str(), 1);
    setenv("NTSCRT_OFX_NO_OPEN", "1", 1);
#endif
    return p;
}();
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
        REQUIRE_MESSAGE(host.load(NTSCRT_OFX_BINARY, err), err);
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
