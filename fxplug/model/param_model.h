// param_model.h — host-independent half of the FxPlug plug-in.
//
// Turns ntscrt_param_schema_json() (docs/params.md) into an ordered list of
// host controls with STABLE numeric FxPlug parameter IDs, converts between
// host values and look-JSON values, assembles the per-frame look /
// feedback JSON and (de)serialises the FxPlug "plugin state" blob.
//
// Pure C++ (no FxPlug / Apple headers) so it is unit-tested on every platform
// (fxplug/tests/test_param_model.cpp). The Objective-C++ shim in fxplug/src
// only moves values between this model and the FxPlug APIs.
#pragma once

#include <nlohmann/json.hpp>

#include "../../core/params/timeline.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ntscrt_fx {

using nlohmann::json;

// ---- fixed host-extra parameter IDs (never change; FxPlug range is [1, 9998]) ----
enum HostParamID : uint32_t {
    kTopGroup = 1,          // "NTSCRT" group wrapping everything (published by the Motion template)
    kPresetMenu = 2,        // bundled look presets
    kLoadPreset = 3,        // push button
    kSavePreset = 4,        // push button
    kScreenLoopMenu = 5,    // Off + bundled feedback presets
    kSeed = 6,              // int
    kHistoryFrames = 7,     // int: earlier source frames scheduled for temporal stages
    kBaseLook = 8,          // hidden string: JSON the params are overlaid on (from preset/file)
    kBaseFeedback = 9,      // hidden string: same for Screen Loop
    kExtrasGroup = 10,      // "Preset & timing" subgroup
    kDebugLogging = 11,     // toggle: log every frame (diagnostics; never in the look or presets)
    kOpenLogs = 12,         // push button: show the log folder in Finder
};
constexpr uint32_t kLastHostID = 99;  // host extras live in [1, 99]
constexpr uint32_t kFirstMappedID = 100;     // param_ids.json assigns [100, 4999]
constexpr uint32_t kLastMappedID = 4999;
constexpr uint32_t kFirstFallbackID = 5000;  // unmapped schema entries hash into [5000, 8999]
constexpr uint32_t kLastFallbackID = 8999;

enum class Kind { Bool, Int, Float, Choice, Color };

struct Option {
    json value;
    std::string label;
    json applies;  // schema "applies": path -> value written when this option is chosen (null = none)
};

struct Param {
    std::string id, group, label, hint;
    Kind kind = Kind::Float;
    json def;  // schema default (look-JSON representation)
    double min = 0, max = 1, slider_min = 0, slider_max = 1, step = 0.01;
    std::vector<Option> options;
    bool animatable = true;
    bool in_presets = true;  // schema "preset": false = presets never set or reset it
    bool feedback = false;   // path was "fb:/..." -> writes into feedback_json
    bool encode_number = false;  // bool written as 1/0 (schema "encode": "number")
    std::string pointer;     // RFC 6901 pointer (without "fb:")
    std::string enabled_if;  // pointer; may start with "fb:"
    std::string visible_path;
    json visible_equals;
    bool visible_truthy = false;  // visible_if without "equals": shown while the value there is truthy
    uint32_t host_id = 0;
};

struct Group {
    std::string id, label, parent;
    uint32_t host_id = 0;
};

// One entry of the look Preset menu: a bundled look, or a Digital video format
// (upstream 0.20 lists the formats in its Preset menu, "Digital video" section).
struct MenuPreset {
    std::string name;    // "Clean VHS", "Digital video/MiniDV"
    bool format = false;  // data = the format's "applies" map (apply with applies_host_values)
    json data;            // look preset JSON
};

// An animated preset (upstream "timeline" keys) as host keyframes.
struct PresetKeys {
    int64_t pass = 0;    // frames one timeline pass spans at the host rate (0 = not animated)
    int64_t frames = 0;  // frames keyed from the effect's first frame
    // host id -> (frame offset from the effect's first frame, host value). A single
    // (0, v) entry = holds still: set v without keyframes. Bool / choice params get keys
    // where the value changes (old value the frame before, new one on it), the others one on every frame.
    std::map<uint32_t, std::vector<std::pair<int64_t, json>>> keys;
    bool animated() const { return pass > 0; }
};
constexpr int64_t kMaxKeyedFrames = 3600;  // longest stretch keyed (the last value then holds)

struct UiItem {
    enum Type { StartGroup, EndGroup, Control } type;
    size_t index;  // into groups (StartGroup) or params (Control)
};

class Model {
public:
    // id_map: fxplug/param_ids.json ({"ids": {"group:<id>": n, "<param id>": n}}).
    static Model from_schema(const json &schema, const json &id_map, std::vector<std::string> *warnings);

    std::vector<Group> groups;
    std::vector<Param> params;
    std::vector<UiItem> layout;  // creation order: groups nested by "parent", params in schema order
    json constants = json::object();       // schema "constants" ("/version", "fb:/kind", ...): written into every look
    json digital_formats = json::array();  // schema "digital_formats"
    ntscrt::timeline::Rules timeline;      // how timeline keys interpolate

    const Param *find_host(uint32_t host_id) const;
    const Param *find(const std::string &id) const;

    // Host-side value of a param: bool, int, double, choice index (int), color [r,g,b] (0..1).
    json default_host_value(const Param &p) const;
    json to_look_value(const Param &p, const json &host) const;
    // Look-JSON value -> host value (nullopt if it can't be represented, e.g. unknown choice).
    std::optional<json> to_host_value(const Param &p, const json &look_value) const;

    // Overlay every param (get(p) returns its host value) onto base_look / base_feedback.
    // feedback_out is null when Screen Loop is off (feedback_on == false).
    json build_look(const json &base_look, const std::function<json(const Param &)> &get) const;
    std::optional<json> build_feedback(bool feedback_on, const json &base_feedback,
                                       const std::function<json(const Param &)> &get) const;

    // Host values for all look (feedback=false) or feedback (true) params from a preset file:
    // value at the param's path if present, else the schema default.
    std::map<uint32_t, json> host_values_from_preset(const json &preset, bool feedback) const;

    // Look Preset menu: the bundled looks of ntscrt_bundled_presets_json (an array of
    // {name, kind, json}; Screen Loop ones skipped) in that order, then the Digital video formats.
    std::vector<MenuPreset> look_menu(const json &bundled) const;
    // Host values a Digital video format's (or a choice option's) "applies" map writes;
    // object values (/digital/values) spread over the params below that path.
    std::map<uint32_t, json> applies_host_values(const json &applies) const;
    // Host values choosing option `host_choice` (index) of choice p writes (e.g. Horizontal
    // resolution -> Width). Empty when the option sets nothing.
    std::map<uint32_t, json> option_host_values(const Param &p, const json &host_choice) const;
    // After the user edits `edited` by hand: the host value (option index) choice `c` should
    // drop to ("Custom") because its chosen option set `edited` to something else; else nullopt.
    std::optional<json> demoted_choice(const Param &c, const json &c_host, const Param &edited,
                                       const json &edited_host) const;
    // Keyframes for an animated look preset: fps = the host's frame rate, available =
    // frames from the effect's first frame to its last (<= 0 = unknown: one pass).
    // The host clears the keyframes of every look param first (presets replace the look).
    PresetKeys preset_keys(const json &preset, double fps, int64_t available) const;
    // The look the timeline gives frame n of the effect (the preset upgraded, keys applied).
    json preset_look_at_frame(const json &preset, int64_t n, double fps) const;

    // UI state for enabled_if / visible_if, evaluated on built look / feedback.
    static bool truthy(const json &v);
    bool is_enabled(const Param &p, const json &look, const json &feedback) const;
    bool is_visible(const Param &p, const json &look, const json &feedback) const;
};

uint32_t fallback_id(const std::string &key, const std::set<uint32_t> &used);
// Value at pointer, or nullptr json when absent / bad pointer.
json value_at(const json &doc, const std::string &pointer);
bool set_at(json &doc, const std::string &pointer, const json &v);

// Is a temporal stage (Digital codec, Glitch receiver, Screen Loop) active? Then the host
// must schedule earlier source frames.
bool needs_history(const json &look, const std::optional<json> &feedback);

// ---- FxPlug plugin state (pluginState NSData) ----
struct RenderState {
    int64_t frame = 0;
    double fps = 30.0;
    int64_t frame_duration_value = 1001;  // CMTime of one timeline frame
    int32_t frame_duration_timescale = 30000;
    int history = 0;                     // earlier frames scheduled (sourceImages[1..history])
    std::string look;                    // look JSON at this frame
    std::string feedback;                // empty = Screen Loop off
    uint64_t seed = 0;
    std::map<int64_t, std::string> past_looks;  // frame -> look JSON (only frames that differ)
    bool debug = false;                  // "Debug Logging" on: log this frame in detail

    std::string serialize() const;
    static std::optional<RenderState> parse(const std::string &bytes);
};

// ---- diagnostic log file (ntscrt-fxplug.log) ----
// Appends "<local time>.<ms> [<thread>] <msg>" lines, creating the folder; rotates the file
// to "<stem>.old<ext>" past max_bytes. Thread-safe, never throws. An empty path disables it.
class FileLog {
public:
    explicit FileLog(std::string path, uint64_t max_bytes = 1u << 20);
    const std::string &path() const { return path_; }
    std::string folder() const;  // parent directory ("" if none)
    void write(const std::string &msg) const;

private:
    std::string path_;
    uint64_t max_bytes_;
};

// Frame index from a time offset (both as value/timescale rationals): round(offset / frame).
int64_t frame_from_offset(int64_t offset_value, int32_t offset_timescale, int64_t frame_value, int32_t frame_timescale);

}  // namespace ntscrt_fx
