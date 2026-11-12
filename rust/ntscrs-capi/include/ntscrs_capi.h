/*
 * ntscrs_capi.h — C declarations for rust/ntscrs-capi (src/lib.rs), a minimal
 * C ABI over the ntsc-rs core crate. Port of NTSCRT Vendor/ntscrs-capi; the
 * additions are ntsc_process_frame_ex and the GPU plan exports (ntsc_gpu_*,
 * ntsc_debug_process_yiq; src/gpu_plan.rs).
 *
 * All strings are UTF-8 and returned strings must be freed with
 * ntsc_string_free.
 */
#ifndef NTSCRS_CAPI_H
#define NTSCRS_CAPI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NtscInstance NtscInstance;

#define NTSC_RGBA8 0
#define NTSC_BGRA8 1

void ntsc_string_free(char *s);

/* JSON array of setting descriptors (name, label, description, kind,
 * min/max, enum options, nested "group" children). NULL on failure. */
char *ntsc_settings_descriptors_json(void);

NtscInstance *ntsc_new(void);  /* ntsc-rs default settings */
void ntsc_free(NtscInstance *inst);

/* Settings in ntsc-rs's "version": 1 preset JSON. */
char *ntsc_settings_to_json(const NtscInstance *inst);
/* 0 = ok; nonzero = failure, *err_out (if non-NULL) = message to free. */
int32_t ntsc_settings_from_json(NtscInstance *inst, const char *json, char **err_out);

/* Apply the effect in place to width*height 4-byte pixels with `row_bytes`
 * stride. `frame_index` selects the field and seeds the noise. Alpha is
 * written opaque. 0 = ok. */
int32_t ntsc_process_frame(const NtscInstance *inst, int32_t fmt, uint8_t *data, uint32_t width,
                           uint32_t height, uint32_t row_bytes, int64_t frame_index);

/* As ntsc_process_frame, plus ntsc-rs's scale_factor (proxy render scale;
 * 1,1 = identical to ntsc_process_frame) and a wrapping offset added to the
 * settings' random_seed (0 = identical). */
int32_t ntsc_process_frame_ex(const NtscInstance *inst, int32_t fmt, uint8_t *data, uint32_t width,
                              uint32_t height, uint32_t row_bytes, int64_t frame_index, float scale_x,
                              float scale_y, int32_t seed_offset);

/* ---- GPU plan (src/gpu_plan.rs, docs/dev/NTSC-GPU.md) ------------------
 * Everything random / 64-bit / f64 the GPU port of the effect needs, resolved
 * with ntsc-rs's own code for one frame. Layouts are mirrored by
 * core/stages/ntsc/gpu (static_asserts there). */

/* Stages, as bit numbers, in ntsc-rs's pass order (apply_effect_to_yiq_field). */
enum {
    NTSC_ST_LUMA_FILTER = 0,
    NTSC_ST_CHROMA_LOWPASS_IN = 1,
    NTSC_ST_CHROMA_INTO_LUMA = 2, /* always runs */
    NTSC_ST_COMPOSITE_SHARPENING = 3,
    NTSC_ST_COMPOSITE_NOISE = 4,
    NTSC_ST_SNOW = 5,
    NTSC_ST_HEAD_SWITCHING = 6,
    NTSC_ST_TRACKING_NOISE = 7,
    NTSC_ST_LUMA_INTO_CHROMA = 8, /* always runs */
    NTSC_ST_LUMA_SMEAR = 9,
    NTSC_ST_RINGING = 10,
    NTSC_ST_LUMA_NOISE = 11,
    NTSC_ST_CHROMA_NOISE = 12,
    NTSC_ST_CHROMA_PHASE_ERROR = 13,
    NTSC_ST_CHROMA_PHASE_NOISE = 14,
    NTSC_ST_CHROMA_DELAY = 15,
    NTSC_ST_EDGE_WAVE = 16,
    NTSC_ST_TAPE_SPEED = 17, /* masking it off also turns sharpening off */
    NTSC_ST_CHROMA_LOSS = 18,
    NTSC_ST_SHARPEN = 19,
    NTSC_ST_VERT_BLEND = 20,
    NTSC_ST_CHROMA_LOWPASS_OUT = 21,
    NTSC_ST_COUNT = 22
};
#define NTSC_ST_ALL 0xffffffffu

/* Pass kinds. */
enum {
    NTSC_GPU_K_FILTER = 1,           /* row IIR on `planes`, filter[plane] each */
    NTSC_GPU_K_LUMA_BOX = 2,         /* input luma box filter */
    NTSC_GPU_K_CHROMA_INTO_LUMA = 3, /* modulate I/Q into Y (row xi) */
    NTSC_GPU_K_DEMOD = 4,            /* luma_into_chroma: mode 0 box, 1 notch (filter[0]), 2 1-line comb, 3 2-line comb */
    NTSC_GPU_K_ROTATE_CONST = 5,     /* rotate (I,Q): a = sin, b = cos */
    NTSC_GPU_K_ROTATE_ROWS = 6,      /* rotate (I,Q) by the row table's sin/cos */
    NTSC_GPU_K_CHROMA_LOSS = 7,      /* zero I,Q on flagged rows */
    NTSC_GPU_K_VERT_BLEND = 8        /* chroma_vert_blend per field */
};
#define NTSC_GPU_ROW_CHROMA_LOSS 1u
#define NTSC_GPU_INIT_ZERO 0
#define NTSC_GPU_INIT_FIRST_SAMPLE 1

typedef struct NtscGpuFilter {
    float num[4];    /* numerator b0..b3 */
    float den[4];    /* denominator a1..a3 (implicit a0 = 1); den[3] = 0 */
    float zi[4];     /* initial state factors: z[k] = zi[k] * initial value */
    int32_t len;     /* coefficient count (2..4); 0 = plane not filtered */
    int32_t delay;   /* output shifted left by `delay` samples */
    int32_t initial; /* NTSC_GPU_INIT_* */
    float initial_value;
} NtscGpuFilter;

typedef struct NtscGpuPass {
    int32_t stage;   /* NTSC_ST_* */
    int32_t kind;    /* NTSC_GPU_K_* */
    uint32_t planes; /* bit 0 Y, 1 I, 2 Q */
    int32_t mode;
    float a, b;
    int32_t pad[2];
    NtscGpuFilter filter[3]; /* Y, I, Q */
} NtscGpuPass;

typedef struct NtscGpuRow {     /* one per plane row */
    uint32_t src_row;           /* source image row (fielding) */
    uint32_t field_row;         /* row index within its field */
    uint32_t field_first;       /* plane row of its field's first row */
    uint32_t field_rows;        /* rows in its field */
    uint32_t xi;                /* chroma subcarrier phase (0..3) */
    uint32_t flags;             /* NTSC_GPU_ROW_* */
    float phase_sin, phase_cos; /* chroma phase noise rotation */
} NtscGpuRow;

typedef struct NtscGpuOutRow { /* one per output row: plane rows a, b averaged (a == b: copied) */
    uint32_t a, b;
} NtscGpuOutRow;

typedef struct NtscGpuPlanInfo {
    uint64_t seed;
    uint64_t field_frame[2];
    int32_t field;       /* 0 Upper, 1 Lower, 2 Both, 3 InterleavedUpper, 4 InterleavedLower */
    uint32_t plane_rows; /* rows of each Y/I/Q plane */
    uint32_t num_fields;
    uint32_t field_first[2];
    uint32_t field_rows[2];
    uint32_t num_passes;
    uint32_t unsupported; /* stages on but not implemented on the GPU (bit per NTSC_ST_*) */
    uint32_t enabled;     /* stages that run */
    float horizontal_scale, vertical_scale;
} NtscGpuPlanInfo;

typedef struct NtscGpuRowSeed { /* phase 2: one per plane row */
    uint32_t seed_lo, seed_hi;  /* the row's Xoshiro seed */
    int32_t noise_seed;         /* rng.next_u32() (Fbm seed) */
    float offset;               /* rng.random::<f32>() * width */
} NtscGpuRowSeed;

typedef struct NtscGpuRowShift { /* phase 3: one per plane row */
    float shift;
    int32_t boundary;   /* 0 constant 0, 1 extend */
    int32_t copy_start; /* -1 = whole row */
    float transient_intensity;
} NtscGpuRowShift;

typedef struct NtscGpuSpeckle { /* phases 2-3: one per transient */
    uint32_t row;
    int32_t x0;
    uint32_t count;
    uint32_t value_offset;
} NtscGpuSpeckle;

/* The plan of one frame (same arguments as ntsc_process_frame_ex, plus
 * `stage_mask`: stages to keep, NTSC_ST_ALL = the settings as they are).
 * `passes`: max_passes entries; `rows`, `out`: `height` entries (may be NULL).
 * 0 = ok, 3 = more than max_passes passes (info->num_passes says how many). */
int32_t ntsc_gpu_plan(const NtscInstance *inst, uint32_t width, uint32_t height, int64_t frame_index, float scale_x,
                      float scale_y, int32_t seed_offset, uint32_t stage_mask, NtscGpuPlanInfo *info,
                      NtscGpuPass *passes, uint32_t max_passes, NtscGpuRow *rows, NtscGpuOutRow *out);

/* Per-row noise seeds of a noise pass (0 composite, 1 chroma I, 9 chroma Q,
 * 10 luma: ntsc-rs's noise_seeds). `out`: `height` entries. 0 = ok. */
int32_t ntsc_gpu_row_seeds(const NtscInstance *inst, uint32_t width, uint32_t height, int64_t frame_index,
                           float scale_x, float scale_y, int32_t seed_offset, uint32_t pass, NtscGpuRowSeed *out);

/* Phase 3 / phases 2-3 tables: formats fixed, not implemented yet (return -1). */
int32_t ntsc_gpu_row_shifts(const NtscInstance *inst, uint32_t width, uint32_t height, int64_t frame_index,
                            float scale_x, float scale_y, int32_t seed_offset, uint32_t stage, NtscGpuRowShift *out);
int32_t ntsc_gpu_speckles(const NtscInstance *inst, uint32_t width, uint32_t height, int64_t frame_index,
                          float scale_x, float scale_y, int32_t seed_offset, uint32_t stage, NtscGpuSpeckle *events,
                          uint32_t max_events, float *values, uint32_t max_values, uint32_t *num_events,
                          uint32_t *num_values);

/* Debug / parity: ntsc-rs on one frame with only `stage_mask`'s stages, the
 * Y, I, Q planes (width * plane_rows floats each, before deinterlacing) into
 * `planes` (planes_len floats). 0 = ok. */
int32_t ntsc_debug_process_yiq(const NtscInstance *inst, int32_t fmt, const uint8_t *data, uint32_t width,
                               uint32_t height, uint32_t row_bytes, int64_t frame_index, float scale_x, float scale_y,
                               int32_t seed_offset, uint32_t stage_mask, float *planes, size_t planes_len);

/* Bit per NTSC_ST_* stage the GPU implements. */
uint32_t ntsc_gpu_implemented_stages(void);

#ifdef __cplusplus
}
#endif
#endif /* NTSCRS_CAPI_H */
