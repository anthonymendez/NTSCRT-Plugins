// fields.h — Digital video's One field down the chain (upstream 0.19,
// AppState.downscaleSpec / DownscaleSpec.lineHeight): with the stage on,
// Fields on One field and the Downscale on, the chain (NTSC, Glitch, CRT)
// gets the field — half the lines, each standing for two picture rows — so
// the CRT draws a scanline per field line (the 240p look). Screen Loop
// doesn't run Digital video, so it keeps square lines.
#pragma once

#include <algorithm>

#include "migrate.h"

namespace ntscrt::digital {

// Rows of the picture each chain line stands for (1 or 2) for a look whose
// Digital stage runs. Default Fields is One field (upstream 0.19).
inline int chain_line_height(const nlohmann::json &look) {
    if (!look.is_object()) return 1;
    auto d = look.find("digital");
    if (d == look.end() || !d->is_object() || !d->value("enabled", false)) return 1;
    auto ds = look.find("downscale");
    if (ds != look.end() && ds->is_object()) {
        auto e = ds->find("enabled");
        if (e != ds->end() && e->is_boolean() && !e->get<bool>()) return 1;
    }
    const nlohmann::json v = migrate::values_for(look);
    return migrate::number(v, "fields", 1) >= 0.5 ? 2 : 1;
}

// Lines the chain gets for a Downscale of `height` rows (upstream max(8, h / 2)).
inline int field_lines(int height, int line_height) {
    return line_height > 1 ? std::max(8, height / line_height) : height;
}

}  // namespace ntscrt::digital
