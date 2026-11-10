// ntsc-dump — prints ntsc-rs's settings descriptors and the NTSC stage's
// house-default settings as one JSON object:
//   {"descriptors": [...], "defaults": {...}}
// tools/gen_schema.py reads this (snapshot: core/params/ntsc_descriptors.json).
//
//   ntsc-dump [out.json]     (stdout when no path is given)
#include <cstdio>
#include <fstream>

#include "stages/ntsc/ntsc_internal.h"
#include "stages/stages.h"

int main(int argc, char **argv) {
    using ntscrt::json;
    json out;
    out["descriptors"] = json::parse(ntscrt::NtscStage::descriptors_json());
    out["defaults"] = json::parse(ntscrt::ntsc_effective_settings_json(json::object()));
    std::string s = out.dump(2) + "\n";
    if (argc > 1) {
        std::ofstream f(argv[1], std::ios::binary);
        if (!f) { std::fprintf(stderr, "cannot write %s\n", argv[1]); return 1; }
        f << s;
    } else {
        std::fwrite(s.data(), 1, s.size(), stdout);
    }
    return 0;
}
