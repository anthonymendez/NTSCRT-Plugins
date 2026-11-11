// mock_host.h — a minimal but real OpenFX host for tests: loads a .ofx binary,
// implements the property / param / image-effect / memory / thread / message
// suites over plain C++ containers, and records everything the plugin defines.
#pragma once

#include <ofxCore.h>
#include <ofxImageEffect.h>
#include <ofxParam.h>
#include <ofxProperty.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace mock {

using Value = std::variant<int, double, std::string, void *>;

struct PropSet {
    std::map<std::string, std::vector<Value>> props;
    void set(const std::string &k, std::vector<Value> v) { props[k] = std::move(v); }
    bool has(const std::string &k) const { return props.count(k) != 0; }
    int i(const std::string &k, int idx = 0) const;
    double d(const std::string &k, int idx = 0) const;
    std::string s(const std::string &k, int idx = 0) const;
    void *p(const std::string &k, int idx = 0) const;
    int dim(const std::string &k) const;
    OfxPropertySetHandle h() { return reinterpret_cast<OfxPropertySetHandle>(this); }
    // Strings handed out by propGetString must outlive the call.
    std::vector<std::unique_ptr<std::string>> str_pool;
};

struct Param {
    std::string type, name;
    PropSet props;
    std::vector<double> v;  // numeric components
    std::string str;        // string params
    int sets = 0;           // paramSetValue calls (plugin edits)
    // Keyframes: time -> components. Non-empty: getValueAtTime interpolates
    // linearly (held before the first / after the last), getNumKeys counts them.
    std::map<double, std::vector<double>> keys;
    long reads_at_time = 0;  // paramGetValueAtTime calls
};

struct ParamSet {
    PropSet props;
    std::vector<std::unique_ptr<Param>> params;  // definition order
    Param *find(const std::string &name) const;
};

struct Image {
    PropSet props;
    std::vector<float> px;  // RGBA32F, bottom-up rows
};

struct Clip {
    std::string name;
    PropSet props;
    // frames the host can deliver: time -> RGBA32F bottom-up buffer
    int width = 0, height = 0;
    std::map<int, std::vector<float>> frames;
    // Frame for any time 0..any_frame_last not in `frames` (long clips without the memory).
    std::vector<float> any_frame;
    int any_frame_last = -1;
    std::vector<int> fetched;  // times requested by the plugin
};

struct Effect {
    PropSet props;
    ParamSet params;
    std::map<std::string, std::unique_ptr<Clip>> clips;
    std::vector<std::string> messages;
    OfxImageEffectHandle h() { return reinterpret_cast<OfxImageEffectHandle>(this); }
};

class Host {
public:
    Host();
    ~Host();
    bool load(const std::string &path, std::string &err);
    int num_plugins() const;
    OfxPlugin *plugin(int i) const;

    OfxStatus action(OfxPlugin *p, const char *action, Effect *e, PropSet *in = nullptr, PropSet *out = nullptr);
    // Clones descriptor params (with default values) and clips into a new instance.
    std::unique_ptr<Effect> make_instance(const Effect &desc, const char *context);
    static void set_param_from_default(Param &p);

    OfxHost host{};
    PropSet host_props;
    std::vector<std::string> log;  // messages from the plugin
    // Called with every image the plugin releases (lets tests read rendered output).
    std::function<void(const Image &)> on_release;
    // Answer of the image-effect suite's abort() (default: never abort); `aborts` counts calls.
    std::function<int()> on_abort;
    long abort_calls = 0;

private:
    void *lib_ = nullptr;
    int (*get_num_)() = nullptr;
    OfxPlugin *(*get_plugin_)(int) = nullptr;
};

Host *current();
const std::vector<std::string> &missing_props();  // property names fetched but never set
}  // namespace mock
