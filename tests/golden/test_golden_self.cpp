// Self-references: ntscrt_core against its own committed renders.
//
// tests/golden/self/manifest.json lists looks the upstream comparison
// (test_golden.cpp) can't cover: Digital (both codecs), Glitch over time,
// Screen Loop, alpha Clip / Spill, crop, a moving sequence, render_scale != 1
// and every bundled preset at one frame. Each compared frame has a baseline
// tests/golden/self/ref/<id>-f<frame>.png; a per-OS baseline in
// self/ref-windows/ or self/ref-macos/ wins when present (for entries whose
// GPU math differs between D3D11 and Metal beyond the tolerance).
//
// These are regression baselines, not truth: after an intended look change,
// regenerate them with
//     cmake --build <build> --config Release --target golden-update
// (= NTSCRT_GOLDEN_UPDATE=1 test_golden -tc="golden self*"), look at the git
// diff of the PNGs, and commit. NTSCRT_GOLDEN_UPDATE=os writes the per-OS dir.
//
// Environment: NTSCRT_GOLDEN_STRICT=1 fails on a missing baseline,
// NTSCRT_GOLDEN_ONLY=<id> runs one entry, NTSCRT_GOLDEN_OUT_DIR gets
// <name>.png (actual, same name as the baseline) + <name>.diff.png for each
// failure, so `gh run download -n golden-mismatch-<os> -D tests/golden/self/ref-<os>`
// turns a CI mismatch into per-OS baselines.
#include "golden_common.h"

#include <doctest/doctest.h>
#include <ntscrt/ntscrt.h>

#include <cstdio>
#include <map>
#include <memory>
#include <set>

#include "testing.h"

#ifndef NTSCRT_GOLDEN_RESOURCE_DIR
#define NTSCRT_GOLDEN_RESOURCE_DIR NTSCRT_SOURCE_DIR
#endif
#ifndef NTSCRT_GOLDEN_OUT_DIR
#define NTSCRT_GOLDEN_OUT_DIR "golden-out"
#endif

using namespace golden;

namespace {
const fs::path kRoot = fs::u8path(NTSCRT_SOURCE_DIR);
const fs::path kSelf = kRoot / "tests" / "golden" / "self";

#ifdef _WIN32
const char *kOs = "windows";
#elif defined(__APPLE__)
const char *kOs = "macos";
#else
const char *kOs = "linux";
#endif

fs::path assets_dir() {
    return fs::u8path(env_or("NTSCRT_TESTASSETS_DIR", (kRoot / "upstream/NTSCRT/TestAssets").string()));
}

// Box downscale by an integer factor (what a host hands the effect at a proxy scale).
Image box_down(const Image &in, int k) {
    if (k <= 1) return in;
    Image o{in.w / k, in.h / k, {}};
    o.px.resize(size_t(o.w) * o.h * 4);
    for (int y = 0; y < o.h; ++y)
        for (int x = 0; x < o.w; ++x)
            for (int c = 0; c < 4; ++c) {
                int s = 0;
                for (int j = 0; j < k; ++j)
                    for (int i = 0; i < k; ++i) s += in.px[((size_t(y) * k + j) * in.w + x * k + i) * 4 + c];
                o.px[(size_t(y) * o.w + x) * 4 + c] = uint8_t((s + k * k / 2) / (k * k));
            }
    return o;
}

// The entry's source clip: the input image, scrolled by `motion` px per frame,
// optionally with a soft disc of straight (or premultiplied) alpha.
struct Source {
    Image base;
    int dx = 0, dy = 0;
    bool disc = false, premult = false;
    std::map<const ntscrt_image *, std::pair<std::unique_ptr<ntscrt_image>, std::vector<uint8_t>>> live;

    std::vector<uint8_t> frame(int64_t f) const {
        const int w = base.w, h = base.h;
        std::vector<uint8_t> px(base.px.size());
        const int ox = int(((dx * f) % w + w) % w), oy = int(((dy * f) % h + h) % h);
        const double r = 0.36 * std::min(w, h), soft = 6.0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const uint8_t *s = &base.px[(size_t((y + oy) % h) * w + (x + ox) % w) * 4];
                uint8_t *d = &px[(size_t(y) * w + x) * 4];
                std::copy(s, s + 4, d);
                if (!disc) continue;
                const double dist = std::hypot(x + 0.5 - w / 2.0, y + 0.5 - h / 2.0);
                const double a = std::clamp((r - dist) / soft + 0.5, 0.0, 1.0);
                d[3] = uint8_t(std::lround(a * 255));
                if (premult)
                    for (int c = 0; c < 3; ++c) d[c] = uint8_t(std::lround(d[c] * d[3] / 255.0));
            }
        return px;
    }
    static const ntscrt_image *fetch(void *u, int64_t f) {
        auto *s = static_cast<Source *>(u);
        if (f < 0) return nullptr;
        auto px = s->frame(f);
        auto im = std::make_unique<ntscrt_image>(
            ntscrt_image{s->base.w, s->base.h, ptrdiff_t(s->base.w) * 4, NTSCRT_RGBA8, nullptr});
        auto &slot = s->live[im.get()];
        slot.second = std::move(px);
        im->data = slot.second.data();
        const ntscrt_image *k = im.get();
        slot.first = std::move(im);
        return k;
    }
    static void release(void *u, const ntscrt_image *im) { static_cast<Source *>(u)->live.erase(im); }
};

std::optional<json> load_preset(const fs::path &p, std::string *err) {
    auto j = read_json(p);
    if (!j && err) *err = "cannot read " + p.string();
    return j;
}

// Look and feedback JSON for an entry: presets/<preset>.json (or {}) with the
// entry's "look" merged over it (RFC 7386); "screen_loop" names a
// presets/Screen Loop/ file, "feedback" an inline object merged over it.
bool entry_looks(const json &e, json &look, json &feedback, std::string *err) {
    look = json::object();
    if (e.contains("preset")) {
        auto p = load_preset(kRoot / "presets" / fs::u8path(e["preset"].get<std::string>() + ".json"), err);
        if (!p) return false;
        look = *p;
    }
    if (e.contains("look")) look.merge_patch(e["look"]);
    feedback = json();
    if (e.contains("screen_loop")) {
        auto p = load_preset(kRoot / "presets" / "Screen Loop" / fs::u8path(e["screen_loop"].get<std::string>() + ".json"), err);
        if (!p) return false;
        feedback = *p;
    }
    if (e.contains("feedback")) {
        if (feedback.is_null()) feedback = json::object();
        feedback.merge_patch(e["feedback"]);
    }
    return true;
}

// A tolerance from the entry (a number, or {"windows": a, "macos": b, "default": c})
// or the manifest's default_<key>.
double entry_tolerance(const json &e, const json &m, const char *key) {
    double v = m.value(std::string("default_") + key, 0.0);
    if (e.contains(key)) {
        const json &t = e[key];
        if (t.is_number()) v = t.get<double>();
        else if (t.is_object()) v = t.value(kOs, t.value("default", v));
    }
    return v;
}

bool all_opaque(const Image &im) {
    for (size_t i = 3; i < im.px.size(); i += 4)
        if (im.px[i] != 255) return false;
    return true;
}

// RGB PNG when opaque (a quarter smaller), RGBA otherwise; max zlib effort.
bool write_ref(const fs::path &p, const Image &im) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    stbi_write_png_compression_level = 9;
    if (!all_opaque(im)) return stbi_write_png(p.string().c_str(), im.w, im.h, 4, im.px.data(), im.w * 4) != 0;
    std::vector<uint8_t> rgb(size_t(im.w) * im.h * 3);
    for (size_t i = 0, n = size_t(im.w) * im.h; i < n; ++i)
        for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = im.px[i * 4 + c];
    return stbi_write_png(p.string().c_str(), im.w, im.h, 3, rgb.data(), im.w * 3) != 0;
}
}  // namespace

TEST_CASE("golden self: manifest is well-formed") {
    auto m = read_json(kSelf / "manifest.json");
    REQUIRE_MESSAGE(m.has_value(), "cannot parse ", (kSelf / "manifest.json").string());
    REQUIRE((*m)["entries"].is_array());
    std::set<std::string> ids;
    for (const auto &e : (*m)["entries"]) {
        INFO("entry ", e.dump());
        REQUIRE(e.contains("id"));
        CHECK(ids.insert(e["id"].get<std::string>()).second);
        CHECK(e.contains("input"));
        REQUIRE(e.contains("frames"));
        CHECK(e["frames"].is_array());
        CHECK(!e["frames"].empty());
        json look, fb;
        std::string err;
        CHECK_MESSAGE(entry_looks(e, look, fb, &err), err);
    }
}

TEST_CASE("golden self: ntscrt_core matches its committed baselines") {
    auto m = read_json(kSelf / "manifest.json");
    REQUIRE(m.has_value());
    const bool strict = env_or("NTSCRT_GOLDEN_STRICT", "") == "1";
    const std::string update = env_or("NTSCRT_GOLDEN_UPDATE", "");
    const std::string only = env_or("NTSCRT_GOLDEN_ONLY", "");
    const fs::path out_dir = fs::u8path(env_or("NTSCRT_GOLDEN_OUT_DIR", NTSCRT_GOLDEN_OUT_DIR)) / "self";
    const fs::path shared_dir = kSelf / "ref", os_dir = kSelf / (std::string("ref-") + kOs);

    const std::string res = env_or("NTSCRT_RESOURCE_DIR", NTSCRT_GOLDEN_RESOURCE_DIR);
    ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_AUTO, nullptr, nullptr, res.c_str()};
    ntscrt_instance *inst = nullptr;
    REQUIRE(ntscrt_create(&ci, &inst) == NTSCRT_OK);
    if (!ntscrt::testing::stats(inst).has_gpu) {
        // The no-GPU path renders the CRT and Screen Loop differently by design.
        const char *req = std::getenv("NTSCRT_REQUIRE_GPU");
        if (req && *req && std::string(req) != "0") FAIL("no GPU device and NTSCRT_REQUIRE_GPU is set");
        MESSAGE("no GPU device: self-references skipped");
        ntscrt_destroy(inst);
        return;
    }

    int compared = 0, skipped = 0, written = 0, failed = 0, hung = 0;
    for (const auto &e : (*m)["entries"]) {
        const std::string id = e["id"];
        if (!only.empty() && id != only) continue;
        Image input;
        const fs::path in_png = assets_dir() / fs::u8path(e["input"].get<std::string>());
        if (!load_png(in_png, input)) {
            ++skipped;
            MESSAGE("skip ", id, ": input ", in_png.string(), " missing (init upstream/NTSCRT submodule)");
            if (strict) FAIL_CHECK("missing input ", in_png.string());
            continue;
        }
        json look, fb;
        std::string err;
        REQUIRE_MESSAGE(entry_looks(e, look, fb, &err), err);
        const std::string look_s = look.dump(), fb_s = fb.is_null() ? std::string() : fb.dump();

        const double scale = e.value("render_scale", 1.0);
        Source src;
        // input_downscale: a smaller clip (keeps the baselines small); render_scale: a proxy render
        src.base = box_down(box_down(input, e.value("input_downscale", 1)), int(std::lround(1.0 / scale)));
        if (e.contains("motion")) { src.dx = e["motion"][0]; src.dy = e["motion"][1]; }
        src.disc = e.value("alpha", "") == "disc";
        src.premult = e.value("premultiplied", false);

        std::vector<int64_t> frames = e["frames"].get<std::vector<int64_t>>();
        std::sort(frames.begin(), frames.end());
        std::vector<int64_t> order = frames;
        if (e.value("sequence", false)) {  // every frame up to the last, like playback
            order.clear();
            for (int64_t f = frames.front(); f <= frames.back(); ++f) order.push_back(f);
        }
        const double thr = entry_tolerance(e, *m, "threshold");    // mean |diff|, 0..255
        const double over = entry_tolerance(e, *m, "max_over16");  // share of pixels off by > 16
        ntscrt_flush_caches(inst);
        for (int64_t f : order) {
            auto px = src.frame(f);
            ntscrt_image si{src.base.w, src.base.h, ptrdiff_t(src.base.w) * 4, NTSCRT_RGBA8, px.data()};
            Image out{src.base.w, src.base.h, std::vector<uint8_t>(px.size())};
            ntscrt_image di{out.w, out.h, ptrdiff_t(out.w) * 4, NTSCRT_RGBA8, out.px.data()};
            ntscrt_render_args ra{};
            ra.frame_index = f;
            ra.fps = e.value("fps", 30.0);
            ra.look_json = look_s.c_str();
            ra.feedback_json = fb_s.empty() ? nullptr : fb_s.c_str();
            ra.render_scale = scale;
            ra.fetch_source = &Source::fetch;
            ra.release_source = &Source::release;
            ra.user = &src;
            ra.source_premultiplied = src.premult ? 1 : 0;
            const ntscrt_status st = ntscrt_render_cpu(inst, &ra, &si, &di);
            INFO("entry ", id, " frame ", f, " look ", look_s);
            if (st != NTSCRT_OK && gpu_hang_on_gha_macos(ntscrt_last_error(inst))) {
                // GitHub's macOS VMs have a paravirtual GPU that times out ("GPU Hang") under load
                // (see tests/crt/test_crt.cpp); only there is it a warning. Fresh instance, next entry.
                MESSAGE("WARNING: ", id, " frame ", f, " hung the CI runner's virtual GPU: ",
                        std::string(ntscrt_last_error(inst)));
                ++hung;
                ntscrt_destroy(inst);
                REQUIRE(ntscrt_create(&ci, &inst) == NTSCRT_OK);
                break;
            }
            REQUIRE_MESSAGE(st == NTSCRT_OK, "render failed (", int(st), "): ", std::string(ntscrt_last_error(inst)));
            if (!std::binary_search(frames.begin(), frames.end(), f)) continue;

            const std::string name = id + "-f" + std::to_string(f) + ".png";
            if (update == "1" || update == "os") {
                const fs::path dst = (update == "os" ? os_dir : shared_dir) / name;
                REQUIRE_MESSAGE(write_ref(dst, out), "cannot write ", dst.string());
                ++written;
                continue;
            }
            fs::path ref_png = os_dir / name;
            if (!fs::exists(ref_png)) ref_png = shared_dir / name;
            Image ref;
            if (!load_png(ref_png, ref)) {
                ++skipped;
                if (strict) FAIL_CHECK("missing baseline ", ref_png.string());
                continue;
            }
            Image diff;
            // straight-alpha output: compare what shows once composited (premultiplied)
            const DiffStats s = compare(out, ref, &diff, 4, !src.premult);
            const bool ok = s.same_size && s.mean_abs <= thr && s.frac_over_16 <= over;
            ++compared;
            std::printf("[golden-self] %-44s mean=%.4f max=%3d over16=%.4f thr=%.2f/%.3f %s%s\n", name.c_str(),
                        s.mean_abs, s.max_abs, s.frac_over_16, thr, over, ok ? "ok" : "FAIL",
                        ref_png.parent_path() == os_dir ? (" (" + std::string(kOs) + " baseline)").c_str() : "");
            if (!ok) {
                ++failed;
                write_ref(out_dir / name, out);
                if (s.same_size) write_png(out_dir / (id + "-f" + std::to_string(f) + ".diff.png"), diff);
            }
            CHECK_MESSAGE(s.same_size, "size mismatch: ours ", out.w, "x", out.h, " ref ", ref.w, "x", ref.h);
            CHECK_MESSAGE(s.mean_abs <= thr, name, ": mean abs diff ", s.mean_abs, " > ", thr);
            CHECK_MESSAGE(s.frac_over_16 <= over, name, ": ", s.frac_over_16, " of the pixels are off by > 16 (max ",
                          over, ")");
        }
    }
    ntscrt_destroy(inst);
    if (!update.empty())
        std::printf("[golden-self] wrote %d baselines into %s\n", written,
                    (update == "os" ? os_dir : shared_dir).string().c_str());
    else
        std::printf("[golden-self] compared %d, skipped %d, failed %d, GPU hangs (macOS CI only) %d\n", compared,
                    skipped, failed, hung);
}
