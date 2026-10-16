#include "nim/content/tfx_vm.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>

namespace nim {
namespace {

// Opcode numbering follows the Tiger TFX VM (same as Destiny 2; cross-checked against Deimos).
struct OpInfo { const char* name; uint8_t size; };
OpInfo op_info(uint8_t op) {
    switch (op) {
    case 0x01: return {"add", 1};        case 0x02: return {"sub", 1};
    case 0x03: return {"mul", 1};        case 0x04: return {"div", 1};
    case 0x05: return {"mul_", 1};       case 0x06: return {"add_", 1};
    case 0x07: return {"is_zero", 1};    case 0x08: return {"min", 1};
    case 0x09: return {"max", 1};        case 0x0A: return {"less_than", 1};
    case 0x0B: return {"dot", 1};        case 0x0C: return {"merge_1_3", 1};
    case 0x0D: return {"merge_2_2", 1};  case 0x0E: return {"merge_3_1", 1};
    case 0x0F: return {"cubic", 1};      case 0x13: return {"lerp", 1};
    case 0x14: return {"lerp_sat", 1};   case 0x15: return {"mul_add", 1};
    case 0x16: return {"clamp", 1};      case 0x18: return {"abs", 1};
    case 0x19: return {"sign", 1};       case 0x1A: return {"floor", 1};
    case 0x1B: return {"ceil", 1};       case 0x1C: return {"round", 1};
    case 0x1D: return {"frac", 1};       case 0x1F: return {"normalize3", 1};
    case 0x20: return {"negate", 1};     case 0x21: return {"vec_rot_sin", 1};
    case 0x22: return {"vec_rot_cos", 1}; case 0x23: return {"vec_rot_sincos", 1};
    case 0x28: return {"splat", 1};      case 0x29: return {"permute", 2};
    case 0x2A: return {"saturate", 1};   case 0x2E: return {"triangle", 1};
    case 0x2F: return {"jitter", 1};     case 0x30: return {"wander", 1};
    case 0x31: return {"rand", 1};       case 0x32: return {"rand_smooth", 1};
    case 0x35: return {"transform_vec4", 1};
    case 0x42: return {"push_const", 2}; case 0x43: return {"lerp_const", 2};
    case 0x44: return {"lerp_const_sat", 2}; case 0x45: return {"spline4_const", 2};
    case 0x46: return {"spline8_const", 2}; case 0x47: return {"spline8_chain_const", 2};
    case 0x48: return {"gradient4_const", 2}; case 0x49: return {"unk49", 2};
    case 0x4A: return {"push_extern_float", 3}; case 0x4B: return {"push_extern_vec4", 3};
    case 0x4C: return {"push_extern_mat4", 3}; case 0x4D: return {"push_extern_texview", 3};
    case 0x4E: return {"push_extern_u32", 3}; case 0x4F: return {"push_extern_uav", 3};
    case 0x50: return {"unk50", 1};      case 0x51: return {"push_from_output", 2};
    case 0x53: return {"pop_output", 2}; case 0x54: return {"pop_output_unk", 2};
    case 0x55: return {"pop_output_mat4", 2};
    case 0x57: return {"push_temp", 2};  case 0x58: return {"pop_temp", 2};
    case 0x59: return {"pop_texview", 2}; case 0x5A: return {"unk5a", 2};
    case 0x5D: return {"pop_sampler", 2}; case 0x5E: return {"pop_uav", 2};
    case 0x5F: return {"unk5f", 2};
    case 0x61: return {"push_sampler", 2}; case 0x62: return {"push_object_channel", 5};
    case 0x63: return {"push_global_channel", 2}; case 0x64: return {"push_global_channel_", 2};
    case 0x65: return {"unk65", 1};      case 0x66: return {"push_tex_dimensions", 2};
    case 0x67: return {"push_tex_tiling", 3}; case 0x68: return {"push_tex_layer_count", 3};
    case 0x69: return {"unk69", 3};
    case 0x6A: case 0x6B: case 0x6C: case 0x6D: return {"unk6x", 1};
    case 0x10: case 0x11: case 0x12: case 0x17: case 0x1E: case 0x2B: case 0x2C: case 0x2D:
    case 0x33: case 0x34: case 0x3B: case 0x3D: case 0x3F: case 0x40: case 0x41: return {"unk", 1};
    case 0x80: return {"return", 1};
    default: return {nullptr, 0};
    }
}

float4 map(float4 a, float (*f)(float)) { return {f(a[0]), f(a[1]), f(a[2]), f(a[3])}; }
template <class F> float4 zip(float4 a, float4 b, F f) {
    return {f(a[0], b[0]), f(a[1], b[1]), f(a[2], b[2]), f(a[3], b[3])};
}
float4 splat(float v) { return {v, v, v, v}; }
float4 sin_rot(float4 a) {  // engine's polynomial sin(2*pi*a) estimate
    float4 r;
    for (int i = 0; i < 4; ++i) {
        float w = a[i] - std::round(a[i]);
        float y = w * (-16.0f * std::fabs(w) + 8.0f);
        r[i] = y * (0.225f * std::fabs(y) + 0.775f);
    }
    return r;
}

class Vm {
public:
    Vm(std::span<const float4> k, std::span<float4> out, const TfxEnvironment& env)
        : k_(k), out_(out), env_(env) {}

    float4 stack[33]{};
    int sp = 0;  // stack[0] is a permanent zero slot, as in the engine
    float4 temp[16]{};

    float4& top() { return stack[sp]; }
    float4 at(int rel) {
        int i = sp + rel;
        if (i < 0 || i > 32) throw std::runtime_error("TFX stack index out of range");
        return stack[i];
    }
    void push(float4 v) {
        if (sp >= 32) throw std::runtime_error("TFX stack overflow");
        stack[++sp] = v;
    }
    float4 pop() {
        float4 v = stack[sp];
        if (sp > 0) --sp;
        return v;
    }
    void binary(auto f) { float4 b = pop(); float4 a = top(); top() = f(a, b); }
    float4 konst(size_t i) {
        if (i >= k_.size()) throw std::runtime_error(std::format("TFX constant {} out of range", i));
        return k_[i];
    }
    float4& output(size_t i) {
        if (i >= out_.size()) throw std::runtime_error(std::format("TFX output {} out of range", i));
        return out_[i];
    }

    std::span<const float4> k_;
    std::span<float4> out_;
    const TfxEnvironment& env_;
};

} // namespace

std::string disassemble_tfx(std::span<const uint8_t> bc) {
    std::string s;
    for (size_t ip = 0; ip < bc.size();) {
        auto [name, size] = op_info(bc[ip]);
        if (!name) { s += std::format("{:04X}: ?? {:02X}\n", ip, bc[ip]); break; }
        s += std::format("{:04X}: {}", ip, name);
        for (size_t a = 1; a < size && ip + a < bc.size(); ++a) s += std::format(" {:02X}", bc[ip + a]);
        s += '\n';
        ip += size;
    }
    return s;
}

TfxResult run_tfx(std::span<const uint8_t> bc, std::span<const float4> constants, std::span<float4> out,
                  const TfxEnvironment& env, std::string* trace) {
    TfxResult res;
    Vm vm(constants, out, env);
    std::vector<int> pending_textures;  // stack depth bookkeeping for texture views
    uint8_t last_extern_id = 0, last_extern_offset = 0;

    auto note = [&](const char* name) {
        if (std::find(res.unsupported.begin(), res.unsupported.end(), name) == res.unsupported.end())
            res.unsupported.push_back(name);
    };

    for (size_t ip = 0; ip < bc.size();) {
        const uint8_t op = bc[ip];
        auto [name, size] = op_info(op);
        if (!name) throw std::runtime_error(std::format("TFX: unknown opcode {:02X} at {:04X}", op, ip));
        if (ip + size > bc.size()) throw std::runtime_error("TFX: truncated instruction");
        const uint8_t a1 = size > 1 ? bc[ip + 1] : 0, a2 = size > 2 ? bc[ip + 2] : 0;
        res.instructions++;
        if (op == 0x80) break;

        switch (op) {
        case 0x01: case 0x06: vm.binary([](float4 a, float4 b) { return zip(a, b, [](float x, float y) { return x + y; }); }); break;
        case 0x02: vm.binary([](float4 a, float4 b) { return zip(a, b, [](float x, float y) { return x - y; }); }); break;
        case 0x03: case 0x05: vm.binary([](float4 a, float4 b) { return zip(a, b, [](float x, float y) { return x * y; }); }); break;
        case 0x04: vm.binary([](float4 a, float4 b) {
            return zip(a, b, [](float x, float y) {
                return std::fabs(y) > 1e-19f ? x / y : std::copysign(INFINITY, x);
            });
        }); break;
        case 0x07: vm.top() = map(vm.top(), [](float x) { return x == 0.0f ? 1.0f : 0.0f; }); break;
        case 0x08: vm.binary([](float4 a, float4 b) { return zip(a, b, [](float x, float y) { return std::min(x, y); }); }); break;
        case 0x09: vm.binary([](float4 a, float4 b) { return zip(a, b, [](float x, float y) { return std::max(x, y); }); }); break;
        case 0x0A: vm.binary([](float4 a, float4 b) { return zip(a, b, [](float x, float y) { return x < y ? 1.0f : 0.0f; }); }); break;
        case 0x0B: vm.binary([](float4 a, float4 b) { return splat(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]); }); break;
        case 0x0C: vm.binary([](float4 a, float4 b) { return float4{a[0], b[0], b[1], b[2]}; }); break;
        case 0x0D: vm.binary([](float4 a, float4 b) { return float4{a[0], a[1], b[0], b[1]}; }); break;
        case 0x0E: vm.binary([](float4 a, float4 b) { return float4{a[0], a[1], a[2], b[0]}; }); break;
        case 0x0F: vm.binary([](float4 c, float4 x) {
            float4 r;
            for (int i = 0; i < 4; ++i) r[i] = (c[0] * x[i] + c[1]) * x[i] * x[i] + (c[2] * x[i] + c[3]);
            return r;
        }); break;
        case 0x13: case 0x14: {
            float4 s = vm.pop(), y = vm.pop(), x = vm.top();
            for (int i = 0; i < 4; ++i) {
                x[i] = (y[i] - x[i]) * s[i] + x[i];
                if (op == 0x14) x[i] = std::max(x[i], 0.0f);
            }
            vm.top() = x;
            break;
        }
        case 0x15: {
            float4 c = vm.pop(), b = vm.pop(), a = vm.top();
            for (int i = 0; i < 4; ++i) a[i] = std::fma(a[i], b[i], c[i]);
            vm.top() = a;
            break;
        }
        case 0x16: {
            float4 lo = vm.pop(), hi = vm.pop(), v = vm.top();
            for (int i = 0; i < 4; ++i) v[i] = std::min(std::max(v[i], lo[i]), hi[i]);
            vm.top() = v;
            break;
        }
        case 0x18: vm.top() = map(vm.top(), [](float x) { return std::fabs(x); }); break;
        case 0x19: vm.top() = map(vm.top(), [](float x) { return x > 0 ? 1.0f : x < 0 ? -1.0f : 0.0f; }); break;
        case 0x1A: vm.top() = map(vm.top(), [](float x) { return std::floor(x); }); break;
        case 0x1B: vm.top() = map(vm.top(), [](float x) { return std::ceil(x); }); break;
        case 0x1C: vm.top() = map(vm.top(), [](float x) { return std::round(x); }); break;
        case 0x1D: vm.top() = map(vm.top(), [](float x) { return x - std::floor(x); }); break;
        case 0x1F: {
            float4 v = vm.top();
            float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            vm.top() = len > 0 ? float4{v[0] / len, v[1] / len, v[2] / len, v[3] / len} : float4{};
            break;
        }
        case 0x20: vm.top() = map(vm.top(), [](float x) { return -x; }); break;
        case 0x21: vm.top() = sin_rot(vm.top()); break;
        case 0x22: { float4 v = vm.top(); for (auto& x : v) x += 0.25f; vm.top() = sin_rot(v); break; }
        case 0x23: { float4 v = vm.top(); v[1] += 0.25f; v[3] += 0.25f; vm.top() = sin_rot(v); break; }
        case 0x28: vm.top() = splat(vm.top()[0]); break;
        case 0x29: {
            float4 v = vm.top();
            vm.top() = {v[(a1 >> 6) & 3], v[(a1 >> 4) & 3], v[(a1 >> 2) & 3], v[a1 & 3]};
            break;
        }
        case 0x2A: vm.top() = map(vm.top(), [](float x) { return std::min(std::max(x, 0.0f), 1.0f); }); break;
        case 0x2E: vm.top() = map(vm.top(), [](float x) { return std::fabs(x - std::round(x)) * 2.0f; }); break;
        case 0x35: {
            float4 v = vm.pop(), w = vm.pop(), z = vm.pop(), y = vm.pop(), x = vm.top();
            float4 r;
            for (int i = 0; i < 4; ++i) r[i] = x[i] * v[0] + y[i] * v[1] + z[i] * v[2] + w[i] * v[3];
            vm.top() = r;
            break;
        }
        case 0x42: vm.push(vm.konst(a1)); break;
        case 0x43: case 0x44: {
            float4 a = vm.konst(a1), b = vm.konst(a1 + 1u), t = vm.top();
            for (int i = 0; i < 4; ++i) {
                t[i] = a[i] + t[i] * (b[i] - a[i]);
                if (op == 0x44) t[i] = std::min(std::max(t[i], 0.0f), 1.0f);
            }
            vm.top() = t;
            break;
        }
        case 0x4A: vm.push(splat(env.extern_float ? env.extern_float(a1, a2) : 0.0f)); break;
        case 0x4B: vm.push(env.extern_vec4 ? env.extern_vec4(a1, a2) : float4{}); break;
        case 0x4C:
            for (uint8_t r = 0; r < 4; ++r)
                vm.push(env.extern_vec4 ? env.extern_vec4(a1, uint8_t(a2 + r)) : float4{});
            break;
        case 0x4D: case 0x4F:  // texture / UAV handles: keep the stack shape, remember the source
            last_extern_id = a1;
            last_extern_offset = a2;
            vm.push(float4{});
            break;
        case 0x4E: vm.push(float4{}); note(name); break;
        case 0x51: vm.push(vm.output(a1)); break;
        case 0x53: vm.output(a1) = vm.pop(); break;
        case 0x54: vm.pop(); note(name); break;
        case 0x55: {
            float4 w = vm.pop(), z = vm.pop(), y = vm.pop(), x = vm.pop();
            vm.output(a1) = x; vm.output(a1 + 1u) = y; vm.output(a1 + 2u) = z; vm.output(a1 + 3u) = w;
            break;
        }
        case 0x57: if (a1 >= 16) throw std::runtime_error("TFX temp slot"); vm.push(vm.temp[a1]); break;
        case 0x58: if (a1 >= 16) throw std::runtime_error("TFX temp slot"); vm.temp[a1] = vm.pop(); break;
        case 0x59:
            res.extern_textures.push_back({uint32_t(a1 & 0x1F), last_extern_id, last_extern_offset});
            vm.pop();
            break;
        case 0x5D: case 0x5E: vm.pop(); break;          // sampler / UAV binds
        case 0x61: vm.push(float4{}); break;            // sampler handle
        case 0x62: {
            uint32_t h = uint32_t(bc[ip + 1]) << 24 | uint32_t(bc[ip + 2]) << 16 | uint32_t(bc[ip + 3]) << 8 | bc[ip + 4];
            vm.push(env.object_channel ? env.object_channel(h) : splat(1.0f));
            break;
        }
        case 0x63: case 0x64: vm.push(env.global_channel ? env.global_channel(a1) : splat(1.0f)); break;
        case 0x66: case 0x67: case 0x68: {
            const float4 v = {0.25f, 0.25f, 0.25f, 0.0625f};
            vm.push({v[(a1 >> 6) & 3], v[(a1 >> 4) & 3], v[(a1 >> 2) & 3], v[a1 & 3]});
            note(name);
            break;
        }
        case 0x3D: { float4 v1 = vm.at(-1); vm.push(v1); vm.sp -= 2; note("unk3d"); break; }
        default:
            // Unimplemented ops leave the stack shape as-is; record them so callers can tell.
            note(name);
            break;
        }

        if (trace) {
            *trace += std::format("{:04X} {:<22} sp={:2}", ip, name, vm.sp);
            if (vm.sp > 0) {
                auto t = vm.top();
                *trace += std::format("  top=({:.4g}, {:.4g}, {:.4g}, {:.4g})", t[0], t[1], t[2], t[3]);
            }
            *trace += '\n';
        }
        ip += size;
    }
    return res;
}

} // namespace nim
