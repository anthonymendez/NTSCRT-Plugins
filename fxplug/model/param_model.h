// param_model.h — the FxPlug side of the shared host param model.
//
// Everything about values, looks, presets, conditions and menus is the shared
// library (hostparams/ntscrt_host/params.h), which the OpenFX plug-in uses too, so a
// preset renders the same in Final Cut as in Resolve. This file only adds what is
// FxPlug's own: STABLE numeric parameter IDs (fxplug/param_ids.json), the control
// layout (nested subgroups), the value types FxPlug controls hold (menu index,
// RGB components) and the "plugin state" blob a render receives.
//
// Pure C++ (no FxPlug / Apple headers) so it is unit-tested on every platform
// (fxplug/tests/test_param_model.cpp). The Objective-C++ shim in fxplug/src
// only moves values between this model and the FxPlug APIs.
#pragma once

#include <nlohmann/json.hpp>

#include "ntscrt_host/debug_switch.h"
#include "ntscrt_host/file_log.h"
#include "ntscrt_host/params.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ntscrt_fx {

using nlohmann::json;
namespace host = ntscrt::host;
using host::FileLog;
using Kind = host::PType;

// ---- fixed host-extra parameter IDs (never change; FxPlug range is [1, 9998]) ----
enum HostParamID : uint32_t {
    kTopGroup = 1,          // "NTSCRT" group wrapping everything (published by the Motion template)
    kPresetMenu = 2,        // bundled look presets
    kLoadPreset = 3,        // push button
    kSavePreset = 4,        // push button
    kScreenLoopMenu = 5,    // Off, (Custom), bundled Screen Loop presets: sets the Screen Loop params
    kSeed = 6,              // int
    kHistoryFrames = 7,     // int: earlier source frames scheduled for temporal stages
    kBaseLook = 8,          // hidden string, unused since 0.5.0 (looks are built from the params alone)
    kBaseFeedback = 9,      // hidden string, unused since 0.5.0
    kExtrasGroup = 10,      // "Preset & timing" subgroup
    kDebugLogging = 11,     // toggle: log every frame (diagnostics; never in the look or presets)
    kOpenLogs = 12,         // push button: show the log folder in Finder
    kDebugAll = 13,         // push button: debug logging for all NTSCRT effects on / off (host::DebugSwitch)
};
constexpr uint32_t kLastHostID = 99;  // host extras live in [1, 99]
constexpr uint32_t kFirstMappedID = 100;     // param_ids.json assigns [100, 4999]
constexpr uint32_t kLastMappedID = 4999;
constexpr uint32_t kFirstFallbackID = 5000;  // unmapped schema entries hash into [5000, 8999]
constexpr uint32_t kLastFallbackID = 8999;

// Screen Loop menu entries before the bundled presets.
constexpr int kScreenLoopOff = 0, kScreenLoopCustom = 1, kFirstScreenLoopPreset = 2;

// A schema param as an FxPlug control: the shared definition plus its host id and slider range.
struct Param : host::ParamDef {
    size_t index = 0;      // into Model::schema.params
    uint32_t host_id = 0;
    double ui_min = 0, ui_max = 1, slider_min = 0, slider_max = 1, ui_step = 0.01;  // FxPlug sliders need finite ranges
};

struct Group {
    std::string id, label, parent;
    uint32_t host_id = 0;
};

struct UiItem {
    enum Type { StartGroup, EndGroup, Control } type;
    size_t index;  // into groups (StartGroup) or params (Control)
};

// Host value of a param: bool, int, double, choice option index (int), colour [r, g, b] (0..1).
using HostGetter = std::function<json(const Param &)>;
using HostValues = std::map<uint32_t, json>;  // host id -> host value

// A preset as FxPlug writes it (host::PresetPlan in host ids and host values).
struct HostPlan {
    bool replaces_look = false;
    int64_t pass = 0, first = 0, frames = 0;  // animated: frames per timeline pass, the keyed stretch
    // Every look param presets set (their keyframes are cleared first): one (0, v) entry =
    // a plain value; more = keyframes at frame offsets from the effect's first frame.
    std::map<uint32_t, std::vector<std::pair<int64_t, json>>> look;
    HostValues feedback;  // Screen Loop params
    bool animated() const { return pass > 0; }
};

class Model {
public:
    // id_map: fxplug/param_ids.json ({"ids": {"group:<id>": n, "<param id>": n}}).
    static Model from_schema(const json &schema, const json &id_map, std::vector<std::string> *warnings);

    host::Schema schema;
    std::vector<Group> groups;
    std::vector<Param> params;   // parallel to schema.params
    std::vector<UiItem> layout;  // creation order: groups nested by "parent", params in schema order

    const Param *find_host(uint32_t host_id) const;
    const Param *find(const std::string &id) const;

    json default_host_value(const Param &p) const;
    json to_look_value(const Param &p, const json &host) const;
    // Look-typed (or any preset spelling of a) value -> host value (nullopt if it can't be represented).
    std::optional<json> to_host_value(const Param &p, const json &look_value) const;
    HostValues to_host_values(const std::vector<std::pair<size_t, json>> &vals) const;
    host::ValueFn look_values(const HostGetter &get) const;  // param index -> look value

    // The look and feedback a frame renders with (host::build_look; feedback always an
    // object, "enabled" off when Screen Loop is off).
    host::Built build(const HostGetter &get) const;
    bool is_enabled(const Param &p, const HostGetter &get) const;
    bool is_visible(const Param &p, const HostGetter &get) const;

    // Bundled presets (host::parse_presets): the look Preset menu (looks, then Digital
    // video formats) and the Screen Loop presets, both in the stable released order.
    std::vector<host::Preset> look_menu(const json &bundled) const;
    std::vector<host::Preset> loop_menu(const json &bundled) const;
    // What applying a preset writes; fps = the host frame rate, available = frames from
    // the effect's first frame to its last (<= 0 = unknown).
    // at: the frame (offset from the first) the keyed stretch must cover, e.g. the playhead.
    HostPlan plan_preset(const json &preset, double fps, int64_t available, int64_t at = 0) const;
    // Host values a Digital video format's (or an option's) "applies" map writes.
    HostValues applies_host_values(const json &applies) const;
    // What a user edit of control `edited` writes next: its option's applies, choices dropping to Custom.
    HostValues edit_effects(const Param &edited, const HostGetter &get) const;
    // The Screen Loop menu's Off / (Custom) entries: the "Screen Loop" switch.
    HostValues screen_loop_switch(bool on) const;
    // Core caches a change of host parameter `pid` can make stale (0 = none). keyed: it has keyframes.
    uint32_t cache_mask(uint32_t pid, bool keyed) const;
};

uint32_t fallback_id(const std::string &key, const std::set<uint32_t> &used);

// ---- FxPlug plugin state (pluginState NSData) ----
struct RenderState {
    int64_t frame = 0;
    double fps = 30.0;
    int64_t frame_duration_value = 1001;  // CMTime of one timeline frame
    int32_t frame_duration_timescale = 30000;
    int history = 0;                     // earlier frames scheduled (sourceImages[1..history])
    std::string look;                    // look JSON at this frame
    std::string feedback;                // feedback JSON (empty = none)
    uint64_t seed = 0;
    std::map<int64_t, std::string> past_looks;  // frame -> look JSON (only frames that differ)
    bool debug = false;                  // "Debug Logging" on: log this frame in detail

    std::string serialize() const;
    static std::optional<RenderState> parse(const std::string &bytes);
};

// Frame index from a time offset (both as value/timescale rationals): round(offset / frame).
int64_t frame_from_offset(int64_t offset_value, int32_t offset_timescale, int64_t frame_value, int32_t frame_timescale);

}  // namespace ntscrt_fx
