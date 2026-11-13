// ntsc_gpu.h — the GPU port of the ntsc-rs effect (design: docs/dev/NTSC-GPU.md).
// The Rust path (NtscStage::process) stays the reference and the fallback: a
// frame runs here only when every pass its settings turn on is implemented
// (NtscGpuPlan::supported) and the route is on (ntsc_gpu_route: by default on
// D3D11 hardware devices that do ntsc-rs's fused multiply-adds exactly).
#pragma once

#include <ntscrs_capi.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../../../gpu/gpu.h"
#include "../../../gpu/gpu_route.h"

namespace ntscrt {

// Speckle events of one stage (ntsc_gpu_speckles), plus each plane row's
// range of them (first event, count) for the kernel.
struct NtscGpuSpeckleList {
    std::vector<NtscGpuSpeckle> events;
    std::vector<float> values;
    std::vector<uint32_t> row_ranges;  // 2 per plane row
};

// One frame's plan, from the shim (ntsc_gpu_plan): the passes in ntsc-rs's
// order, their coefficients, the per-row tables.
struct NtscGpuPlan {
    static constexpr int kSeedPasses = 11;  // ntsc-rs's noise_seeds 0..10
    NtscGpuPlanInfo info{};
    std::vector<NtscGpuPass> passes;
    std::vector<NtscGpuRow> rows;    // info.plane_rows entries used
    std::vector<NtscGpuOutRow> out;  // one per output row
    // Tables of the passes that have them (filled for the passes in `passes` only):
    std::vector<NtscGpuRowSeed> seeds[kSeedPasses];         // by K_NOISE mode (seed pass)
    std::vector<NtscGpuRowShift> shifts[NTSC_ST_COUNT];     // by stage (K_SHIFT_ROWS, tracking K_NOISE)
    NtscGpuSpeckleList speckles[NTSC_ST_COUNT];             // by stage (K_SPECKLES)
    uint32_t simd_lanes = 8;                                // ntsc_gpu_simd_lanes()
    bool tables_ready = false;                              // the tables are this plan's (gpu_tables)
    double table_ms = 0;                                    // CPU time of the plan and the tables
    float scale = 1;                                        // the frame's arguments for the tables
    int64_t frame = 0;
    int32_t seed_offset = 0;
    int width = 0, height = 0;
    // One-entry cache (NtscStage::gpu_plan): what this plan and its tables were
    // made for. The same settings, frame, seed, size, scale and stage mask
    // again (a paused timeline, an Inspector edit of a non-NTSC param) keep
    // them instead of building them again; `cached` says the last call did.
    struct Key {
        std::string settings;  // ntsc-rs's effective settings JSON
        int64_t frame = 0;
        int32_t seed_offset = 0;
        int width = 0, height = 0;
        float scale = 0;
        uint32_t stage_mask = 0, forced_cpu = 0;
        bool operator==(const Key &o) const {
            return frame == o.frame && seed_offset == o.seed_offset && width == o.width && height == o.height &&
                   scale == o.scale && stage_mask == o.stage_mask && forced_cpu == o.forced_cpu &&
                   settings == o.settings;
        }
    };
    Key key;
    bool key_valid = false;
    bool cached = false;
    bool supported() const { return info.unsupported == 0; }
    // "composite_noise,head_switching" etc.: the stages that keep this frame on the CPU.
    std::string unsupported_names() const;
};

// Name of an NTSC_ST_* stage ("luma_filter", ...).
const char *ntsc_stage_name(int stage);

// Mirrors `NtscGpuU` in shaders/ntsc_gpu.slang (ConstantBuffer b0): 4-byte
// scalars and 4-vectors only.
struct NtscGpuU {
    uint32_t width, planeRows, outHeight, planes;
    uint32_t mode, pad0, pad1, pad2;
    float c16, inv255, rotSin, rotCos;
    float num[3][4];
    float den[3][4];
    float zi[3][4];
    int32_t fp[3][4];  // len, delay, initial, unused
    float fiv[4];
    float fx[4];    // per pass: noise frequency, -, gain / shift amount
    int32_t ix[4];  // per pass: octaves / vertical offset, SIMD lanes, flag, unused
};
static_assert(sizeof(NtscGpuU) == 288, "NtscGpuU must match the shader's constant buffer");
static_assert(sizeof(NtscGpuRowSeed) == 16 && sizeof(NtscGpuRowShift) == 16 && sizeof(NtscGpuSpeckle) == 16,
              "phase 2-3 tables (gpu_plan.rs)");
static_assert(sizeof(NtscGpuFilter) == 64 && sizeof(NtscGpuPass) == 224, "shim layout (gpu_plan.rs)");
static_assert(sizeof(NtscGpuRow) == 32 && sizeof(NtscGpuOutRow) == 8, "row tables (gpu_plan.rs)");
static_assert(sizeof(NtscGpuPlanInfo) == 72, "plan info (gpu_plan.rs)");

class NtscGpu;

// NTSCRT_NTSC_GPU: unset / "" / "auto" = Auto (the default), "0" = Off (ntsc-rs
// always), "1" = On (as Auto), "force" = also on software devices (WARP) and
// on a GPU whose mad() isn't fused and that has no doubles (gpu_route.h, shared
// with NTSCRT_DIGITAL_GPU).
using NtscGpuEnv = GpuRouteEnv;
NtscGpuEnv ntsc_gpu_env();
// The raw value ("" when unset): a change re-decides an instance's route.
std::string ntsc_gpu_env_text();

// Whether frames may take the GPU route on `dev` at all (NTSC-GPU.md "The
// default rule"): not with NTSCRT_NTSC_GPU=0, never on Metal (unverified;
// Metal compiles with fast math), not on a software device (ntsc-rs is
// faster) and, given `g` (its fused-mad probe), not on a GPU that can do
// ntsc-rs's fused multiply-adds neither with mad() nor in double precision,
// unless "force". `why` gets the reason either way: on, it names the
// setting, the adapter, the FMA mode and whether this CPU's ntsc-rs fuses.
bool ntsc_gpu_route(gpu::Device &dev, std::string *why = nullptr, NtscGpu *g = nullptr);
// NTSCRT_NTSC_GPU isn't "0" (the route may be on).
bool ntsc_gpu_requested();
// This CPU's ntsc-rs fuses its multiply-adds (fearless_simd's AVX2 level,
// which requires FMA, or NEON): the GPU route is then bit-identical to it.
// Without (pre-AVX2 x86) ntsc-rs rounds its products separately and the GPU,
// which always fuses, may differ from it by a level (the route is still used).
bool cpu_ntsc_fuses();

class NtscGpu {
public:
    explicit NtscGpu(gpu::Device &dev);
    ~NtscGpu();
    // Runs `plan` on `in` (BGRA8, plan.width x plan.height): the effect, then
    // the Bob deinterlace to a BGRA8 texture (alpha opaque, as ntsc-rs writes).
    std::shared_ptr<gpu::Texture> run(const gpu::Texture &in, const NtscGpuPlan &plan);
    // Debug / parity: the Y, I, Q planes (plane_rows x width floats each,
    // Y then I then Q) after the first `num_passes` passes (-1: all).
    std::vector<float> planes(const gpu::Texture &in, const NtscGpuPlan &plan, int num_passes = -1);
    // How this device does ntsc-rs's fused multiply-adds: "mad" (mad() is
    // fused), "double" (it isn't, e.g. WARP: the *_x kernels, exact), or
    // "mad-unfused" (neither: results may differ from ntsc-rs by a level).
    const char *fma_mode();
    // With NTSCRT_PROFILE=1: the last run's time per pass (waits for the GPU
    // after each one), "from_bgra8", "<stage name>", ..., "to_bgra8".
    const std::vector<std::pair<std::string, double>> &pass_times() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ntscrt
