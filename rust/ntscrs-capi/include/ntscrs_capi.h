/*
 * ntscrs_capi.h — C declarations for rust/ntscrs-capi (src/lib.rs), a minimal
 * C ABI over the ntsc-rs core crate. Port of NTSCRT Vendor/ntscrs-capi; the
 * only addition is ntsc_process_frame_ex.
 *
 * All strings are UTF-8 and returned strings must be freed with
 * ntsc_string_free.
 */
#ifndef NTSCRS_CAPI_H
#define NTSCRS_CAPI_H

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

#ifdef __cplusplus
}
#endif
#endif /* NTSCRS_CAPI_H */
