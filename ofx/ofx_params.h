// ofx_params.h — host-independent model of the core param schema
// (docs/params.md) used by the OpenFX plugin: parse the schema, map ids to
// host param names, build look/feedback JSON from values and pull values out
// of preset JSON. Values travel as nlohmann::json of the param's natural type
// (bool, integer, double, the choice option's value, colour array/hex string).
#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ntscrt_ofx {
using json = nlohmann::json;

enum class PType { Bool, Int, Float, Choice, Color };

// "grey out / hide unless": value at `path` is truthy, or == equals if given.
struct Cond {
    std::string path;
    bool has_equals = false;
    json equals;
};

struct GroupDef {
    std::string id, name, label, parent;  // name = host param name; parent = group id or ""
};

struct ParamDef {
    std::string id, name, group, label, hint;
    PType type = PType::Float;
    json def;  // coerced default
    bool has_min = false, has_max = false, has_dmin = false, has_dmax = false, has_step = false;
    double min = 0, max = 0, dmin = 0, dmax = 0, step = 0;
    std::vector<std::pair<json, std::string>> options;  // choice: (value, label)
    std::string path;   // JSON pointer, without "fb:" prefix
    bool feedback = false;  // path was "fb:/..."
    bool animatable = true;
    bool in_presets = true;  // schema "preset": false = presets never set or reset it
    std::optional<Cond> enabled_if, visible_if;
    int color_n = 3;
    bool color_hex = false;
};

struct Schema {
    std::vector<GroupDef> groups;
    std::vector<ParamDef> params;
    std::map<std::string, size_t> by_look_path, by_fb_path;  // path -> params index

    static Schema parse(const char *schema_json);  // throws on malformed JSON
    bool has_feedback() const { return !by_fb_path.empty(); }
    // Param that writes `path` (a condition path: "/x" for look, "fb:/x" for feedback).
    const ParamDef *owner(const std::string &cond_path) const;
};

struct Preset {
    std::string name;
    bool feedback = false;
    json data;
};
// Presets in menu order: released presets first, in the order they shipped
// (released_preset_order), then newer ones alphabetically, so the index a
// saved project holds keeps naming the same preset.
std::vector<Preset> parse_presets(const char *presets_json);
const std::vector<std::string> &released_preset_order();

// Host param name for a schema id: [A-Za-z0-9_] only.
std::string host_name(const std::string &id);

// Coerce an arbitrary JSON value into the param's value type. nullopt if impossible.
std::optional<json> coerce(const ParamDef &p, const json &v);
// Choice option index for a value (-1 if none).
int choice_index(const ParamDef &p, const json &value);
// Colour value <-> host components (RGB or RGBA doubles).
std::vector<double> color_components(const ParamDef &p, const json &value);
json color_value(const ParamDef &p, const std::vector<double> &c);

// value getter for a param (by index into Schema::params)
using ValueFn = std::function<json(size_t)>;

// Build look + feedback JSON. feedback is null when the schema has no fb: params.
void build_look(const Schema &s, const ValueFn &value, json &look, json &feedback);

// Value a preset assigns to param p, or nullopt if the preset doesn't apply to p's
// side (look preset vs feedback preset). Missing keys yield the param default
// (feedback "enabled" yields true so choosing a Screen Loop preset turns it on).
std::optional<json> preset_value(const ParamDef &p, const json &preset, bool feedback_preset);

bool cond_true(const Schema &s, const Cond &c, const ValueFn &value);

bool is_feedback_preset(const json &j);
}  // namespace ntscrt_ofx
