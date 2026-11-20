// Digital video through the C ABI (docs/dev/DIGITAL-CHECKPOINTS.md): whatever order
// the host asks for frames in (cold seeks, jumps back, stepping back one frame at a
// time, renders at another render scale in between, other settings in between, renders
// the host aborts), every frame is byte-identical to playing from frame 0 in order, for
// every Digital video format on the plug-in's default look (NTSC, CRT on) and alone; and
// after a frame's stream was coded once a jump re-runs at most a checkpoint's spacing,
// even under a datamosh that drops every keyframe (no natural restart).
#include <doctest/doctest.h>

#include <ntscrt/ntscrt.h>

#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ntscrt_host/params.h"

using json = nlohmann::json;

namespace {

void set_env(const char *k, const char *v) {
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1);
    else unsetenv(k);
#endif
}

constexpr int W = 192, H = 108;
constexpr int kEvery = 15;  // CheckpointConfig::every

struct Host {
    std::map<int64_t, std::vector<uint8_t>> px;
    std::map<int64_t, ntscrt_image> img;
    int w = W, h = H;
    int fetches = 0;
    int fail_after = -1;  // >= 0: fetches past this many in a render get nothing (an abort)
    std::function<std::string(int64_t)> look_at;  // keyframes (nullptr: none)
    std::string look_text;
    static std::vector<uint8_t> make(int64_t f, int w, int h) {
        std::vector<uint8_t> p(size_t(w) * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t *q = &p[(size_t(y) * w + x) * 4];
                const int sx = int(10 + 3 * f) % (w + 40) - 20;  // a square moving across a texture
                const bool sq = x >= sx && x < sx + 30 && y >= 30 && y < 60;
                q[0] = uint8_t(sq ? 30 : (x * 255) / w);
                q[1] = uint8_t(sq ? 240 : ((x / 6 + y / 6) % 2) * 120 + 40 + f % 7);
                q[2] = uint8_t(sq ? 250 : (y * 255) / h);
                q[3] = 255;
            }
        return p;
    }
};
const ntscrt_image *fetch(void *u, int64_t f) {
    auto *s = static_cast<Host *>(u);
    if (f < 0) return nullptr;
    if (s->fail_after >= 0 && s->fetches >= s->fail_after) return nullptr;
    ++s->fetches;
    s->px[f] = Host::make(f, s->w, s->h);
    s->img[f] = {s->w, s->h, ptrdiff_t(s->w) * 4, NTSCRT_RGBA8, s->px[f].data()};
    return &s->img[f];
}
void release(void *u, const ntscrt_image *im) {
    auto *s = static_cast<Host *>(u);
    for (auto it = s->img.begin(); it != s->img.end(); ++it)
        if (&it->second == im) {
            s->px.erase(it->first);
            s->img.erase(it);
            return;
        }
}
const char *look_at(void *u, int64_t f) {
    auto *s = static_cast<Host *>(u);
    if (!s->look_at) return nullptr;
    s->look_text = s->look_at(f);
    return s->look_text.c_str();
}

json plugin_default() {
    static const json l = [] {
        const auto schema = ntscrt::host::Schema::parse(ntscrt_param_schema_json());
        return ntscrt::host::build_look(schema, [&](size_t i) { return schema.params[i].def; }).look;
    }();
    return l;
}
json with_format(json look, const std::string &id) {
    static const json s = json::parse(ntscrt_param_schema_json());
    bool found = false;
    for (const json &f : s["digital_formats"])
        if (f.value("id", std::string()) == id) {
            found = true;
            for (auto it = f["applies"].begin(); it != f["applies"].end(); ++it) {
                if (it.value().is_object())
                    for (auto v = it.value().begin(); v != it.value().end(); ++v)
                        ntscrt::host::set_at(look, it.key() + "/" + v.key(), v.value());
                else ntscrt::host::set_at(look, it.key(), it.value());
            }
        }
    REQUIRE_MESSAGE(found, id);
    return look;
}
json digital_only(json look) {
    for (const char *s : {"ntsc", "glitch", "crt", "shader"})
        if (look.contains(s) && look[s].is_object()) look[s]["enabled"] = false;
    return look;
}

int note_int(const std::string &tm, const char *key) {
    const size_t p = tm.find(key);
    return p == std::string::npos ? -999 : std::atoi(tm.c_str() + p + std::char_traits<char>::length(key));
}

struct Inst {
    ntscrt_instance *p = nullptr;
    Host host;
    std::string timings;
    Inst() {
        ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_AUTO, nullptr, nullptr, NTSCRT_BUILT_RESOURCES_DIR};
        REQUIRE(ntscrt_create(&ci, &p) == NTSCRT_OK);
    }
    ~Inst() { ntscrt_destroy(p); }
    Inst(const Inst &) = delete;
    std::vector<uint8_t> render(const std::string &look, int64_t f, double scale = 1) {
        host.w = int(W * scale);
        host.h = int(H * scale);
        std::vector<uint8_t> in = Host::make(f, host.w, host.h), out(in.size());
        ntscrt_image s{host.w, host.h, ptrdiff_t(host.w) * 4, NTSCRT_RGBA8, in.data()};
        ntscrt_image d{host.w, host.h, ptrdiff_t(host.w) * 4, NTSCRT_RGBA8, out.data()};
        ntscrt_render_args ra{};
        ra.frame_index = f;
        ra.fps = 30;
        ra.look_json = look.c_str();
        ra.render_scale = scale;
        ra.seed = 3;
        ra.fetch_source = fetch;
        ra.release_source = release;
        ra.look_at = &::look_at;
        ra.user = &host;
        host.fetches = 0;
        const ntscrt_status st = ntscrt_render_cpu(p, &ra, &s, &d);
        CHECK_MESSAGE(st == NTSCRT_OK, ntscrt_last_error(p));
        timings = ntscrt_last_timings(p);
        return out;
    }
    int captures() const { return note_int(timings, "digital_captures="); }
};

std::vector<std::vector<uint8_t>> played(const std::string &look, int frames, double scale = 1) {
    Inst a;
    std::vector<std::vector<uint8_t>> out;
    for (int f = 0; f < frames; ++f) out.push_back(a.render(look, f, scale));
    return out;
}

// A cold render of f on a fresh instance: the stream from its keyframe, no checkpoints.
std::vector<uint8_t> fresh(const std::string &look, int64_t f, double scale = 1) {
    Inst a;
    return a.render(look, f, scale);
}

void check_seeks(const std::string &label, const json &lj) {
    INFO(label);
    constexpr int F = 160;
    const std::string look = lj.dump();
    const auto ref = played(look, F);
    Inst b;
    // Cold seeks, jumps back and forth, stepping back one frame at a time across a keyframe.
    for (int f : {159, 37, 121, 120, 119, 118, 117, 116, 5, 150, 149, 148, 80, 81, 82, 60, 159, 0, 140, 139}) {
        CAPTURE(f);
        CHECK(b.render(look, f) == ref[size_t(f)]);
    }
    // A render at another render scale in between (a host's viewer): that scale's own
    // stream, and the full-size one goes on as before.
    for (auto [f, g] : {std::pair{100, 101}, std::pair{90, 102}, std::pair{101, 103}}) {
        CAPTURE(f);
        CHECK(b.render(look, f, 0.5) == fresh(look, f, 0.5));
        CHECK(b.render(look, g) == ref[size_t(g)]);
    }
    // Other settings in between (a non-structure control and a structure one), then back.
    json other = lj;
    other["digital"]["values"]["quality"] = 0.3;
    json rate = lj;
    rate["digital"]["values"]["frame_rate"] = 10;
    for (const json &o : {other, rate}) {
        CHECK(b.render(o.dump(), 130) == fresh(o.dump(), 130));
        CHECK(b.render(look, 131) == ref[131]);
        CHECK(b.render(look, 125) == ref[125]);
    }
    // A render the host aborts (earlier frames it no longer gives), then the frame again.
    b.host.fail_after = 2;
    b.render(look, 155);
    b.host.fail_after = -1;
    CHECK(b.render(look, 155) == ref[155]);
    CHECK(b.render(look, 154) == ref[154]);
}

}  // namespace

TEST_CASE("Digital video: seeks, steps back, other render scales and settings render what playing renders") {
    for (const char *id : {"webcam", "video_call", "minidv", "early_web", "cd_rom", "datamosh"})
        check_seeks(std::string(id) + " (default look)", with_format(plugin_default(), id));
    // Digital video alone, and with data loss on a GOP format.
    check_seeks("datamosh (Digital video only)", digital_only(with_format(plugin_default(), "datamosh")));
    json lossy = digital_only(with_format(plugin_default(), "early_web"));
    lossy["digital"]["values"]["data_loss"] = 0.5;
    lossy["digital"]["values"]["deblocking"] = 0.7;
    check_seeks("early web + data loss + deblocking (Digital video only)", lossy);
}

TEST_CASE("Digital video: with every keyframe dropped, a jump re-runs at most the checkpoint spacing") {
    // Datamosh at 1: every keyframe after the first is lost, so the stream reaches back
    // to frame 0 (a minute's start). Once played, jumps resume from a checkpoint.
    const std::string look = digital_only(with_format(plugin_default(), "datamosh")).dump();
    constexpr int F = 200;
    const auto ref = played(look, F);
    {
        Inst cold;
        cold.render(look, 170);
        CHECK(cold.captures() == 171);  // nothing to go on from: the whole stream
    }
    Inst d;
    for (int f = 0; f < F; ++f) d.render(look, f);
    for (int f : {100, 50, 197, 20, 133, 134, 7, 199}) {
        CAPTURE(f);
        CHECK(d.render(look, f) == ref[size_t(f)]);
        CHECK(d.captures() < kEvery);
        CHECK(d.host.fetches < kEvery);
    }
    // Stepping back: after the first step, the frames before come from the trail.
    d.render(look, 180);
    int total = 0;
    for (int f = 179; f >= 166; --f) {
        CAPTURE(f);
        CHECK(d.render(look, f) == ref[size_t(f)]);
        total += d.captures();
    }
    CHECK(total <= kEvery);
    // NTSCRT_DIGITAL_CHECKPOINT_MB=0: no checkpoints, every jump back runs from frame 0.
    set_env("NTSCRT_DIGITAL_CHECKPOINT_MB", "0");
    Inst none;
    set_env("NTSCRT_DIGITAL_CHECKPOINT_MB", nullptr);
    for (int f = 0; f <= 60; ++f) none.render(look, f);
    CHECK(none.render(look, 40) == ref[40]);
    CHECK(none.captures() == 41);
}

TEST_CASE("Digital video: an edited keyframe before the frame invalidates the checkpoints after it") {
    const json base = digital_only(with_format(plugin_default(), "datamosh"));
    auto history = [base](double early) {
        return [base, early](int64_t f) {
            json l = base;
            l["digital"]["values"]["quality"] = f < 40 ? early : 0.6;
            return l.dump();
        };
    };
    const std::string look_now = [&] {  // the look from frame 40 on (the same in both histories)
        json l = base;
        l["digital"]["values"]["quality"] = 0.6;
        return l.dump();
    }();
    auto render_with = [&](Inst &i, double early, int64_t f) {
        i.host.look_at = history(early);
        return i.render(f < 40 ? history(early)(f) : look_now, f);
    };
    Inst b;
    for (int f = 0; f <= 150; ++f) render_with(b, 0.6, f);
    // The keyframe at frame 10 edited, then a jump (back): the checkpoints after it hold
    // the old history. (Playing on is trusted to be in the same history: an edit flushes
    // the stage in the plug-ins, NTSCRT_CACHE_DIGITAL.)
    const auto after = render_with(b, 0.1, 60);
    Inst c;
    CHECK(after == render_with(c, 0.1, 60));
    CHECK(b.captures() == 61);  // nothing holds: from the start again
    // Back to the first history: the checkpoints the re-run didn't pass are still there
    // (one per capture: those it passed now hold the edited history).
    Inst r;
    CHECK(render_with(b, 0.6, 150) == render_with(r, 0.6, 150));
    CHECK(b.captures() < kEvery);
    Inst r2;
    CHECK(render_with(b, 0.6, 100) == render_with(r2, 0.6, 100));
    CHECK(b.captures() < kEvery);
    Inst r3;
    CHECK(render_with(b, 0.6, 50) == render_with(r3, 0.6, 50));
    CHECK(b.captures() == 51);
}
