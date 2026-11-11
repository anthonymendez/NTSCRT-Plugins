// ofx_params.h — host-independent model of the core param schema
// (docs/params.md) used by the OpenFX plugin: parse the schema, map ids to
// host param names, build look/feedback JSON from values and pull values out
// of preset JSON. Values travel as nlohmann::json of the param's natural type
// (bool, integer, double, the choice option's value, colour array/hex string).
#pragma once

#include <nlohmann/json.hpp>

#include "../core/params/timeline.h"

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
    std::vector<json> option_applies;  // choice: per option, the schema's "applies" map (path -> value) or null
    std::string path;   // JSON pointer, without "fb:" prefix
    bool feedback = false;  // path was "fb:/..."
    bool animatable = true;
    bool in_presets = true;  // schema "preset": false = presets never set or reset it
    std::optional<Cond> enabled_if, visible_if;
    int color_n = 3;
    bool color_hex = false;
    bool encode_number = false;  // bool stored as 1/0 in the look (schema "encode": "number")
};

struct Schema {
    std::vector<GroupDef> groups;
    std::vector<ParamDef> params;
    std::map<std::string, size_t> by_look_path, by_fb_path;  // path -> params index
    json constants = json::object();  // schema "constants": path ("/x" or "fb:/x") -> value, written into every look
    json digital_formats = json::array();  // schema "digital_formats"
    ntscrt::timeline::Rules timeline;      // how timeline keys interpolate

    static Schema parse(const char *schema_json);  // throws on malformed JSON
    bool has_feedback() const { return !by_fb_path.empty(); }
    // Param that writes `path` (a condition path: "/x" for look, "fb:/x" for feedback).
    const ParamDef *owner(const std::string &cond_path) const;
};

struct Preset {
    std::string name;
    bool feedback = false;
    bool format = false;  // a Digital video format: data is its "applies" map
    json data;
};
// Presets in menu order: released presets first, in the order they shipped
// (released_preset_order), then newer ones alphabetically, so the index a
// saved project holds keeps naming the same preset. Digital video formats
// (schema digital_formats) join the look menu as "Digital video/<name>".
std::vector<Preset> parse_presets(const char *presets_json, const json &digital_formats = json::array());
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

// ---- choice "applies" (e.g. Horizontal resolution presets set the width) and Digital formats
// Values to write when param p's option `value` is chosen: (param index, value). Empty if none.
std::vector<std::pair<size_t, json>> option_applies(const Schema &s, size_t p, const json &value);
// Values a Digital video format's "applies" map writes: (param index, value). Object values
// (/digital/values) are spread over the params below that path.
std::vector<std::pair<size_t, json>> applies_values(const Schema &s, const json &applies);
// After the user sets the Width by hand: the Horizontal resolution label that still fits
// ("Custom" when the chosen preset's width differs), or nullopt to leave it.
std::optional<json> demoted_choice(const Schema &s, size_t choice_param, const json &choice_value, size_t edited,
                                   const json &edited_value);

// ---- animated presets (upstream timeline keys -> host keyframes)
struct PresetKeys {
    int64_t pass = 0;    // frames one timeline pass spans at the host rate (0 = not animated)
    int64_t frames = 0;  // frames written, from the effect's first frame
    // Per look param the preset sets: its keyframes (frame offset, value), or one
    // (0, value) pair when it holds still. Stepped params (bool, choice) get keys
    // where the value changes (old value the frame before, new value on it); the others a key on every frame (exact on every
    // frame whatever interpolation the host uses between keys).
    std::map<size_t, std::vector<std::pair<int64_t, json>>> keys;
    bool animated() const { return pass > 0; }
};
// Longest stretch keyed when the effect is longer (the last value then holds).
constexpr int64_t kMaxKeyedFrames = 3600;
// fps: the host frame rate; available: frames from the effect's first frame to its
// last (<= 0 = unknown: one pass). Not animated (pass 0) without timeline keys.
PresetKeys preset_keys(const Schema &s, const json &preset, double fps, int64_t available);
// The look the timeline gives frame n (look preset, upgraded), as the host params then hold it.
json preset_look_at_frame(const Schema &s, const json &preset, int64_t n, double fps);

bool is_feedback_preset(const json &j);
}  // namespace ntscrt_ofx
