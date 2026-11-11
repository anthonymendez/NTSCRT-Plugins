/*
 * ntscrt.h — stable C ABI of ntscrt_core, shared by the OpenFX (Resolve) and
 * FxPlug 4 (Final Cut Pro) hosts and by tools/ntscrt-cli.
 *
 * CONTRACT (Wave 0). Every agent codes against this file. Changing it needs a
 * note in docs/CONTRACT-CHANGES.md and coordination with the host owners.
 *
 * Model: the host owns params, keyframes and the timeline. For every frame it
 * builds a "look" JSON (upstream NTSCRT preset format, see presets/*.json and
 * docs/params.md) from its parameter values at that time, then calls
 * ntscrt_render_*. Output size always equals input size.
 *
 * Temporal stages (Digital codec, Glitch receiver, Screen Loop) pull earlier
 * source frames and earlier look JSON through the callbacks in
 * ntscrt_render_args, so random-access rendering matches sequential output.
 */
#ifndef NTSCRT_H
#define NTSCRT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(NTSCRT_SHARED)
#  ifdef NTSCRT_BUILDING
#    define NTSCRT_API __declspec(dllexport)
#  else
#    define NTSCRT_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define NTSCRT_API __attribute__((visibility("default")))
#else
#  define NTSCRT_API
#endif

#define NTSCRT_ABI_VERSION 1

typedef struct ntscrt_instance ntscrt_instance;

typedef enum ntscrt_status {
    NTSCRT_OK = 0,
    NTSCRT_ERR_INVALID_ARG = 1,
    NTSCRT_ERR_BAD_PARAMS = 2,     /* look JSON failed to parse/validate */
    NTSCRT_ERR_GPU = 3,            /* device lost / shader compile failure */
    NTSCRT_ERR_MISSING_RESOURCE = 4, /* shader preset, ntsc-rs, librashader missing */
    NTSCRT_ERR_SOURCE_UNAVAILABLE = 5, /* temporal fetch failed; frame rendered degraded */
    NTSCRT_ERR_INTERNAL = 99
} ntscrt_status;
/* Render calls returning NTSCRT_ERR_GPU, NTSCRT_ERR_MISSING_RESOURCE or
 * NTSCRT_ERR_SOURCE_UNAVAILABLE still wrote a complete, degraded frame to dst
 * (CRT shader replaced by the plain picture, or CPU stages only after the GPU
 * was lost); ntscrt_last_error says what is missing. Show it as a warning,
 * don't fail the frame. Other non-OK statuses leave dst unspecified.
 * ntscrt_last_error is cleared by a render that returns NTSCRT_OK. */

typedef enum ntscrt_pixel_format {
    NTSCRT_RGBA8 = 0,
    NTSCRT_BGRA8 = 1,
    NTSCRT_RGBA16 = 2,   /* unorm16 */
    NTSCRT_RGBA16F = 3,
    NTSCRT_RGBA32F = 4   /* Resolve OFX default */
} ntscrt_pixel_format;

/* CPU image. `data` points at the first byte of the TOP row. Bottom-up hosts
 * (OpenFX) pass a pointer to their last row and a negative row_bytes.
 * Float/16-bit inputs are converted to the pipeline's internal 8-bit BGRA at
 * the boundary (clamped, no transfer-function change). Alpha: see the look's
 * "alpha" mode and ntscrt_render_args.source_premultiplied. */
typedef struct ntscrt_image {
    int32_t width;
    int32_t height;
    ptrdiff_t row_bytes;
    ntscrt_pixel_format format;
    void *data;
} ntscrt_image;

typedef enum ntscrt_gpu_backend {
    NTSCRT_GPU_AUTO = 0,   /* D3D11 on Windows, Metal on macOS */
    NTSCRT_GPU_D3D11 = 1,
    NTSCRT_GPU_METAL = 2
} ntscrt_gpu_backend;

typedef struct ntscrt_create_info {
    uint32_t abi_version;          /* = NTSCRT_ABI_VERSION */
    ntscrt_gpu_backend backend;
    /* Optional: share the host's device. Metal: id<MTLDevice>, id<MTLCommandQueue>.
     * D3D11: ID3D11Device*, NULL. NULL = core creates its own device. */
    void *native_device;
    void *native_queue;
    /* Directory holding bundled resources: shaders/ (slang-shaders subset),
     * presets/. UTF-8. */
    const char *resource_dir;
} ntscrt_create_info;

/* ---- temporal callbacks ------------------------------------------------ */

/* Fetch the effect's SOURCE (pre-effect) frame at `frame_index` (same frame
 * numbering as ntscrt_render_args.frame_index). Return NULL if unavailable
 * (before clip start, host refused). The image stays valid until
 * release_source is called with it. */
typedef const ntscrt_image *(*ntscrt_fetch_source_fn)(void *user, int64_t frame_index);
typedef void (*ntscrt_release_source_fn)(void *user, const ntscrt_image *image);

/* Look JSON (same schema as ntscrt_render_args.look_json) as the host's
 * keyframes evaluate it at `frame_index`. Return NULL to mean "same as the
 * current frame". The string must stay valid until the next call or the end
 * of the render call, whichever comes first. */
typedef const char *(*ntscrt_look_at_fn)(void *user, int64_t frame_index);

typedef struct ntscrt_render_args {
    /* Frame number relative to the effect/clip start (0 = first frame). */
    int64_t frame_index;
    double fps;
    /* Full look at this frame, upstream preset format (see docs/params.md).
     * The "timeline" and "view" keys are ignored. */
    const char *look_json;
    /* Screen Loop settings ("kind": "ntscrt-video-feedback"), or NULL = off. */
    const char *feedback_json;
    /* Host render scale (proxy/preview), 1.0 = full res. Pixel-size params
     * are scaled by it so previews match final renders. */
    double render_scale;
    /* Seed added to every random stream so two instances on one timeline can
     * differ. 0 = upstream behaviour. */
    uint64_t seed;

    ntscrt_fetch_source_fn fetch_source;     /* may be NULL */
    ntscrt_release_source_fn release_source; /* required if fetch_source set */
    ntscrt_look_at_fn look_at;               /* may be NULL */
    void *user;

    /* Alpha convention of the source images AND the destination (hosts keep
     * the output's premultiplication equal to the source's): 1 = RGB is
     * premultiplied by alpha, 0 = straight. Irrelevant for opaque sources.
     * How alpha is used comes from the look's "alpha": {"mode": "clip"|"spill"}
     * (docs/params.md). */
    int32_t source_premultiplied;
} ntscrt_render_args;

/* ---- API ---------------------------------------------------------------- */

NTSCRT_API uint32_t ntscrt_abi_version(void);

/* JSON describing every host-exposed parameter (docs/params.md). Static
 * storage; never free. */
NTSCRT_API const char *ntscrt_param_schema_json(void);

/* Bundled presets as a JSON array [{ "name": "...", "kind": "look"|"feedback",
 * "json": {...} }]. Static storage after first call. */
NTSCRT_API const char *ntscrt_bundled_presets_json(ntscrt_instance *inst);

NTSCRT_API ntscrt_status ntscrt_create(const ntscrt_create_info *info, ntscrt_instance **out);
NTSCRT_API void ntscrt_destroy(ntscrt_instance *inst);

/* Thread safety: one render at a time per instance. Hosts that render
 * concurrently create one instance per thread or serialise. */
NTSCRT_API ntscrt_status ntscrt_render_cpu(ntscrt_instance *inst,
                                           const ntscrt_render_args *args,
                                           const ntscrt_image *src,
                                           ntscrt_image *dst);

/* macOS only. Textures are id<MTLTexture> (BGRA8/RGBA8/RGBA16F/RGBA32F,
 * top row first), on the device given at create time; src must be readable
 * by shaders and any work producing it already COMMITTED and complete (the
 * core reads it back on its own command buffers for the CPU stages). The
 * final write into dst is encoded on `command_buffer` (id<MTLCommandBuffer>,
 * the host commits it) if non-NULL, else on the instance's queue and waited
 * on. dst need not be shader-writable (a blit is used then). Output alpha
 * follows the look's alpha mode (see ntscrt_render_args.source_premultiplied). */
NTSCRT_API ntscrt_status ntscrt_render_metal(ntscrt_instance *inst,
                                             const ntscrt_render_args *args,
                                             void *src_texture,
                                             void *dst_texture,
                                             void *command_buffer);

/* Drop cached temporal state (call on seek far away, param reset, purge). */
NTSCRT_API void ntscrt_flush_caches(ntscrt_instance *inst);

/* Drop only some caches (bits below), e.g. after an edit that can only change
 * one stage. Source frames are cached by frame, render scale, size, alpha mode
 * and crop, so edits to those never need NTSCRT_CACHE_SOURCE; the Glitch
 * receiver checks its own history for edits that apply to every frame alike,
 * so it only needs flushing when keyframed values change. */
#define NTSCRT_CACHE_SOURCE     0x01u /* converted + cropped source frames */
#define NTSCRT_CACHE_DIGITAL    0x02u /* Digital codec state */
#define NTSCRT_CACHE_GLITCH     0x04u /* Glitch receiver simulation */
#define NTSCRT_CACHE_CRT        0x08u /* CRT feedback history, failed-preset memo */
#define NTSCRT_CACHE_SCREENLOOP 0x10u /* Screen Loop history and its CRT */
#define NTSCRT_CACHE_ALL        0xFFFFFFFFu
NTSCRT_API void ntscrt_flush_caches_mask(ntscrt_instance *inst, uint32_t which);

/* Human-readable message for the last non-OK status on this instance. */
NTSCRT_API const char *ntscrt_last_error(ntscrt_instance *inst);

#ifdef __cplusplus
}
#endif
#endif /* NTSCRT_H */
