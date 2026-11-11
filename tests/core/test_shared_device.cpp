// One GPU device, kernel cache and (for presets without history) librashader
// chain per process, shared by every instance: same pictures as instances on
// their own, safe from several threads at once, device loss handled per
// instance; the Digital thread pool is joined when the last instance goes.
#include <doctest/doctest.h>
#include <ntscrt/ntscrt.h>

#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "stages/digital/parallel.h"
#include "testing.h"
#include "gpu/gpu.h"

using namespace ntscrt;

namespace {

ntscrt_instance *make() {
    ntscrt_create_info ci{NTSCRT_ABI_VERSION, NTSCRT_GPU_AUTO, nullptr, nullptr, NTSCRT_BUILT_RESOURCES_DIR};
    ntscrt_instance *inst = nullptr;
    REQUIRE(ntscrt_create(&ci, &inst) == NTSCRT_OK);
    return inst;
}

struct Clip {
    int w, h;
    std::map<int64_t, std::vector<uint8_t>> frames;
    std::map<const ntscrt_image *, std::unique_ptr<ntscrt_image>> live;
    Clip(int w_, int h_) : w(w_), h(h_) {
        for (int64_t f = 0; f < 8; ++f) {
            auto &px = frames[f];
            px.resize(size_t(w) * h * 4);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    uint8_t *p = &px[(size_t(y) * w + x) * 4];
                    p[0] = uint8_t((x * 7 + f * 13) & 255);
                    p[1] = uint8_t((y * 5 + x) & 255);
                    p[2] = uint8_t(((x ^ y) * 3 + f * 29) & 255);
                    p[3] = 255;
                }
        }
    }
    ntscrt_image image(int64_t f) { return {w, h, ptrdiff_t(w) * 4, NTSCRT_RGBA8, frames.at(f).data()}; }
    static const ntscrt_image *fetch(void *u, int64_t f) {
        auto *c = static_cast<Clip *>(u);
        if (f < 0 || f >= 8) return nullptr;
        auto im = std::make_unique<ntscrt_image>(c->image(f));
        const ntscrt_image *k = im.get();
        c->live[k] = std::move(im);
        return k;
    }
    static void release(void *u, const ntscrt_image *im) { static_cast<Clip *>(u)->live.erase(im); }
};

std::vector<uint8_t> render(ntscrt_instance *inst, Clip &clip, int64_t frame, const std::string &look,
                            ntscrt_status *st_out = nullptr) {
    ntscrt_render_args a{};
    a.frame_index = frame;
    a.fps = 30;
    a.look_json = look.c_str();
    a.render_scale = 1;
    a.fetch_source = &Clip::fetch;
    a.release_source = &Clip::release;
    a.user = &clip;
    ntscrt_image src = clip.image(frame);
    std::vector<uint8_t> px(size_t(clip.w) * clip.h * 4, 7);
    ntscrt_image dst{clip.w, clip.h, ptrdiff_t(clip.w) * 4, NTSCRT_RGBA8, px.data()};
    ntscrt_status st = ntscrt_render_cpu(inst, &a, &src, &dst);
    if (st_out) *st_out = st;
    else CHECK(st == NTSCRT_OK);
    return px;
}

// Looks for the instances: shared glow chain with different params, the
// temporal crtsim (own chain per instance), the bypass, and a Digital look.
std::vector<std::string> looks() {
    return {
        R"({"shader":{"enabled":true,"preset":"glow_gauss"}})",
        R"({"shader":{"enabled":true,"preset":"glow_gauss","params":{"BOOST":1.4,"CURVATURE":1.0}}})",
        R"({"shader":{"enabled":true,"preset":"sim"}})",
        R"({"shader":{"enabled":true,"preset":"easymode"},"glitch":{"enabled":true}})",
        R"({"shader":{"enabled":false}})",
        R"({"shader":{"enabled":true,"preset":"glow_gauss","params":{"BOOST":0.8}},"digital":{"enabled":true}})",
    };
}

bool has_gpu(ntscrt_instance *inst) { return testing::stats(inst).has_gpu; }

}  // namespace

TEST_CASE("instances share one GPU device") {
    ntscrt_instance *a = make(), *b = make();
    if (!has_gpu(a)) {
        ntscrt_destroy(a);
        ntscrt_destroy(b);
        return;
    }
    CHECK(testing::stats(a).gpu_device != nullptr);
    CHECK(testing::stats(a).gpu_device == testing::stats(b).gpu_device);
    ntscrt_destroy(a);
    ntscrt_destroy(b);
}

TEST_CASE("instances sharing the device and chains render what a lone instance renders") {
    const auto L = looks();
    // Reference: each look on an instance of its own, frames in order.
    std::vector<std::vector<std::vector<uint8_t>>> ref(L.size());
    {
        for (size_t i = 0; i < L.size(); ++i) {
            ntscrt_instance *inst = make();
            if (!has_gpu(inst)) {
                ntscrt_destroy(inst);
                return;
            }
            Clip clip(160, 120);
            for (int64_t f = 0; f < 4; ++f) ref[i].push_back(render(inst, clip, f, L[i]));
            ntscrt_destroy(inst);
        }
    }
    // Alternating on one thread: every frame switches the shared glow chain's params.
    {
        std::vector<ntscrt_instance *> inst;
        std::vector<std::unique_ptr<Clip>> clips;
        for (size_t i = 0; i < L.size(); ++i) {
            inst.push_back(make());
            clips.push_back(std::make_unique<Clip>(160, 120));
        }
        for (int64_t f = 0; f < 4; ++f)
            for (size_t i = 0; i < L.size(); ++i) {
                CAPTURE(i);
                CAPTURE(f);
                CHECK(render(inst[i], *clips[i], f, L[i]) == ref[i][size_t(f)]);
            }
        for (auto *p : inst) ntscrt_destroy(p);
    }
    // Concurrently: two instances per look, one thread each.
    {
        const size_t n = L.size() * 2;
        std::vector<ntscrt_instance *> inst;
        for (size_t i = 0; i < n; ++i) inst.push_back(make());
        std::atomic<int> mismatches{0}, failures{0};
        std::vector<std::thread> th;
        for (size_t i = 0; i < n; ++i)
            th.emplace_back([&, i] {
                Clip clip(160, 120);
                const size_t l = i % L.size();
                for (int64_t f = 0; f < 4; ++f) {
                    ntscrt_status st;
                    auto px = render(inst[i], clip, f, L[l], &st);
                    if (st != NTSCRT_OK) ++failures;
                    else if (px != ref[l][size_t(f)]) ++mismatches;
                }
            });
        for (auto &t : th) t.join();
        CHECK(failures == 0);
        CHECK(mismatches == 0);
        for (auto *p : inst) ntscrt_destroy(p);
    }
}

TEST_CASE("different output sizes on one shared chain") {
    ntscrt_instance *a = make(), *b = make(), *lone = make();
    if (!has_gpu(a)) {
        for (auto *p : {a, b, lone}) ntscrt_destroy(p);
        return;
    }
    const std::string look = R"({"shader":{"enabled":true,"preset":"glow_gauss"}})";
    Clip small(96, 72), big(200, 150), small2(96, 72), big2(200, 150);
    auto rs = render(lone, small2, 1, look);
    auto rb = render(lone, big2, 1, look);
    for (int k = 0; k < 2; ++k) {
        CHECK(render(a, small, 1, look) == rs);
        CHECK(render(b, big, 1, look) == rb);
    }
    for (auto *p : {a, b, lone}) ntscrt_destroy(p);
}

TEST_CASE("a device loss on one instance leaves the others rendering") {
    ntscrt_instance *a = make(), *b = make();
    if (!has_gpu(a)) {
        ntscrt_destroy(a);
        ntscrt_destroy(b);
        return;
    }
    const std::string look = R"({"shader":{"enabled":true,"preset":"glow_gauss"}})";
    Clip ca(96, 72), cb(96, 72);
    auto ref = render(b, cb, 2, look);
    testing::simulate_device_lost(a);
    CHECK(render(a, ca, 2, look) == ref);  // recovered on a device again
    CHECK(testing::stats(a).has_gpu);
    CHECK(render(b, cb, 2, look) == ref);
    testing::simulate_device_lost(a, true);  // no new device for a: CPU stages from now on
    ntscrt_status st;
    render(a, ca, 2, look, &st);
    CHECK(st == NTSCRT_ERR_GPU);
    CHECK(!testing::stats(a).has_gpu);
    CHECK(render(b, cb, 2, look) == ref);  // b keeps the shared device and chain
    CHECK(testing::stats(b).has_gpu);
    ntscrt_destroy(a);
    ntscrt_destroy(b);
}

TEST_CASE("the Digital thread pool is joined when the last instance goes") {
    ntscrt_instance *a = make(), *b = make();
    Clip clip(128, 96);
    render(a, clip, 1, R"({"digital":{"enabled":true},"shader":{"enabled":false}})");
    CHECK(digital::threads_running());
    ntscrt_destroy(a);
    CHECK(digital::threads_running());  // b is still there
    ntscrt_destroy(b);
    CHECK(!digital::threads_running());
    // ...and starts again on demand.
    ntscrt_instance *c = make();
    Clip clip2(128, 96);
    render(c, clip2, 1, R"({"digital":{"enabled":true},"shader":{"enabled":false}})");
    CHECK(digital::threads_running());
    ntscrt_destroy(c);
}

TEST_CASE("last_create_error is per thread") {
    // Metal can't start on Windows (and D3D11 not on macOS): that thread sees why.
#ifdef _WIN32
    const gpu::Backend missing = gpu::Backend::Metal;
#else
    const gpu::Backend missing = gpu::Backend::D3D11;
#endif
    std::string other;
    std::thread t([&] {
        CHECK(gpu::Device::create(missing, nullptr, nullptr) == nullptr);
        other = gpu::Device::last_create_error();
    });
    t.join();
    CHECK(!other.empty());
    // This thread's last call didn't fail.
    CHECK(std::strcmp(gpu::Device::last_create_error(), "") == 0);
}
