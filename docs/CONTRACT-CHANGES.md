# Contract changes log
Append one entry per change to a Wave-0 contract file: date, agent, file, change, reason.

- 2026-11-10, agent D, `core/gpu/gpu.h`: added `static const char *Device::last_create_error()`.
  Reason: `Device::create` must not throw when no GPU exists; it returns nullptr and this gives the reason
  (the façade can copy it into `ntscrt_last_error`). Also comment-only portability notes (RWTexture2D reads
  only for R32F, cbuffer layout, structured-buffer stride inference, BGRA8 storage fallback, Metal index map).
- 2026-11-10, agent D, `cmake/Shaders.cmake`: signature unchanged (`ntscrt_add_shaders(<target> <file.slang>...)`).
  Kernel names are `"<file stem>/<entry point>"`; every `[shader("compute")]` function is an entry point;
  stems must be unique per target.
