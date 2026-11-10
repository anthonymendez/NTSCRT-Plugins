// ntscrt_shaderc — build-host tool driven by cmake/Shaders.cmake.
//
//   ntscrt_shaderc <slangc> <in.slang> <stem> <out.inc> <tmp dir> [-I dir]...
//
// 1. slangc -> Metal Shading Language (all entry points in one library) and
//    reflection JSON (entry points, [numthreads]).
// 2. slangc -> HLSL per entry point (profile cs_5_0) and its reflection JSON,
//    which gives each global's D3D register.
// 3. Rewrites the [[buffer(n)]]/[[texture(n)]]/[[sampler(n)]] indices in every
//    MSL kernel signature from the D3D registers, so both backends bind the
//    same way (gpu.h convention):
//        b0            -> buffer(0)
//        t8..t15       -> buffer(8..15)      (StructuredBuffer)
//        u8..u15       -> buffer(16..23)     (RWStructuredBuffer)
//        t0..t7        -> texture(0..7)      (Texture2D)
//        u0..u7        -> texture(8..15)     (RWTexture2D / WTexture2D)
//        s0, s1        -> sampler(0), sampler(1)
//    Slang's own Metal layout packs t/u textures into one sequential range and
//    lets t8/u8 buffers collide, so it can't be used as-is.
//    An RWTexture2D the kernel never reads is declared access::write instead
//    of access::read_write (Metal only allows read_write on a few formats).
// 4. Writes a .inc that defines a gpu::ShaderFile (see shader_registry.h).
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace {

[[noreturn]] void die(const std::string &msg) {
    std::fprintf(stderr, "ntscrt_shaderc: error: %s\n", msg.c_str());
    std::exit(1);
}

std::string read_text(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) die("cannot read " + p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string quote(const std::string &s) { return "\"" + s + "\""; }

void run(const std::string &cmd) {
#ifdef _WIN32
    std::string c = "\"" + cmd + "\"";  // cmd.exe strips the outer pair
#else
    const std::string &c = cmd;
#endif
    int rc = std::system(c.c_str());
    if (rc != 0) die("command failed (" + std::to_string(rc) + "): " + cmd);
}

struct Reg {
    char kind;  // 'b','t','u','s'
    int index;
};

// name -> register, from an HLSL-target reflection JSON.
std::map<std::string, Reg> registers(const json &refl, std::map<std::string, uint32_t> *strides,
                                     uint32_t *cb_size) {
    std::map<std::string, Reg> out;
    for (const auto &p : refl.value("parameters", json::array())) {
        std::string name = p.value("name", "");
        json b = p.contains("binding") ? p["binding"] : json();
        if (b.is_null() && p.contains("bindings") && !p["bindings"].empty()) b = p["bindings"][0];
        if (b.is_null()) continue;
        std::string k = b.value("kind", "");
        int idx = b.value("index", 0);
        char c = k == "constantBuffer" ? 'b' : k == "shaderResource" ? 't' : k == "unorderedAccess" ? 'u'
               : k == "samplerState" ? 's' : 0;
        if (!c) continue;
        out[name] = {c, idx};
        const json &ty = p.value("type", json::object());
        if (strides && ty.value("baseShape", "") == "structuredBuffer") {
            uint32_t sz = 0, al = 1;
            for (const auto &s : ty["resultType"].value("sizes", json::array()))
                if (s.value("kind", "") == "uniform") {
                    sz = s.value("value", 0u);
                    al = std::max(1u, s.value("alignment", 1u));
                }
            sz = (sz + al - 1) / al * al;
            (*strides)[name] = sz;
        }
        if (cb_size && c == 'b' && idx == 0 && ty.value("kind", "") == "constantBuffer") {
            for (const auto &s : ty["elementType"].value("sizes", json::array()))
                if (s.value("kind", "") == "uniform") *cb_size = s.value("value", 0u);
        }
    }
    return out;
}

int metal_index(const std::string &what, const Reg &r, const std::string &name) {
    auto bad = [&]() -> int {
        die("'" + name + "' is " + std::string(1, r.kind) + std::to_string(r.index) + " but used as a Metal " + what +
            "; follow the gpu.h binding convention");
    };
    if (what == "buffer") {
        if (r.kind == 'b' && r.index < 8) return r.index;
        if (r.kind == 't' && r.index >= 8 && r.index < 16) return r.index;
        if (r.kind == 'u' && r.index >= 8 && r.index < 16) return r.index + 8;
        return bad();
    }
    if (what == "texture") {
        if (r.kind == 't' && r.index < 8) return r.index;
        if (r.kind == 'u' && r.index < 8) return r.index + 8;
        return bad();
    }
    if (r.kind == 's' && r.index < 16) return r.index;
    return bad();
}

std::string rewrite_msl(const std::string &msl, const std::map<std::string, Reg> &regs) {
    static const std::regex param(R"(([A-Za-z_][A-Za-z0-9_]*)_(\d+)(\s*)\[\[(buffer|texture|sampler)\((\d+)\)\]\])");
    std::string out;
    size_t pos = 0;
    for (;;) {
        size_t k = msl.find("[[kernel]]", pos);
        if (k == std::string::npos) break;
        size_t open = msl.find('(', k);
        if (open == std::string::npos) die("malformed MSL kernel signature");
        int depth = 0;
        size_t close = open;
        for (; close < msl.size(); ++close) {
            if (msl[close] == '(') ++depth;
            else if (msl[close] == ')' && --depth == 0) break;
        }
        out.append(msl, pos, open - pos);
        std::string sig = msl.substr(open, close - open);
        std::string res;
        auto begin = std::sregex_iterator(sig.begin(), sig.end(), param);
        size_t last = 0;
        for (auto it = begin; it != std::sregex_iterator(); ++it) {
            const auto &m = *it;
            std::string base = m[1].str(), what = m[4].str();
            auto r = regs.find(base);
            if (r == regs.end()) die("MSL parameter '" + base + "' has no D3D register in the HLSL reflection");
            res.append(sig, last, size_t(m.position(0)) - last);
            res += base + "_" + m[2].str() + m[3].str() + "[[" + what + "(" +
                   std::to_string(metal_index(what, r->second, base)) + ")]]";
            last = size_t(m.position(0) + m.length(0));
        }
        res.append(sig, last, std::string::npos);
        out += res;
        pos = close;
    }
    out.append(msl, pos, std::string::npos);

    // read_write -> write for textures that are never read.
    static const std::regex rw(R"(access::read_write>\s*([A-Za-z_][A-Za-z0-9_]*)_\d+)");
    std::set<std::string> rw_names;
    for (auto it = std::sregex_iterator(out.begin(), out.end(), rw); it != std::sregex_iterator(); ++it)
        rw_names.insert((*it)[1].str());
    for (const auto &n : rw_names) {
        std::regex reads("\\b" + n + "_\\d+\\)*\\.read\\(");
        if (std::regex_search(out, reads)) continue;
        std::regex decl("access::read_write>(\\s*)" + n + "_(\\d+)");
        out = std::regex_replace(out, decl, "access::write>$1" + n + "_$2");
    }
    return out;
}

void emit_bytes(std::ostream &o, const std::string &name, const std::string &text) {
    o << "static const unsigned char " << name << "[] = {";
    for (size_t i = 0; i < text.size(); ++i) {
        if (i % 24 == 0) o << "\n";
        o << unsigned(static_cast<unsigned char>(text[i])) << ",";
    }
    o << "0};\n";
}

std::string ident(const std::string &s) {
    std::string r;
    for (char c : s) r += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    return r;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 6) die("usage: ntscrt_shaderc <slangc> <in.slang> <stem> <out.inc> <tmpdir> [-I dir]...");
    std::string slangc = argv[1], in = argv[2], stem = argv[3], out_inc = argv[4], tmp = argv[5];
    std::string inc;
    for (int i = 6; i + 1 < argc; i += 2)
        if (std::string(argv[i]) == "-I") inc += " -I " + quote(argv[i + 1]);
    const std::string common = " -line-directive-mode none -warnings-disable 39029,39001" + inc;
    const std::string base = tmp + "/" + stem;

    run(quote(slangc) + " " + quote(in) + " -target metal -o " + quote(base + ".metal") + " -reflection-json " +
        quote(base + ".metal.json") + common);
    json mrefl = json::parse(read_text(base + ".metal.json"));
    std::string msl = read_text(base + ".metal");

    struct E {
        std::string name, hlsl;
        uint32_t threads[3] = {1, 1, 1};
        uint32_t strides[16] = {};
        uint32_t cb = 0;
    };
    std::vector<E> entries;
    std::map<std::string, Reg> all_regs;
    for (const auto &ep : mrefl.value("entryPoints", json::array())) {
        if (ep.value("stage", "") != "compute") continue;
        E e;
        e.name = ep.value("name", "");
        auto tg = ep.value("threadGroupSize", json::array());
        for (size_t i = 0; i < 3 && i < tg.size(); ++i) e.threads[i] = tg[i].get<uint32_t>();
        std::string hb = base + "." + e.name;
        run(quote(slangc) + " " + quote(in) + " -target hlsl -profile cs_5_0 -entry " + e.name + " -o " +
            quote(hb + ".hlsl") + " -reflection-json " + quote(hb + ".hlsl.json") + common);
        e.hlsl = read_text(hb + ".hlsl");
        std::map<std::string, uint32_t> strides;
        auto regs = registers(json::parse(read_text(hb + ".hlsl.json")), &strides, &e.cb);
        for (const auto &[n, r] : regs) {
            auto it = all_regs.find(n);
            if (it != all_regs.end() && (it->second.kind != r.kind || it->second.index != r.index))
                die("'" + n + "' has different registers in different entry points");
            all_regs[n] = r;
        }
        for (const auto &[n, s] : strides) {
            const Reg &r = regs[n];
            if (r.kind == 't' && r.index >= 8 && r.index < 16) e.strides[r.index - 8] = s;
            else if (r.kind == 'u' && r.index >= 8 && r.index < 16) e.strides[r.index] = s;
            else die("structured buffer '" + n + "' must use t8..t15 or u8..u15");
        }
        entries.push_back(std::move(e));
    }
    if (entries.empty()) die(in + ": no [shader(\"compute\")] entry points");
    msl = rewrite_msl(msl, all_regs);
    { std::ofstream dbg(base + ".final.metal", std::ios::binary); dbg << msl; }  // for inspection

    std::ostringstream o;
    const std::string id = ident(stem);
    o << "// Generated by ntscrt_shaderc from " << in << " -- do not edit.\n";
    o << "namespace ntscrt_shaders_" << id << " {\n";
    emit_bytes(o, "msl", msl);
    for (size_t i = 0; i < entries.size(); ++i) emit_bytes(o, "hlsl" + std::to_string(i), entries[i].hlsl);
    o << "static const ntscrt::gpu::ShaderEntry entries[] = {\n";
    for (size_t i = 0; i < entries.size(); ++i) {
        const E &e = entries[i];
        o << "  {\"" << e.name << "\", reinterpret_cast<const char *>(hlsl" << i << "), {" << e.threads[0] << ","
          << e.threads[1] << "," << e.threads[2] << "}, {";
        for (int s = 0; s < 16; ++s) o << e.strides[s] << (s < 15 ? "," : "");
        o << "}, " << e.cb << "},\n";
    }
    o << "};\n";
    o << "}  // namespace ntscrt_shaders_" << id << "\n";
    o << "static const ntscrt::gpu::ShaderFile ntscrt_shader_file_" << id << " = {\"" << stem
      << "\", reinterpret_cast<const char *>(ntscrt_shaders_" << id << "::msl), ntscrt_shaders_" << id
      << "::entries, " << entries.size() << "};\n";

    std::ofstream f(out_inc, std::ios::binary);
    if (!f) die("cannot write " + out_inc);
    f << o.str();
    return 0;
}
