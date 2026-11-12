// params.h — the host-independent parameter model both plug-ins use (OpenFX
// ofx/ntscrt_ofx.cpp, FxPlug fxplug/src/NTSCRTPlugIn.mm through
// fxplug/model/param_model.h). No OFX or Apple headers: unit-tested on every
// platform (tests/hostparams).
//
// It turns the core's param schema (docs/params.md) into param definitions,
// coerces values, builds the look / feedback JSON a frame renders with,
// evaluates enabled_if / visible_if, applies presets (timeline keys become
// keyframes, Digital video looks are migrated), choice "applies" and Digital
// video formats, lists the bundled presets in a stable menu order, and knows
// which core caches a param change can affect.
//
// Values travel as nlohmann::json in the param's LOOK type: bool, integer,
// double, the choice option's value (string or number), a colour as the
// schema's default spells it (hex string or [r, g, b(, a)]). A host whose
// controls store something else (menu index, RGB components) converts at its
// edge (choice_index / color_components).
#pragma once

#include <nlohmann/json.hpp>

#include "../../core/params/timeline.h"

#include <climits>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ntscrt::host {
using json = nlohmann::json;

enum class PType { Bool, Int, Float, Choice, Color };

// enabled_if / visible_if: the value at `path` (the param owning it) is truthy,
// or equals `equals` (numbers to 1e-9) when given. Schema spelling: a pointer
// string, or {"path", "equals"?}; "fb:/..." points into the feedback.
struct Cond {
    std::string path;
    bool has_equals = false;
    json equals;
};

struct Option {
    json value;          // written into the look
    std::string label;   // shown in the menu
    json applies;        // schema "applies": path -> value written when chosen (null = none)
};

struct GroupDef {
    std::string id, name, label, parent;  // name = portable host name ("grp_" + host_name(id)); parent = group id or ""
};

struct ParamDef {
    std::string id, name, group, label, hint;  // name = host_name(id)
    PType type = PType::Float;
    json def;  // the schema default, coerced
    bool has_min = false, has_max = false, has_dmin = false, has_dmax = false, has_step = false;
    double min = 0, max = 0, dmin = 0, dmax = 0, step = 0;
    std::vector<Option> options;  // choice
    std::string path;             // JSON pointer, without "fb:"
    bool feedback = false;        // path was "fb:/..." (feedback_json)
    bool animatable = true;
    bool in_presets = true;       // schema "preset": false = presets never set or reset it
    bool encode_number = false;   // bool written as 1/0 (schema "encode": "number")
    std::optional<Cond> enabled_if, visible_if;
    int color_n = 3;              // colour components (4 when the default has alpha)
    bool color_hex = false;       // colour written as "#rrggbb"
    bool has_applies() const;     // a choice with an option that applies values
};

struct Schema {
    std::vector<GroupDef> groups;
    std::vector<ParamDef> params;
    std::map<std::string, size_t> by_look_path, by_fb_path;  // path -> params index
    std::map<std::string, size_t> by_id;                     // id -> params index
    json constants = json::object();       // "constants": "/x" or "fb:/x" -> value, written into every look
    json digital_formats = json::array();  // "digital_formats"
    ntscrt::timeline::Rules timeline;      // how timeline keys interpolate

    static Schema parse(const json &schema);
    static Schema parse(const char *schema_json);  // throws on malformed JSON
    bool has_feedback() const { return !by_fb_path.empty(); }
    // The param writing a condition path ("/x" look, "fb:/x" feedback), or null.
    const ParamDef *owner(const std::string &cond_path) const;
    std::optional<size_t> index_of(const std::string &id) const;
};

// Host param name for a schema id: [A-Za-z0-9_] only, not starting with a digit.
std::string host_name(const std::string &id);

// ---- values ---------------------------------------------------------------
bool truthy(const json &v);
// Equal, numbers to 1e-9 (presets carry 0.029 and 0.028999999999999998); arrays element-wise.
bool same_value(const json &a, const json &b);
// Value at an RFC 6901 pointer, nullopt when absent or the pointer is bad.
std::optional<json> value_at(const json &doc, const std::string &pointer);
// Writes v at pointer (creating objects); false on a bad pointer.
bool set_at(json &doc, const std::string &pointer, const json &v);

// Any JSON spelling -> the param's value: numbers for bools, bools for numbers,
// numbers clamped to the schema's min / max (ints rounded), a choice's value or
// label, a colour as hex ("#rrggbb", "rrggbb", 8 digits) or a number array. nullopt if impossible.
std::optional<json> coerce(const ParamDef &p, const json &v);
// Choice option index of a value (numbers to 1e-9), -1 if none.
int choice_index(const ParamDef &p, const json &value);
// Colour value <-> components (color_n doubles; alpha 1 when missing).
std::vector<double> color_components(const ParamDef &p, const json &value);
json color_value(const ParamDef &p, const std::vector<double> &c);

// ---- the look a frame renders with ---------------------------------------
using ValueFn = std::function<json(size_t)>;  // param index -> its current (look-typed) value

struct Built {
    json look;
    json feedback;  // always an object when the schema has fb: params (Screen Loop off = "enabled": false); else null
};
// Upstream's file format as it is: constants, every param at its path (hidden
// params left out: CRT presets share /shader/params/<NAME> keys), encode-number
// bools as 1/0. Built from nothing: keys no param writes never reach the core.
Built build_look(const Schema &s, const ValueFn &value);
void build_look(const Schema &s, const ValueFn &value, json &look, json &feedback);

// enabled_if / visible_if on the params' values. No condition, or one on a path
// no param owns: true.
bool cond_true(const Schema &s, const Cond &c, const ValueFn &value);
bool is_enabled(const Schema &s, const ParamDef &p, const ValueFn &value);
bool is_visible(const Schema &s, const ParamDef &p, const ValueFn &value);

// Is Screen Loop on in a built feedback object? (The core's rule: a feedback
// object without "enabled" is on; null = off.)
bool screen_loop_on(const json &feedback);
// Does a stage that reads earlier frames or looks run (Digital video, Glitch, Screen Loop)?
bool temporal_stages_on(const json &look, const json &feedback);

// ---- presets ----------------------------------------------------------------
bool is_feedback_preset(const json &j);  // "kind": "ntscrt-video-feedback"
// Value a preset gives param p, or nullopt when the preset doesn't set p (the
// other half: look vs Screen Loop file, or schema "preset": false). Missing or
// unusable keys give the default, except a Screen Loop file's "enabled", which is
// on (upstream saves only loops that are on). Look presets are migrated first
// (core/stages/digital/migrate.h).
std::optional<json> preset_value(const ParamDef &p, const json &preset, bool feedback_preset);

// An animated look (upstream timeline keys) as host keyframes.
//
// The timeline plays from the effect's first frame (frame offset 0, the "origin") and loops
// every `pass` frames. The keyed stretch is [first, first + frames): whole passes from a pass
// boundary (so it plays the loop in phase with the origin), as many as kMaxPresetKeys keys allow
// (at least one, at most kMaxKeyedFrames frames), covering the frame `at` (the playhead when the
// preset is applied) and starting at the origin when that fits. Before it the timeline's first values hold, after it its last ones.
//
// Keys are sparse: numbers get a key where the per-frame curve bends (a piecewise-linear fit,
// within kKeyTolerance of the curve's swing, half a step for finely stepped CRT values, exact
// for integers and wherever the value holds), for hosts that interpolate linearly between keys
// (Resolve's Edit and Fusion pages: measured by tests/resolve "position"; FxPlug: keys made with
// linear segments). Bools and choices get keys where the value changes (old value the frame
// before, new one on it).
struct PresetKeys {
    int64_t pass = 0;    // frames one timeline pass spans at the host rate (0 = not animated)
    int64_t first = 0;   // first keyed frame (offset from the effect's first frame, a multiple of pass)
    int64_t frames = 0;  // frames keyed from `first`
    // Per look param the preset sets (param index): (frame offset from the effect's first
    // frame, value). One (0, v) entry = holds still: set v without keyframes.
    std::map<size_t, std::vector<std::pair<int64_t, json>>> keys;
    bool animated() const { return pass > 0; }
};
constexpr int64_t kMaxKeyedFrames = 3600;  // longest stretch keyed (the last values then hold)
// Keys a preset writes, about: the stretch is as many whole passes as fit (at least one). Writing
// keys costs the host time (Resolve: ~50 us each headless, more with its interface), 0.5.0-rc.2's
// 97,200 for one preset froze Resolve's interface for about a minute.
constexpr int64_t kMaxPresetKeys = 3000;
constexpr double kKeyTolerance = 0.005;     // sparse keys: max deviation, as a fraction of a value's swing
// fps: the host frame rate; available: frames from the effect's first frame to its last
// (<= 0 = unknown length); at: the frame to cover (offset from the first frame).
// Not animated (pass 0) without timeline keys.
// guards: also key the frames either side of each bend, for hosts that interpolate numbers with
// smooth splines (Resolve's Edit and Fusion pages and FxPlug's linear segments don't need them).
PresetKeys preset_keys(const Schema &s, const json &preset, double fps, int64_t available, int64_t at = 0,
                       bool guards = false);
// The value a host interpolating linearly between `keys` (sorted by frame) gives at frame n
// (held before the first / after the last; bools and choices step). Tests and diagnostics.
json interpolated_key_value(const ParamDef &p, const std::vector<std::pair<int64_t, json>> &keys, double n);
// How far sparse keys may put param p's value from the per-frame curve (factor x the
// guarantee for linear interpolation: kKeyTolerance of the param's display span, half a
// step for stepped CRT values, 0 for integers, bools and choices).
double key_tolerance(const ParamDef &p, double factor = 1.0);
// Do two looks (or feedbacks) agree within key_tolerance(factor) on every param's value, and
// exactly everywhere else? `where` says the first difference.
bool within_key_tolerance(const Schema &s, const json &a, const json &b, std::string *where = nullptr,
                          double factor = 1.0);
// The look the timeline gives frame n of the effect (the preset upgraded, keys applied).
json preset_look_at_frame(const Schema &s, const json &preset, int64_t n, double fps);

// What applying a preset (bundled, or a loaded file) writes into the host params.
//  - look preset: every look param presets set loses its keyframes and gets `look[i]`
//    (one entry = a plain value, more = keyframes at frame offsets); a "screen_loop"
//    object in it (our Save preset) also sets the Screen Loop params;
//  - Screen Loop file: only the Screen Loop params (`feedback`).
struct PresetPlan {
    bool replaces_look = false;  // false for a Screen Loop file
    PresetKeys look;             // look.keys: every look param presets set (see above)
    std::vector<std::pair<size_t, json>> feedback;  // Screen Loop params: (index, value)
};
PresetPlan plan_preset(const Schema &s, const json &preset, double fps, int64_t available, int64_t at = 0,
                       bool guards = false);

// ---- choice "applies", Digital video formats -------------------------------
// Values a Digital video format's (or an option's) "applies" map writes: (param, value).
// Object values (/digital/values) spread over the params below that path.
std::vector<std::pair<size_t, json>> applies_values(const Schema &s, const json &applies);
// What choosing option `value` of choice p writes (Horizontal resolution -> Width).
std::vector<std::pair<size_t, json>> option_applies(const Schema &s, size_t p, const json &value);
// After `edited` is set by hand to edited_value: the option choice c drops to ("Custom",
// the option without applies) because its chosen option set `edited` to something else.
std::optional<json> demoted_choice(const Schema &s, size_t c, const json &c_value, size_t edited,
                                   const json &edited_value);
// Everything a user edit of param `edited` writes next (value() reads the params after
// the edit): the chosen option's applies, then the choices that drop to Custom.
std::vector<std::pair<size_t, json>> edit_effects(const Schema &s, size_t edited, const ValueFn &value);

// ---- bundled presets and the Preset menus -----------------------------------
struct Preset {
    std::string name;
    bool feedback = false;  // a Screen Loop preset
    bool format = false;    // a Digital video format: data is its "applies" map
    json data;
};
// <resource_dir>/presets/**/*.json as ntscrt_bundled_presets_json returns it
// ([{name, kind, json}], by name), read without a core instance (no GPU device).
std::string bundled_presets_json(const std::string &resource_dir);
// Menu order: released presets first, in the order they shipped (released_preset_order),
// then newer ones alphabetically, so the index a saved project holds keeps naming the
// same preset. Digital video formats join the looks as "Digital video/<name>".
std::vector<Preset> parse_presets(const json &bundled, const json &digital_formats = json::array());
std::vector<Preset> parse_presets(const char *bundled_json, const json &digital_formats = json::array());
const std::vector<std::string> &released_preset_order();

// ---- Save preset --------------------------------------------------------------
// The file Save preset writes: the look, with the Screen Loop settings under "screen_loop"
// (upstream ignores the key; Load applies it).
json saved_preset(const json &look, const json &feedback);
// The upstream Screen Loop file written beside it while the loop is on, else nullopt.
std::optional<json> saved_screen_loop(const json &feedback);

// ---- host extras ----------------------------------------------------------------
// Seed: added to every random stream so two instances can differ; 0 = upstream behaviour.
constexpr int kSeedMin = 0, kSeedMax = INT_MAX, kSeedDisplayMax = 1000;
uint64_t render_seed(int64_t host_value);  // clamped to [kSeedMin, kSeedMax]

// Core caches (NTSCRT_CACHE_* bits) a change of param p can make stale. keyed: p has
// keyframes (a Glitch edit that holds for every frame is caught by the receiver itself).
uint32_t cache_mask(const ParamDef &p, bool keyed);

}  // namespace ntscrt::host
