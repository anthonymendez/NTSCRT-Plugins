#include "mock_host.h"

#include <ofxMemory.h>
#include <ofxMessage.h>
#include <ofxMultiThread.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

#ifdef _WIN32
// A crash in a test run says where it happened (module + offset) instead of a bare SEGFAULT.
static LONG WINAPI report_crash(EXCEPTION_POINTERS *e) {
    void *addr = e && e->ExceptionRecord ? e->ExceptionRecord->ExceptionAddress : nullptr;
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(addr), &m))
        GetModuleFileNameA(m, name, MAX_PATH);
    std::fprintf(stderr, "[mock host] crash 0x%08lx at %p (%s+0x%llx, thread %lu)\n",
                 e && e->ExceptionRecord ? e->ExceptionRecord->ExceptionCode : 0ul, addr, name,
                 m ? (unsigned long long)(static_cast<char *>(addr) - reinterpret_cast<char *>(m)) : 0ull,
                 GetCurrentThreadId());
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
static const bool g_crash_handler = (SetUnhandledExceptionFilter(report_crash), true);
#endif

namespace mock {
namespace {
Host *g_host = nullptr;
PropSet *P(OfxPropertySetHandle h) { return reinterpret_cast<PropSet *>(h); }
Effect *E(OfxImageEffectHandle h) { return reinterpret_cast<Effect *>(h); }
ParamSet *PS(OfxParamSetHandle h) { return reinterpret_cast<ParamSet *>(h); }
Param *PA(OfxParamHandle h) { return reinterpret_cast<Param *>(h); }
Clip *C(OfxImageClipHandle h) { return reinterpret_cast<Clip *>(h); }

std::vector<std::string> g_missing;  // props the plugin asked for that nobody set
std::map<PropSet *, Image *> g_images;  // live images by their property handle

// ---- property suite ------------------------------------------------------
template <class T>
OfxStatus set_one(OfxPropertySetHandle h, const char *k, int idx, T v) {
    if (!h || idx < 0) return kOfxStatErrBadHandle;
    auto &vec = P(h)->props[k];
    if (int(vec.size()) <= idx) vec.resize(size_t(idx) + 1, Value(T{}));
    vec[size_t(idx)] = Value(v);
    return kOfxStatOK;
}
OfxStatus pSetPointer(OfxPropertySetHandle h, const char *k, int i, void *v) { return set_one<void *>(h, k, i, v); }
OfxStatus pSetString(OfxPropertySetHandle h, const char *k, int i, const char *v) {
    return set_one<std::string>(h, k, i, v ? v : "");
}
OfxStatus pSetDouble(OfxPropertySetHandle h, const char *k, int i, double v) { return set_one<double>(h, k, i, v); }
OfxStatus pSetInt(OfxPropertySetHandle h, const char *k, int i, int v) { return set_one<int>(h, k, i, v); }
OfxStatus pSetPointerN(OfxPropertySetHandle h, const char *k, int n, void *const *v) {
    P(h)->props[k].clear();
    for (int i = 0; i < n; ++i) set_one<void *>(h, k, i, v[i]);
    return kOfxStatOK;
}
OfxStatus pSetStringN(OfxPropertySetHandle h, const char *k, int n, const char *const *v) {
    P(h)->props[k].clear();
    for (int i = 0; i < n; ++i) set_one<std::string>(h, k, i, v[i] ? v[i] : "");
    return kOfxStatOK;
}
OfxStatus pSetDoubleN(OfxPropertySetHandle h, const char *k, int n, const double *v) {
    P(h)->props[k].clear();
    for (int i = 0; i < n; ++i) set_one<double>(h, k, i, v[i]);
    return kOfxStatOK;
}
OfxStatus pSetIntN(OfxPropertySetHandle h, const char *k, int n, const int *v) {
    P(h)->props[k].clear();
    for (int i = 0; i < n; ++i) set_one<int>(h, k, i, v[i]);
    return kOfxStatOK;
}

const Value *get_one(OfxPropertySetHandle h, const char *k, int idx, OfxStatus &st) {
    if (!h) { st = kOfxStatErrBadHandle; return nullptr; }
    auto it = P(h)->props.find(k);
    if (it == P(h)->props.end()) {
        g_missing.push_back(k);
        st = kOfxStatErrUnknown;
        return nullptr;
    }
    if (idx < 0 || idx >= int(it->second.size())) { st = kOfxStatErrBadIndex; return nullptr; }
    st = kOfxStatOK;
    return &it->second[size_t(idx)];
}
OfxStatus pGetPointer(OfxPropertySetHandle h, const char *k, int i, void **v) {
    OfxStatus st;
    const Value *x = get_one(h, k, i, st);
    if (!x) return st;
    if (auto p = std::get_if<void *>(x)) { *v = *p; return kOfxStatOK; }
    return kOfxStatErrValue;
}
OfxStatus pGetString(OfxPropertySetHandle h, const char *k, int i, char **v) {
    OfxStatus st;
    const Value *x = get_one(h, k, i, st);
    if (!x) return st;
    if (auto p = std::get_if<std::string>(x)) {
        P(h)->str_pool.push_back(std::make_unique<std::string>(*p));
        *v = P(h)->str_pool.back()->data();
        return kOfxStatOK;
    }
    return kOfxStatErrValue;
}
OfxStatus pGetDouble(OfxPropertySetHandle h, const char *k, int i, double *v) {
    OfxStatus st;
    const Value *x = get_one(h, k, i, st);
    if (!x) return st;
    if (auto p = std::get_if<double>(x)) { *v = *p; return kOfxStatOK; }
    if (auto p = std::get_if<int>(x)) { *v = *p; return kOfxStatOK; }
    return kOfxStatErrValue;
}
OfxStatus pGetInt(OfxPropertySetHandle h, const char *k, int i, int *v) {
    OfxStatus st;
    const Value *x = get_one(h, k, i, st);
    if (!x) return st;
    if (auto p = std::get_if<int>(x)) { *v = *p; return kOfxStatOK; }
    if (auto p = std::get_if<double>(x)) { *v = int(*p); return kOfxStatOK; }
    return kOfxStatErrValue;
}
OfxStatus pGetPointerN(OfxPropertySetHandle h, const char *k, int n, void **v) {
    for (int i = 0; i < n; ++i) if (OfxStatus s = pGetPointer(h, k, i, v + i)) return s;
    return kOfxStatOK;
}
OfxStatus pGetStringN(OfxPropertySetHandle h, const char *k, int n, char **v) {
    for (int i = 0; i < n; ++i) if (OfxStatus s = pGetString(h, k, i, v + i)) return s;
    return kOfxStatOK;
}
OfxStatus pGetDoubleN(OfxPropertySetHandle h, const char *k, int n, double *v) {
    for (int i = 0; i < n; ++i) if (OfxStatus s = pGetDouble(h, k, i, v + i)) return s;
    return kOfxStatOK;
}
OfxStatus pGetIntN(OfxPropertySetHandle h, const char *k, int n, int *v) {
    for (int i = 0; i < n; ++i) if (OfxStatus s = pGetInt(h, k, i, v + i)) return s;
    return kOfxStatOK;
}
OfxStatus pReset(OfxPropertySetHandle h, const char *k) {
    if (!h) return kOfxStatErrBadHandle;
    P(h)->props.erase(k);
    return kOfxStatOK;
}
OfxStatus pGetDimension(OfxPropertySetHandle h, const char *k, int *n) {
    if (!h) return kOfxStatErrBadHandle;
    auto it = P(h)->props.find(k);
    // Real hosts pre-create every descriptor property (multi-valued ones empty);
    // treat unknown names as empty so plugins can append to them.
    *n = it == P(h)->props.end() ? 0 : int(it->second.size());
    return kOfxStatOK;
}
OfxPropertySuiteV1 g_prop = {pSetPointer, pSetString, pSetDouble, pSetInt, pSetPointerN, pSetStringN,
                             pSetDoubleN, pSetIntN, pGetPointer, pGetString, pGetDouble, pGetInt,
                             pGetPointerN, pGetStringN, pGetDoubleN, pGetIntN, pReset, pGetDimension};

// ---- parameter suite -----------------------------------------------------
int n_components(const std::string &t) {
    if (t == kOfxParamTypeInteger || t == kOfxParamTypeDouble || t == kOfxParamTypeBoolean || t == kOfxParamTypeChoice)
        return 1;
    if (t == kOfxParamTypeRGB || t == kOfxParamTypeDouble3D || t == kOfxParamTypeInteger3D) return 3;
    if (t == kOfxParamTypeRGBA) return 4;
    if (t == kOfxParamTypeDouble2D || t == kOfxParamTypeInteger2D) return 2;
    return 0;
}
bool int_typed(const std::string &t) {
    return t == kOfxParamTypeInteger || t == kOfxParamTypeBoolean || t == kOfxParamTypeChoice ||
           t == kOfxParamTypeInteger2D || t == kOfxParamTypeInteger3D;
}
bool string_typed(const std::string &t) { return t == kOfxParamTypeString || t == kOfxParamTypeStrChoice; }

OfxStatus paramDefine(OfxParamSetHandle ps, const char *type, const char *name, OfxPropertySetHandle *props) {
    if (!ps) return kOfxStatErrBadHandle;
    if (PS(ps)->find(name)) return kOfxStatErrExists;
    auto p = std::make_unique<Param>();
    p->type = type;
    p->name = name;
    p->props.set(kOfxParamPropType, {std::string(type)});
    p->props.set(kOfxPropName, {std::string(name)});
    p->props.set(kOfxPropLabel, {std::string(name)});
    p->props.set(kOfxParamPropSecret, {0});
    p->props.set(kOfxParamPropEnabled, {1});
    p->props.set(kOfxParamPropParent, {std::string()});
    if (props) *props = p->props.h();
    PS(ps)->params.push_back(std::move(p));
    return kOfxStatOK;
}
OfxStatus paramGetHandle(OfxParamSetHandle ps, const char *name, OfxParamHandle *h, OfxPropertySetHandle *props) {
    if (!ps) return kOfxStatErrBadHandle;
    Param *p = PS(ps)->find(name);
    if (!p) return kOfxStatErrUnknown;
    if (h) *h = reinterpret_cast<OfxParamHandle>(p);
    if (props) *props = p->props.h();
    return kOfxStatOK;
}
OfxStatus paramSetGetPropertySet(OfxParamSetHandle ps, OfxPropertySetHandle *props) {
    if (!ps) return kOfxStatErrBadHandle;
    *props = PS(ps)->props.h();
    return kOfxStatOK;
}
OfxStatus paramGetPropertySet(OfxParamHandle h, OfxPropertySetHandle *props) {
    if (!h) return kOfxStatErrBadHandle;
    *props = PA(h)->props.h();
    return kOfxStatOK;
}
OfxStatus get_value(Param *p, va_list ap) {
    if (string_typed(p->type)) {
        *va_arg(ap, const char **) = p->str.c_str();
        return kOfxStatOK;
    }
    int n = n_components(p->type);
    for (int i = 0; i < n; ++i) {
        double v = i < int(p->v.size()) ? p->v[size_t(i)] : 0.0;
        if (int_typed(p->type)) *va_arg(ap, int *) = int(v);
        else *va_arg(ap, double *) = v;
    }
    return kOfxStatOK;
}
OfxStatus set_value(Param *p, va_list ap) {
    ++p->sets;
    if (string_typed(p->type)) {
        const char *s = va_arg(ap, const char *);
        p->str = s ? s : "";
        return kOfxStatOK;
    }
    int n = n_components(p->type);
    p->v.resize(size_t(n));
    for (int i = 0; i < n; ++i) p->v[size_t(i)] = int_typed(p->type) ? double(va_arg(ap, int)) : va_arg(ap, double);
    return kOfxStatOK;
}
OfxStatus paramGetValue(OfxParamHandle h, ...) {
    va_list ap;
    va_start(ap, h);
    OfxStatus s = get_value(PA(h), ap);
    va_end(ap);
    return s;
}
OfxStatus paramGetValueAtTime(OfxParamHandle h, OfxTime t, ...) {
    Param *p = PA(h);
    ++p->reads_at_time;
    std::vector<double> saved;
    if (!p->keys.empty()) {  // evaluate the curve at t into v for get_value
        saved = p->v;
        auto hi = p->keys.lower_bound(t);
        if (hi == p->keys.begin()) p->v = hi->second;
        else if (hi == p->keys.end()) p->v = std::prev(hi)->second;
        else if (hi->first == t) p->v = hi->second;
        else if (p->type == kOfxParamTypeBoolean || p->type == kOfxParamTypeChoice) p->v = std::prev(hi)->second;  // stepped
        else {
            auto lo = std::prev(hi);
            double a = (t - lo->first) / (hi->first - lo->first);
            p->v = lo->second;
            for (size_t i = 0; i < p->v.size() && i < hi->second.size(); ++i)
                p->v[i] = lo->second[i] + (hi->second[i] - lo->second[i]) * a;
        }
    }
    va_list ap;
    va_start(ap, t);
    OfxStatus s = get_value(p, ap);
    va_end(ap);
    if (!p->keys.empty()) p->v = saved;
    return s;
}
OfxStatus paramGetDerivative(OfxParamHandle, OfxTime, ...) { return kOfxStatErrUnsupported; }
OfxStatus paramGetIntegral(OfxParamHandle, OfxTime, OfxTime, ...) { return kOfxStatErrUnsupported; }
OfxStatus paramSetValue(OfxParamHandle h, ...) {
    va_list ap;
    va_start(ap, h);
    OfxStatus s = set_value(PA(h), ap);
    va_end(ap);
    return s;
}
OfxStatus paramSetValueAtTime(OfxParamHandle h, OfxTime t, ...) {
    Param *p = PA(h);
    va_list ap;
    va_start(ap, t);
    OfxStatus s = set_value(p, ap);
    va_end(ap);
    if (s == kOfxStatOK && !string_typed(p->type)) p->keys[t] = p->v;  // a keyframe, as hosts do
    ++p->key_sets;
    return s;
}
OfxStatus paramGetNumKeys(OfxParamHandle h, unsigned int *n) {
    *n = unsigned(PA(h)->keys.size());
    return kOfxStatOK;
}
OfxStatus paramGetKeyTime(OfxParamHandle, unsigned int, OfxTime *) { return kOfxStatErrBadIndex; }
OfxStatus paramGetKeyIndex(OfxParamHandle, OfxTime, int, int *) { return kOfxStatFailed; }
OfxStatus paramDeleteKey(OfxParamHandle, OfxTime) { return kOfxStatErrBadIndex; }
OfxStatus paramDeleteAllKeys(OfxParamHandle h) {
    PA(h)->keys.clear();
    ++PA(h)->key_clears;
    return kOfxStatOK;
}
OfxStatus paramCopy(OfxParamHandle, OfxParamHandle, OfxTime, const OfxRangeD *) { return kOfxStatErrUnsupported; }
int g_edit_depth = 0;
OfxStatus paramEditBegin(OfxParamSetHandle, const char *) { ++g_edit_depth; return kOfxStatOK; }
OfxStatus paramEditEnd(OfxParamSetHandle) { --g_edit_depth; return kOfxStatOK; }
OfxParameterSuiteV1 g_param = {paramDefine, paramGetHandle, paramSetGetPropertySet, paramGetPropertySet,
                               paramGetValue, paramGetValueAtTime, paramGetDerivative, paramGetIntegral,
                               paramSetValue, paramSetValueAtTime, paramGetNumKeys, paramGetKeyTime,
                               paramGetKeyIndex, paramDeleteKey, paramDeleteAllKeys, paramCopy,
                               paramEditBegin, paramEditEnd};

// ---- image effect suite --------------------------------------------------
OfxStatus getPropertySet(OfxImageEffectHandle e, OfxPropertySetHandle *p) {
    if (!e) return kOfxStatErrBadHandle;
    *p = E(e)->props.h();
    return kOfxStatOK;
}
OfxStatus getParamSet(OfxImageEffectHandle e, OfxParamSetHandle *p) {
    if (!e) return kOfxStatErrBadHandle;
    *p = reinterpret_cast<OfxParamSetHandle>(&E(e)->params);
    return kOfxStatOK;
}
OfxStatus clipDefine(OfxImageEffectHandle e, const char *name, OfxPropertySetHandle *p) {
    if (!e) return kOfxStatErrBadHandle;
    auto &c = E(e)->clips[name];
    if (!c) {
        c = std::make_unique<Clip>();
        c->name = name;
        c->props.set(kOfxPropType, {std::string(kOfxTypeClip)});
        c->props.set(kOfxPropName, {std::string(name)});
    }
    *p = c->props.h();
    return kOfxStatOK;
}
OfxStatus clipGetHandle(OfxImageEffectHandle e, const char *name, OfxImageClipHandle *h, OfxPropertySetHandle *p) {
    if (!e) return kOfxStatErrBadHandle;
    auto it = E(e)->clips.find(name);
    if (it == E(e)->clips.end()) return kOfxStatErrBadIndex;
    if (h) *h = reinterpret_cast<OfxImageClipHandle>(it->second.get());
    if (p) *p = it->second->props.h();
    return kOfxStatOK;
}
OfxStatus clipGetPropertySet(OfxImageClipHandle c, OfxPropertySetHandle *p) {
    if (!c) return kOfxStatErrBadHandle;
    *p = C(c)->props.h();
    return kOfxStatOK;
}
OfxStatus clipGetImage(OfxImageClipHandle ch, OfxTime t, const OfxRectD *, OfxPropertySetHandle *out) {
    Clip *c = C(ch);
    if (!c) return kOfxStatErrBadHandle;
    int ti = int(t);
    c->fetched.push_back(ti);
    auto it = c->frames.find(ti);
    const std::vector<float> *frame = it != c->frames.end() ? &it->second : nullptr;
    if (!frame && ti >= 0 && ti <= c->any_frame_last && !c->any_frame.empty()) frame = &c->any_frame;
    if (!frame) return kOfxStatFailed;
    auto *im = new Image;
    im->px = *frame;  // output clip frames are scratch buffers the plugin writes into
    if (c->name == kOfxImageEffectOutputClipName) im->px.assign(frame->size(), -1.f);
    PropSet &p = im->props;
    p.set(kOfxPropType, {std::string(kOfxTypeImage)});
    p.set(kOfxImageEffectPropPixelDepth, {c->props.s(kOfxImageEffectPropPixelDepth)});
    p.set(kOfxImageEffectPropComponents, {c->props.s(kOfxImageEffectPropComponents)});
    p.set(kOfxImageEffectPropPreMultiplication, {std::string(kOfxImageUnPreMultiplied)});
    p.set(kOfxImageEffectPropRenderScale, {1.0, 1.0});
    p.set(kOfxImagePropPixelAspectRatio, {1.0});
    p.set(kOfxImagePropData, {static_cast<void *>(im->px.data())});
    p.set(kOfxImagePropBounds, {0, 0, c->width, c->height});
    p.set(kOfxImagePropRegionOfDefinition, {0, 0, c->width, c->height});
    p.set(kOfxImagePropRowBytes, {int(c->width * 4 * sizeof(float))});
    p.set(kOfxImagePropField, {std::string(kOfxImageFieldNone)});
    p.set(kOfxImagePropUniqueIdentifier, {c->name + "@" + std::to_string(ti)});
    g_images[&p] = im;
    *out = p.h();
    return kOfxStatOK;
}
OfxStatus clipReleaseImage(OfxPropertySetHandle h) {
    auto it = g_images.find(P(h));
    if (it == g_images.end()) return kOfxStatErrBadHandle;
    Image *im = it->second;
    g_images.erase(it);
    if (current() && current()->on_release) current()->on_release(*im);
    delete im;
    return kOfxStatOK;
}
OfxStatus clipGetRegionOfDefinition(OfxImageClipHandle ch, OfxTime, OfxRectD *r) {
    *r = {0, 0, double(C(ch)->width), double(C(ch)->height)};
    return kOfxStatOK;
}
int effectAbort(OfxImageEffectHandle) {
    if (!current()) return 0;
    ++current()->abort_calls;
    return current()->on_abort ? current()->on_abort() : 0;
}
OfxStatus imageMemoryAlloc(OfxImageEffectHandle, size_t n, OfxImageMemoryHandle *h) {
    *h = reinterpret_cast<OfxImageMemoryHandle>(std::malloc(n ? n : 1));
    return *h ? kOfxStatOK : kOfxStatErrMemory;
}
OfxStatus imageMemoryFree(OfxImageMemoryHandle h) { std::free(h); return kOfxStatOK; }
OfxStatus imageMemoryLock(OfxImageMemoryHandle h, void **p) { *p = h; return kOfxStatOK; }
OfxStatus imageMemoryUnlock(OfxImageMemoryHandle) { return kOfxStatOK; }
OfxImageEffectSuiteV1 g_effect = {getPropertySet, getParamSet, clipDefine, clipGetHandle, clipGetPropertySet,
                                  clipGetImage, clipReleaseImage, clipGetRegionOfDefinition, effectAbort,
                                  imageMemoryAlloc, imageMemoryFree, imageMemoryLock, imageMemoryUnlock};

// ---- memory / thread / message -----------------------------------------
OfxStatus memoryAlloc(void *, size_t n, void **p) { *p = std::malloc(n ? n : 1); return *p ? kOfxStatOK : kOfxStatErrMemory; }
OfxStatus memoryFree(void *p) { std::free(p); return kOfxStatOK; }
OfxMemorySuiteV1 g_memory = {memoryAlloc, memoryFree};

OfxStatus multiThread(OfxThreadFunctionV1 fn, unsigned int n, void *arg) {
    if (n == 0) n = 1;
    for (unsigned int i = 0; i < n; ++i) fn(i, n, arg);
    return kOfxStatOK;
}
OfxStatus multiThreadNumCPUs(unsigned int *n) { *n = 1; return kOfxStatOK; }
OfxStatus multiThreadIndex(unsigned int *i) { *i = 0; return kOfxStatOK; }
int multiThreadIsSpawnedThread() { return 0; }
OfxStatus mutexCreate(OfxMutexHandle *m, int) { *m = reinterpret_cast<OfxMutexHandle>(new std::recursive_mutex); return kOfxStatOK; }
OfxStatus mutexDestroy(const OfxMutexHandle m) { delete reinterpret_cast<std::recursive_mutex *>(m); return kOfxStatOK; }
OfxStatus mutexLock(const OfxMutexHandle m) { reinterpret_cast<std::recursive_mutex *>(m)->lock(); return kOfxStatOK; }
OfxStatus mutexUnLock(const OfxMutexHandle m) { reinterpret_cast<std::recursive_mutex *>(m)->unlock(); return kOfxStatOK; }
OfxStatus mutexTryLock(const OfxMutexHandle m) {
    return reinterpret_cast<std::recursive_mutex *>(m)->try_lock() ? kOfxStatOK : kOfxStatFailed;
}
OfxMultiThreadSuiteV1 g_thread = {multiThread, multiThreadNumCPUs, multiThreadIndex, multiThreadIsSpawnedThread,
                                  mutexCreate, mutexDestroy, mutexLock, mutexUnLock, mutexTryLock};

void record(const char *type, const char *fmt, va_list ap) {
    char buf[2048];
    std::vsnprintf(buf, sizeof buf, fmt ? fmt : "", ap);
    if (current()) current()->log.push_back(std::string(type ? type : "?") + ": " + buf);
}
OfxStatus message(void *, const char *type, const char *, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    record(type, fmt, ap);
    va_end(ap);
    return kOfxStatOK;
}
OfxStatus setPersistentMessage(void *, const char *type, const char *, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    record(type, fmt, ap);
    va_end(ap);
    return kOfxStatOK;
}
OfxStatus clearPersistentMessage(void *) { return kOfxStatOK; }
OfxMessageSuiteV1 g_msg1 = {message};
OfxMessageSuiteV2 g_msg2 = {message, setPersistentMessage, clearPersistentMessage};

const void *fetchSuite(OfxPropertySetHandle, const char *name, int version) {
    std::string n = name;
    if (n == kOfxPropertySuite && version == 1) return &g_prop;
    if (n == kOfxParameterSuite && version == 1) return &g_param;
    if (n == kOfxImageEffectSuite && version == 1) return &g_effect;
    if (n == kOfxMemorySuite && version == 1) return &g_memory;
    if (n == kOfxMultiThreadSuite && version == 1) return &g_thread;
    if (n == kOfxMessageSuite && version == 1) return &g_msg1;
    if (n == kOfxMessageSuite && version == 2) return &g_msg2;
    return nullptr;
}
}  // namespace

// ---- PropSet helpers -----------------------------------------------------
int PropSet::dim(const std::string &k) const {
    auto it = props.find(k);
    return it == props.end() ? 0 : int(it->second.size());
}
int PropSet::i(const std::string &k, int idx) const {
    auto it = props.find(k);
    if (it == props.end() || idx >= int(it->second.size())) return 0;
    const Value &v = it->second[size_t(idx)];
    if (auto p = std::get_if<int>(&v)) return *p;
    if (auto p = std::get_if<double>(&v)) return int(*p);
    return 0;
}
double PropSet::d(const std::string &k, int idx) const {
    auto it = props.find(k);
    if (it == props.end() || idx >= int(it->second.size())) return 0;
    const Value &v = it->second[size_t(idx)];
    if (auto p = std::get_if<double>(&v)) return *p;
    if (auto p = std::get_if<int>(&v)) return *p;
    return 0;
}
std::string PropSet::s(const std::string &k, int idx) const {
    auto it = props.find(k);
    if (it == props.end() || idx >= int(it->second.size())) return {};
    auto p = std::get_if<std::string>(&it->second[size_t(idx)]);
    return p ? *p : std::string();
}
void *PropSet::p(const std::string &k, int idx) const {
    auto it = props.find(k);
    if (it == props.end() || idx >= int(it->second.size())) return nullptr;
    auto p = std::get_if<void *>(&it->second[size_t(idx)]);
    return p ? *p : nullptr;
}
Param *ParamSet::find(const std::string &name) const {
    for (auto &p : params)
        if (p->name == name) return p.get();
    return nullptr;
}

// ---- Host ------------------------------------------------------------------
Host *current() { return g_host; }
const std::vector<std::string> &missing_props() { return g_missing; }

Host::Host() {
    g_host = this;
    PropSet &h = host_props;
    h.set(kOfxPropName, {std::string("org.ntscrt.mockhost")});
    h.set(kOfxPropLabel, {std::string("NTSCRT mock host")});
    h.set(kOfxPropAPIVersion, {1, 5});
    h.set(kOfxPropVersion, {1, 0, 0});
    h.set(kOfxPropVersionLabel, {std::string("1.0")});
    h.set(kOfxImageEffectHostPropIsBackground, {0});
    h.set(kOfxImageEffectPropSupportsOverlays, {0});
    h.set(kOfxImageEffectPropSupportsMultiResolution, {0});
    h.set(kOfxImageEffectPropSupportsTiles, {0});
    h.set(kOfxImageEffectPropTemporalClipAccess, {1});
    h.set(kOfxImageEffectPropSupportedComponents, {std::string(kOfxImageComponentRGBA)});
    h.set(kOfxImageEffectPropSupportedContexts,
          {std::string(kOfxImageEffectContextFilter), std::string(kOfxImageEffectContextGeneral)});
    h.set(kOfxImageEffectPropSupportedPixelDepths, {std::string(kOfxBitDepthFloat)});
    h.set(kOfxImageEffectPropSupportsMultipleClipDepths, {0});
    h.set(kOfxImageEffectPropSupportsMultipleClipPARs, {0});
    h.set(kOfxImageEffectPropSetableFrameRate, {0});
    h.set(kOfxImageEffectPropSetableFielding, {0});
    h.set(kOfxImageEffectInstancePropSequentialRender, {0});
    h.set(kOfxParamHostPropSupportsStringAnimation, {0});
    h.set(kOfxParamHostPropSupportsCustomInteract, {0});
    h.set(kOfxParamHostPropSupportsChoiceAnimation, {0});
    h.set(kOfxParamHostPropSupportsBooleanAnimation, {0});
    h.set(kOfxParamHostPropSupportsCustomAnimation, {0});
    h.set(kOfxParamHostPropMaxParameters, {-1});
    h.set(kOfxParamHostPropMaxPages, {0});
    h.set(kOfxParamHostPropPageRowColumnCount, {0, 0});
    host.host = h.h();
    host.fetchSuite = fetchSuite;
}

Host::~Host() {
#ifdef _WIN32
    if (lib_) FreeLibrary(static_cast<HMODULE>(lib_));
#else
    if (lib_) dlclose(lib_);
#endif
    if (g_host == this) g_host = nullptr;
}

bool Host::load(const std::string &path, std::string &err) {
#ifdef _WIN32
    HMODULE m = LoadLibraryA(path.c_str());
    if (!m) { err = "LoadLibrary failed: " + std::to_string(GetLastError()); return false; }
    lib_ = m;
    get_num_ = reinterpret_cast<int (*)()>(GetProcAddress(m, "OfxGetNumberOfPlugins"));
    get_plugin_ = reinterpret_cast<OfxPlugin *(*)(int)>(GetProcAddress(m, "OfxGetPlugin"));
#else
    lib_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib_) { err = dlerror(); return false; }
    get_num_ = reinterpret_cast<int (*)()>(dlsym(lib_, "OfxGetNumberOfPlugins"));
    get_plugin_ = reinterpret_cast<OfxPlugin *(*)(int)>(dlsym(lib_, "OfxGetPlugin"));
#endif
    if (!get_num_ || !get_plugin_) { err = "OfxGetNumberOfPlugins/OfxGetPlugin not exported"; return false; }
    return true;
}

int Host::num_plugins() const { return get_num_ ? get_num_() : 0; }
OfxPlugin *Host::plugin(int i) const { return get_plugin_ ? get_plugin_(i) : nullptr; }

OfxStatus Host::action(OfxPlugin *p, const char *act, Effect *e, PropSet *in, PropSet *out) {
    g_missing.clear();
    OfxStatus st = p->mainEntry(act, e ? static_cast<const void *>(e->h()) : nullptr, in ? in->h() : nullptr,
                                out ? out->h() : nullptr);
    if (st != kOfxStatOK && st != kOfxStatReplyDefault) {
        std::fprintf(stderr, "[mock host] %s -> status %d; props fetched but unset:", act, st);
        for (auto &m : g_missing) std::fprintf(stderr, " %s", m.c_str());
        std::fputc('\n', stderr);
    }
    return st;
}

void Host::set_param_from_default(Param &p) {
    if (string_typed(p.type)) { p.str = p.props.s(kOfxParamPropDefault); return; }
    int n = n_components(p.type);
    p.v.assign(size_t(n), 0.0);
    for (int i = 0; i < n && i < p.props.dim(kOfxParamPropDefault); ++i) p.v[size_t(i)] = p.props.d(kOfxParamPropDefault, i);
}

std::unique_ptr<Effect> Host::make_instance(const Effect &desc, const char *context) {
    auto e = std::make_unique<Effect>();
    e->props.props = desc.props.props;
    e->props.set(kOfxPropType, {std::string(kOfxTypeImageEffectInstance)});
    e->props.set(kOfxImageEffectPropContext, {std::string(context)});
    e->props.set(kOfxPropInstanceData, {static_cast<void *>(nullptr)});
    e->props.set(kOfxImageEffectPropProjectSize, {1920.0, 1080.0});
    e->props.set(kOfxImageEffectPropProjectOffset, {0.0, 0.0});
    e->props.set(kOfxImageEffectPropProjectExtent, {1920.0, 1080.0});
    e->props.set(kOfxImageEffectPropProjectPixelAspectRatio, {1.0});
    e->props.set(kOfxImageEffectInstancePropEffectDuration, {100.0});
    e->props.set(kOfxImageEffectInstancePropSequentialRender, {0});
    e->props.set(kOfxImageEffectPropFrameRate, {25.0});
    e->props.set(kOfxPropIsInteractive, {0});
    for (const auto &dp : desc.params.params) {
        auto p = std::make_unique<Param>();
        p->type = dp->type;
        p->name = dp->name;
        p->props.props = dp->props.props;
        set_param_from_default(*p);
        e->params.params.push_back(std::move(p));
    }
    for (const auto &[name, dc] : desc.clips) {
        auto c = std::make_unique<Clip>();
        c->name = name;
        c->props.props = dc->props.props;
        PropSet &p = c->props;
        p.set(kOfxImageClipPropConnected, {1});
        p.set(kOfxImageEffectPropPixelDepth, {std::string(kOfxBitDepthFloat)});
        p.set(kOfxImageEffectPropComponents, {std::string(kOfxImageComponentRGBA)});
        p.set(kOfxImageClipPropUnmappedPixelDepth, {std::string(kOfxBitDepthFloat)});
        p.set(kOfxImageClipPropUnmappedComponents, {std::string(kOfxImageComponentRGBA)});
        p.set(kOfxImageEffectPropPreMultiplication, {std::string(kOfxImageUnPreMultiplied)});
        p.set(kOfxImagePropPixelAspectRatio, {1.0});
        p.set(kOfxImageEffectPropFrameRate, {25.0});
        p.set(kOfxImageEffectPropFrameRange, {10.0, 60.0});
        p.set(kOfxImageEffectPropUnmappedFrameRate, {25.0});
        p.set(kOfxImageEffectPropUnmappedFrameRange, {10.0, 60.0});
        p.set(kOfxImageClipPropFieldOrder, {std::string(kOfxImageFieldNone)});
        p.set(kOfxImageClipPropContinuousSamples, {0});
        e->clips[name] = std::move(c);
    }
    return e;
}
}  // namespace mock
