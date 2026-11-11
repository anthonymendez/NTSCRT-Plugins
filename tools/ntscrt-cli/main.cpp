// ntscrt-cli — headless renderer over the C ABI, for golden tests and debugging.
//
//   ntscrt-cli --in a.png --out b.png [--look look.json] [--feedback fb.json]
//              [--frame N] [--frames COUNT] [--fps 30] [--resources DIR]
//              [--seq "in_%04d.png"]   (image sequence input; enables temporal fetch)
//              [--timeline]   (play the look's "timeline" keys: frame 0 = the effect's first
//                              frame, the timeline's duration at --fps, looping, as the
//                              plug-ins key them; see core/params/timeline.h)
//
// With --frames > 1 the output path may contain %d / %04d.
#include <ntscrt/ntscrt.h>

#include <nlohmann/json.hpp>

#include "../../core/params/timeline.h"
#include "../../core/stages/digital/migrate.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {
std::string read_file(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot read %s\n", p.c_str()); std::exit(2); }
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}
std::string fmt_path(const std::string &pattern, long long n) {
    if (pattern.find('%') == std::string::npos) return pattern;
    char buf[1024]; std::snprintf(buf, sizeof buf, pattern.c_str(), n); return buf;
}
struct Loaded { int w = 0, h = 0; std::vector<uint8_t> px; ntscrt_image img{}; };
std::unique_ptr<Loaded> load_png(const std::string &p) {
    auto L = std::make_unique<Loaded>();
    int n = 0;
    unsigned char *d = stbi_load(p.c_str(), &L->w, &L->h, &n, 4);
    if (!d) return nullptr;
    L->px.assign(d, d + size_t(L->w) * L->h * 4);
    stbi_image_free(d);
    L->img = {L->w, L->h, ptrdiff_t(L->w) * 4, NTSCRT_RGBA8, L->px.data()};
    return L;
}
struct SeqUser { std::string pattern; std::map<const ntscrt_image *, std::unique_ptr<Loaded>> live; };
const ntscrt_image *seq_fetch(void *u, int64_t f) {
    auto *s = static_cast<SeqUser *>(u);
    if (s->pattern.empty() || f < 0) return nullptr;
    auto L = load_png(fmt_path(s->pattern, f));
    if (!L) return nullptr;
    const ntscrt_image *key = &L->img;
    s->live[key] = std::move(L);
    return key;
}
void seq_release(void *u, const ntscrt_image *im) { static_cast<SeqUser *>(u)->live.erase(im); }

// --timeline: the look per frame, as a host keyed from the timeline sends it.
struct Animated {
    nlohmann::json look;  // upgraded
    ntscrt::timeline::Timeline tl;
    ntscrt::timeline::Rules rules;
    double fps = 30;
    std::map<int64_t, std::string> cache;
    const std::string &at(int64_t f) {
        auto it = cache.find(f);
        if (it != cache.end()) return it->second;
        if (cache.size() > 4096) cache.clear();
        return cache[f] = ntscrt::timeline::look_at_frame(look, tl, rules, f, fps).dump();
    }
};
struct User {
    SeqUser seq;
    Animated *anim = nullptr;
};
const ntscrt_image *user_fetch(void *u, int64_t f) { return seq_fetch(&static_cast<User *>(u)->seq, f); }
void user_release(void *u, const ntscrt_image *im) { seq_release(&static_cast<User *>(u)->seq, im); }
const char *user_look_at(void *u, int64_t f) {
    auto *a = static_cast<User *>(u)->anim;
    return a ? a->at(f).c_str() : nullptr;
}
}  // namespace

int main(int argc, char **argv) {
    std::string in, out, look_path, fb_path, res = ".", seq;
    long long frame = 0, frames = 1; double fps = 30.0; bool timeline = false;
    for (int i = 1; i < argc; ++i) {
        auto a = std::string(argv[i]);
        auto next = [&]() -> std::string { if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); } return argv[++i]; };
        if (a == "--in") in = next(); else if (a == "--out") out = next();
        else if (a == "--look") look_path = next(); else if (a == "--feedback") fb_path = next();
        else if (a == "--frame") frame = std::atoll(next().c_str()); else if (a == "--frames") frames = std::atoll(next().c_str());
        else if (a == "--fps") fps = std::atof(next().c_str()); else if (a == "--resources") res = next();
        else if (a == "--seq") seq = next();
        else if (a == "--timeline") timeline = true;
        else if (a == "--schema") { std::puts(ntscrt_param_schema_json()); return 0; }
        else { std::fprintf(stderr, "unknown arg %s\n", a.c_str()); return 2; }
    }
    if ((in.empty() && seq.empty()) || out.empty()) { std::fprintf(stderr, "usage: ntscrt-cli --in a.png|--seq pat --out b.png [--look j]\n"); return 2; }
    std::string look = look_path.empty() ? "{}" : read_file(look_path);
    std::string fb = fb_path.empty() ? "" : read_file(fb_path);

    ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_AUTO, nullptr, nullptr, res.c_str()};
    ntscrt_instance *inst = nullptr;
    if (ntscrt_create(&ci, &inst) != NTSCRT_OK) { std::fprintf(stderr, "create failed\n"); return 1; }
    User su{SeqUser{seq, {}}, nullptr};
    Animated anim;
    if (timeline) {
        auto j = nlohmann::json::parse(look, nullptr, false);
        if (!j.is_object()) { std::fprintf(stderr, "--timeline: the look isn't a JSON object\n"); return 2; }
        anim.look = ntscrt::digital::migrate::upgraded(j);
        anim.tl = ntscrt::timeline::parse(anim.look);
        anim.rules = ntscrt::timeline::Rules::from_schema(nlohmann::json::parse(ntscrt_param_schema_json()));
        anim.fps = fps;
        if (anim.tl.animated()) su.anim = &anim;
        else std::fprintf(stderr, "--timeline: the look has no timeline keys\n");
    }
    int rc = 0;
    for (long long f = frame; f < frame + frames; ++f) {
        auto L = load_png(seq.empty() ? in : fmt_path(seq, f));
        if (!L) { std::fprintf(stderr, "cannot load input for frame %lld\n", f); rc = 1; break; }
        std::vector<uint8_t> o(L->px.size());
        ntscrt_image dst{L->w, L->h, ptrdiff_t(L->w) * 4, NTSCRT_RGBA8, o.data()};
        ntscrt_render_args ra{};
        ra.frame_index = f; ra.fps = fps; ra.look_json = su.anim ? anim.at(f).c_str() : look.c_str();
        ra.feedback_json = fb.empty() ? nullptr : fb.c_str(); ra.render_scale = 1.0;
        if (!seq.empty()) { ra.fetch_source = user_fetch; ra.release_source = user_release; }
        if (su.anim) ra.look_at = user_look_at;
        ra.user = &su;
        ntscrt_status st = ntscrt_render_cpu(inst, &ra, &L->img, &dst);
        if (st == NTSCRT_ERR_GPU || st == NTSCRT_ERR_MISSING_RESOURCE || st == NTSCRT_ERR_SOURCE_UNAVAILABLE)
            std::fprintf(stderr, "warning: frame %lld degraded (%d): %s\n", f, st, ntscrt_last_error(inst));  // still written
        else if (st != NTSCRT_OK) { std::fprintf(stderr, "render frame %lld failed (%d): %s\n", f, st, ntscrt_last_error(inst)); rc = 1; break; }
        std::string op = fmt_path(out, f);
        if (!stbi_write_png(op.c_str(), L->w, L->h, 4, o.data(), L->w * 4)) { std::fprintf(stderr, "cannot write %s\n", op.c_str()); rc = 1; break; }
    }
    ntscrt_destroy(inst);
    return rc;
}
