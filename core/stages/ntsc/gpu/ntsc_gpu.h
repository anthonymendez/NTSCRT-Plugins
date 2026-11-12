// ntsc_gpu.h — the GPU port of the ntsc-rs effect (phases 1-3 of 4; design:
// docs/dev/NTSC-GPU.md). The Rust path (NtscStage::process) stays the
// reference and the fallback: a frame runs here only when every pass its
// settings turn on is implemented (NtscGpuPlan::supported) and the route is
// enabled (ntsc_gpu_route: NTSCRT_NTSC_GPU=1, D3D11, not a software device).
#pragma once

#include <ntscrs_capi.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../../../gpu/gpu.h"

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
    double table_ms = 0;                                    // CPU time of the tables above (and the plan)
    int width = 0, height = 0;
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

// Whether frames may take the GPU route on `dev` at all. Off unless
// NTSCRT_NTSC_GPU=1 (or "force": also on a software device such as WARP,
// where the Rust path is faster); never on Metal in this phase (unverified;
// Metal compiles with fast math). `why` says why not.
bool ntsc_gpu_route(gpu::Device &dev, std::string *why = nullptr);
// NTSCRT_NTSC_GPU is set to something other than "" / "0".
bool ntsc_gpu_requested();
// The D3D11 device is Microsoft's software rasterizer (WARP).
bool gpu_is_software(gpu::Device &dev);

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
