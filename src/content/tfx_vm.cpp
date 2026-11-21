#include "nim/content/tfx_vm.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
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
    case 0x2A: return {"saturate", 1};   case 0x2D: return {"length", 1};
    case 0x2E: return {"triangle", 1};
    case 0x2F: return {"jitter", 1};     case 0x30: return {"wander", 1};
    case 0x31: return {"rand", 1};       case 0x32: return {"rand_smooth", 1};
    case 0x35: return {"transform_vec4", 1};
    case 0x42: return {"push_const", 2}; case 0x43: return {"lerp_const", 2};
    case 0x44: return {"lerp_const_sat", 2}; case 0x45: return {"spline4_const", 2};
    case 0x46: return {"spline8_const", 2}; case 0x47: return {"spline8_chain_const", 2};
    case 0x48: return {"gradient4_const", 2}; case 0x49: return {"gradient8_const", 2};
    case 0x4A: return {"push_extern_float", 3}; case 0x4B: return {"push_extern_vec4", 3};
    case 0x4C: return {"push_extern_mat4", 3}; case 0x4D: return {"push_extern_texview", 3};
    case 0x4E: return {"push_extern_u32", 3}; case 0x4F: return {"push_extern_uav", 3};
    case 0x50: return {"unk50", 1};      case 0x51: return {"push_from_output", 2};
    case 0x53: return {"pop_output", 2}; case 0x54: return {"pop_output_unk", 2};
    case 0x55: return {"pop_output_mat4", 2};
    case 0x57: return {"push_temp", 2};  case 0x58: return {"pop_temp", 2};
    case 0x59: return {"pop_texview", 2}; case 0x5B: return {"pop_srv_view", 2}; case 0x5A: return {"unk5a", 2};
    case 0x5D: return {"pop_sampler", 2}; case 0x5E: return {"pop_uav", 2};
    case 0x5F: return {"unk5f", 2};
    case 0x61: return {"push_sampler", 2}; case 0x62: return {"push_object_channel", 5};
    case 0x63: return {"push_global_channel", 2}; case 0x64: return {"push_global_channel_", 2};
    case 0x65: return {"unk65", 1};      case 0x66: return {"push_tex_dimensions", 2};
    case 0x67: return {"push_tex_tiling", 3}; case 0x68: return {"push_tex_layer_count", 3};
    case 0x69: return {"push_extern_u32_", 3};
    case 0x6A: case 0x6B: case 0x6C: case 0x6D: return {"unk6x", 1};
    case 0x10: case 0x11: case 0x12: case 0x17: case 0x1E: case 0x2B: case 0x2C:
    case 0x33: case 0x34: return {"unk", 1};
    case 0x3B: return {"cmp_lt", 1};     case 0x3C: return {"cmp_le", 1};
    case 0x3D: return {"cmp_gt", 1};     case 0x3E: return {"cmp_ge", 1};
    case 0x3F: return {"cmp_eq", 1};     case 0x40: return {"cmp_ne", 1};
    case 0x41: return {"select", 1};
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


float hash_rand(float v0) {  // engine's 1/(prime/1e6) dot + Blum-Blum-Shub style scramble
    float d = v0 * (1.0f / 1.043501f) + v0 * (1.0f / 0.794471f) + v0 * (1.0f / 0.113777f) + v0 * (1.0f / 0.015101f);
    d -= std::floor(d);
    d = d * d * 251.0f;
    return d - std::floor(d);
}
float pseudo_sin(float a) {
    float w = a - std::round(a);
    return w * (-16.0f * std::fabs(w) + 8.0f);
}
// Piecewise cubic over 4 segments; lane i is active when thresholds[i] <= t < thresholds[i+1].
float4 spline4(float4 t, const float4* c) {
    bool m[4];
    for (int i = 0; i < 4; ++i) m[i] = c[4][i] <= t[i];
    uint32_t acc = 0;
    for (int i = 0; i < 4; ++i) {
        bool sel = m[i] != (i < 3 ? m[i + 1] : false);
        float v = (t[i] * c[0][i] + c[1][i]) * (t[i] * t[i]) + (c[2][i] * t[i] + c[3][i]);
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        if (sel) acc ^= bits;
    }
    float r;
    std::memcpy(&r, &acc, 4);
    return {r, r, r, r};
}
// 8-segment spline: two banks (c, d) of 4 cubic segments with their thresholds.
float spline8_eval(float x, const float4* k, bool* c_first, bool* d_first) {
    auto bank = [&](int base, int th, bool* first) {
        float4 mask;
        for (int i = 0; i < 4; ++i) mask[i] = x >= k[th][i] ? 1.0f : 0.0f;
        float4 ch = {std::fmod(mask[0] + mask[1], 2.0f), std::fmod(mask[1] + mask[2], 2.0f),
                     std::fmod(mask[2] + mask[3], 2.0f), mask[3]};
        float sum = 0;
        for (int i = 0; i < 4; ++i) {
            float hi = k[base][i] * x + k[base + 1][i], lo = k[base + 2][i] * x + k[base + 3][i];
            sum += (hi * x * x + lo) * ch[i];
        }
        *first = mask[0] > 0;
        return sum;
    };
    float c = bank(0, 8, c_first), d = bank(4, 9, d_first);
    return *d_first ? d : c;
}
float4 gradient4(float4 x, const float4* k) {  // base, r, g, b, a, thresholds
    const float4& th = k[5];
    float4 r = k[0];
    float4 pct;
    for (int i = 0; i < 4; ++i) {
        float next = i < 3 ? th[i + 1] : 1.0f;
        float off = x[i] - th[i], seg = next - th[i];
        pct[i] = std::clamp(seg != 0 ? off / seg : (off > 0 ? 1.0f : 0.0f), 0.0f, 1.0f);
    }
    for (int ch = 0; ch < 4; ++ch)
        for (int i = 0; i < 4; ++i) r[ch] += k[1 + ch][i] * pct[i];
    return r;
}
// Eight-stop gradient (goriath bytecode_op_unk3b_const): base k0, per-channel deltas k1..k4 for
// stops 0..3 and k5..k8 for stops 4..7, thresholds k9 (0..3) and k10 (4..7); the last segment ends
// at 1, and a segment narrower than 1e-4 is a step.
float4 gradient8(float4 x, const float4* k) {
    float t[9];
    for (int i = 0; i < 4; ++i) t[i] = k[9][i], t[4 + i] = k[10][i];
    t[8] = 1.0f;
    float w[8];
    for (int i = 0; i < 8; ++i) {
        const float off = x[i & 3] - t[i], seg = t[i + 1] - t[i];
        w[i] = std::clamp(std::fabs(seg) > 1e-4f ? off / seg : (off >= 0 ? 1.0f : 0.0f), 0.0f, 1.0f);
    }
    float4 r = k[0];
    for (int ch = 0; ch < 4; ++ch)
        for (int i = 0; i < 4; ++i) r[ch] += k[1 + ch][i] * w[i] + k[5 + ch][i] * w[4 + i];
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

TfxResult inspect_tfx_bindings(std::span<const uint8_t> bc) {
    TfxResult result;
    uint8_t id = 0, offset = 0, sampler = 0;
    for (size_t ip = 0; ip < bc.size();) {
        auto [name, size] = op_info(bc[ip]);
        if (!name || ip + size > bc.size()) throw std::runtime_error(std::format("TFX: invalid binding opcode {:02X} at 0x{:X}", bc[ip], ip));
        const auto op = bc[ip];
        if (op == 0x80) break;
        if (op == 0x4D || op == 0x4F) { id = bc[ip + 1]; offset = bc[ip + 2]; }
        if (op == 0x59 || op == 0x5B) result.extern_textures.push_back({uint32_t(bc[ip + 1] & 31), id, offset});
        if (op == 0x5E) result.extern_uavs.push_back({uint32_t(bc[ip + 1] & 31), id, offset});
        if (op == 0x61) sampler = bc[ip + 1];
        if (op == 0x5D) result.samplers.push_back({uint32_t(bc[ip + 1] & 31), sampler});
        result.live_inputs |= (op >= 0x4A && op <= 0x4F) || op == 0x62 || op == 0x63 || op == 0x64 || op == 0x69;
        ++result.instructions;
        ip += size;
    }
    return result;
}

std::vector<AtlasOperands> tfx_animated_atlas_operands(std::span<const uint8_t> bc) {
    std::vector<uint8_t> steps, scales, counts;
    for (size_t ip = 0; ip < bc.size();) {
        const auto [name, len] = op_info(bc[ip]);
        if (!name || ip + len > bc.size()) return {};
        if (bc[ip] == 0x80) break;
        if (bc[ip] == 0x68) {
            if (bc[ip + 2] == 0xBB) steps.push_back(bc[ip + 1]);
            else if (bc[ip + 2] == 0x11) scales.push_back(bc[ip + 1]);
            else return {};
        }
        if (bc[ip] == 0x69) {
            if (bc[ip + 2] != 0) return {};
            counts.push_back(bc[ip + 1]);
        }
        ip += len;
    }
    // PS 80A54A27 / 80A7C4E9 sample the same two atlases with several transforms.
    // cb0[39] and [74] use fixed offsets. The other nine transforms select a frame.
    if (scales == std::vector<uint8_t>{3, 4, 5, 8, 11, 14, 17, 20, 23, 26, 29} &&
        steps == std::vector<uint8_t>{6, 9, 12, 15, 18, 21, 24, 27, 30} &&
        counts == std::vector<uint8_t>{7, 10, 13, 16, 19, 22, 25, 28, 31}) {
        constexpr auto absent = AtlasOperands::kUnspecified;
        return {{3, absent, absent, 1}, {4, absent, absent, 2},
                {5, 6, 7, 1}, {8, 9, 10, 1}, {11, 12, 13, 1}, {14, 15, 16, 2}, {17, 18, 19, 2},
                {20, 21, 22, 2}, {23, 24, 25, 1}, {26, 27, 28, 2}, {29, 30, 31, 2}};
    }
    if (counts.size() != scales.size() || counts.size() != steps.size()) return {};
    if (counts.size() == 1) {
        // Sky: step 0, then a count/scale pair. Object state: consecutive scale/step/count.
        if ((steps[0] == 0 && counts[0] != 0 && scales[0] == counts[0] + 1) ||
            (steps[0] == scales[0] + 1 && counts[0] == steps[0] + 1))
            return {{scales[0], steps[0], counts[0]}};
    }
    // The two-layer object effect samples t2 and t3 with separate UVs and frame counts.
    // PS 80B7E4CF blends consecutive frames on t2 and uses a changing frame on t3.
    if (steps == std::vector<uint8_t>{0, 7} && counts == std::vector<uint8_t>{4, 8} &&
        scales == std::vector<uint8_t>{5, 6})
        return {{5, 0, 4, 2}, {6, 7, 8, 3}};
    // The three-layer material samples t0/t1/t2 in these operand groups (PS 80B088FF).
    if (steps == std::vector<uint8_t>{0, 1, 2} && counts == std::vector<uint8_t>{5, 6, 7} &&
        scales == std::vector<uint8_t>{8, 9, 10})
        return {{8, 0, 5, 0}, {9, 1, 6, 1}, {10, 2, 7, 2}};
    return {};
}

TfxResult run_tfx(std::span<const uint8_t> bc, std::span<const float4> constants, std::span<float4> out,
                  const TfxEnvironment& env, std::string* trace, bool record_bindings) {
    auto missing = [](const char* kind) -> float4 {
        throw std::runtime_error(std::format("TFX: missing {} input", kind));
    };
    TfxResult res;
    Vm vm(constants, out, env);
    uint8_t last_extern_id = 0, last_extern_offset = 0, last_sampler = 0;

    auto note = [&](const char* name) {
        throw std::runtime_error(std::format("TFX: unimplemented opcode {} (substitution is disabled)", name));
    };

    // The sequencer (global channel expressions) shares the opcodes up to 0x49 and has its own
    // two-byte 0x4A (push input n) and 0x4C (pop to output n).
    const bool sequencer = bool(env.sequencer_input);
    for (size_t ip = 0; ip < bc.size();) {
        const uint8_t op = bc[ip];
        auto [name, size] = op_info(op);
        if (sequencer && op > 0x49 && op < 0x80) {
            name = op == 0x4A ? "push_input" : op == 0x4C ? "pop_output" : nullptr;
            size = 2;
        }
        if (!name) throw std::runtime_error(std::format("TFX: unknown opcode {:02X} at {:04X}", op, ip));
        if (ip + size > bc.size()) throw std::runtime_error("TFX: truncated instruction");
        const uint8_t a1 = size > 1 ? bc[ip + 1] : 0, a2 = size > 2 ? bc[ip + 2] : 0;
        res.instructions++;
        if ((op >= 0x4A && op <= 0x4F) || op == 0x62 || op == 0x69) res.live_inputs = true;
        if (op == 0x80) break;

        if (sequencer && op == 0x4A) {
            vm.push(env.sequencer_input(a1));
            ip += size;
            continue;
        }
        if (sequencer && op == 0x4C) {
            vm.output(a1) = vm.pop();
            ip += size;
            continue;
        }
        switch (op) {
        case 0x49: {
            float4 k[11];
            for (int i = 0; i < 11; ++i) k[i] = vm.konst(a1 + size_t(i));
            vm.top() = gradient8(vm.top(), k);
            break;
        }
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
        case 0x2D: {
            // Material visibility programs pack (eye - object, 0), then apply
            // linear near and far ramps to this scalar distance.
            const auto x = vm.top();
            vm.top() = splat(std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2] + x[3] * x[3]));
            break;
        }
        case 0x2E: vm.top() = map(vm.top(), [](float x) { return std::fabs(x - std::round(x)) * 2.0f; }); break;
        case 0x35: {
            float4 v = vm.pop(), w = vm.pop(), z = vm.pop(), y = vm.pop(), x = vm.top();
            float4 r;
            for (int i = 0; i < 4; ++i) r[i] = x[i] * v[0] + y[i] * v[1] + z[i] * v[2] + w[i] * v[3];
            vm.top() = r;
            break;
        }
        case 0x42: vm.push(vm.konst(a1)); break;
        case 0x2F: {  // jitter
            float x = vm.top()[0];
            const float m[4] = {4.67f, 2.99f, 1.08f, 1.35f}, o[4] = {0.52f, 0.37f, 0.16f, 0.79f};
            float v = 0.5f;
            for (int i = 0; i < 4; ++i) {
                float a = x * m[i] + o[i];
                a -= std::round(a);
                v += (a * 0.25f) * (std::fabs(a) * -16.0f + 8.0f);
            }
            vm.top() = splat((3.0f - 2.0f * v) * v * v);
            break;
        }
        case 0x30: {  // wander
            float x = vm.top()[0];
            const float m0[4] = {4.08f, 1.02f, 3.0f / 5.37f, 3.0f / 9.67f}, o0[4] = {0.92f, 0.33f, 0.26f, 0.54f};
            const float m1[4] = {1.83f, 3.09f, 0.39f, 0.87f}, o1[4] = {0.12f, 0.37f, 0.16f, 0.79f};
            const float w1[4] = {0.02f, 0.02f, 0.28f, 0.28f};
            float v = 0.5f;
            for (int i = 0; i < 4; ++i) v += pseudo_sin(x * m0[i] + o0[i]) * pseudo_sin(x * m1[i] + o1[i]) * w1[i];
            vm.top() = splat(v);
            break;
        }
        case 0x31: vm.top() = splat(hash_rand(std::floor(vm.top()[0]))); break;
        case 0x32: {  // rand_smooth
            float v = vm.top()[0], v0 = std::round(v), f = v - v0;
            float a = hash_rand(v0), b = hash_rand(v0 + 1.0f), sf = (3.0f - 2.0f * f) * f * f;
            vm.top() = splat(a + (b - a) * sf);
            break;
        }
        case 0x45: {
            float4 k[5];
            for (int i = 0; i < 5; ++i) k[i] = vm.konst(a1 + size_t(i));
            vm.top() = spline4(vm.top(), k);
            break;
        }
        case 0x46: case 0x47: {
            float4 k[10];
            for (int i = 0; i < 10; ++i) k[i] = vm.konst(a1 + size_t(i));
            float4 x = vm.pop();
            float rec = 0;
            if (op == 0x47) rec = vm.top()[0];
            else vm.push(x);
            bool c_first = false, d_first = false;
            float r = spline8_eval(x[0], k, &c_first, &d_first);
            if (op == 0x47 && !d_first && !c_first) r = rec;
            vm.top() = splat(r);
            break;
        }
        case 0x48: {
            float4 k[6];
            for (int i = 0; i < 6; ++i) k[i] = vm.konst(a1 + size_t(i));
            vm.top() = gradient4(vm.top(), k);
            break;
        }
        case 0x43: case 0x44: {
            float4 a = vm.konst(a1), b = vm.konst(a1 + 1u), t = vm.top();
            for (int i = 0; i < 4; ++i) {
                t[i] = a[i] + t[i] * (b[i] - a[i]);
                if (op == 0x44) t[i] = std::min(std::max(t[i], 0.0f), 1.0f);
            }
            vm.top() = t;
            break;
        }
        case 0x4A: vm.push(splat(env.extern_float ? env.extern_float(a1, a2) : missing("extern scalar")[0])); break;
        case 0x4B: vm.push(env.extern_vec4 ? env.extern_vec4(a1, a2) : missing("extern vector")); break;
        case 0x4C:
            for (uint8_t r = 0; r < 4; ++r)
                vm.push(env.extern_vec4 ? env.extern_vec4(a1, uint8_t(a2 + r)) : missing("extern matrix"));
            break;
        case 0x4D: case 0x4F:  // texture / UAV handles: keep the stack shape, remember the source
            last_extern_id = a1;
            last_extern_offset = a2;
            vm.push(float4{});
            break;
        case 0x69:
            if (env.atlas_frame_count) {
                vm.push(splat(env.atlas_frame_count(a1, a2)));
                break;
            }
            [[fallthrough]];
        case 0x4E:  // raw u32 bits, splatted (reading the bytes as a float keeps them)
            vm.push(splat(env.extern_float ? env.extern_float(a1, a2) : missing("extern scalar")[0]));
            break;
        case 0x51: vm.push(vm.output(a1)); break;
        case 0x53: vm.output(a1) = vm.pop(); break;
        case 0x54: note(name); break;
        case 0x55: {
            float4 w = vm.pop(), z = vm.pop(), y = vm.pop(), x = vm.pop();
            vm.output(a1) = x; vm.output(a1 + 1u) = y; vm.output(a1 + 2u) = z; vm.output(a1 + 3u) = w;
            break;
        }
        case 0x57: if (a1 >= 16) throw std::runtime_error("TFX temp slot"); vm.push(vm.temp[a1]); break;
        case 0x58: if (a1 >= 16) throw std::runtime_error("TFX temp slot"); vm.temp[a1] = vm.pop(); break;
        case 0x59: case 0x5B:
            if (record_bindings) res.extern_textures.push_back({uint32_t(a1 & 0x1F), last_extern_id, last_extern_offset});
            vm.pop();
            break;
        case 0x5D:
            if (record_bindings) res.samplers.push_back({uint32_t(a1 & 0x1F), last_sampler});
            vm.pop();
            break;
        case 0x5E:
            if (record_bindings) res.extern_uavs.push_back({uint32_t(a1 & 31), last_extern_id, last_extern_offset});
            vm.pop();
            break;                     // UAV bind
        case 0x61: last_sampler = a1; vm.push(float4{}); break;  // sampler handle
        case 0x62: {
            uint32_t h = uint32_t(bc[ip + 1]) << 24 | uint32_t(bc[ip + 2]) << 16 | uint32_t(bc[ip + 3]) << 8 | bc[ip + 4];
            vm.push(env.object_channel ? env.object_channel(h) : missing("object channel"));
            break;
        }
        case 0x63: vm.push(env.global_channel ? env.global_channel(a1) : missing("global channel")); break;
        case 0x64: vm.push(env.world_channel ? env.world_channel(a1) : missing("world channel")); break;
        case 0x67: {
            // Texture streaming / tiling parameters of texture a1; the shaders seen use .w as the
            // minimum resident mip (an LOD clamp). Supplied by the environment, never invented here.
            if (!env.texture_tiling) missing("texture tiling");
            const float4 v = env.texture_tiling(a1);
            vm.push({v[(a2 >> 6) & 3], v[(a2 >> 4) & 3], v[(a2 >> 2) & 3], v[a2 & 3]});
            break;
        }
        case 0x68: {
            // Layer count of texture a1 (tiled / array textures), swizzled like push_tex_tiling.
            if (!env.texture_layer_count) missing("texture layer count");
            const float4 v = env.texture_layer_count(a1);
            vm.push({v[(a2 >> 6) & 3], v[(a2 >> 4) & 3], v[(a2 >> 2) & 3], v[a2 & 3]});
            break;
        }
        case 0x66:
            throw std::runtime_error(std::format("TFX: unimplemented opcode {} {:02X} {:02X} at {:04X}", name, a1, a2, ip));
        // Comparisons (1 / 0 per component) and the select after them: mida's opcode list names
        // 0x3B-0x41 CompareLess .. CompareNotEqual, CompareNotZeroTernary without values. Order
        // and operands read from the particle update's frame-time guard
        // `dt, 0.001, cmp_gt, 1/60, dt, select` = dt > 0.001 ? dt : 1/60.
        case 0x3B: vm.binary([](float4 a, float4 b) { return zip(a, b, [](float x, float y) { return x < y ? 1.0f : 0.0f; }); }); break;
        case 0x3C: vm.binary([](float4 a, float4 b) { return zip(a, b, [](float x, float y) { return x <= y ? 1.0f : 0.0f; }); }); break;
        case 0x3D: vm.binary([](float4 a, float4 b) { return zip(a, b, [](float x, float y) { return x > y ? 1.0f : 0.0f; }); }); break;
        case 0x3E: vm.binary([](float4 a, float4 b) { return zip(a, b, [](float x, float y) { return x >= y ? 1.0f : 0.0f; }); }); break;
        case 0x3F: vm.binary([](float4 a, float4 b) { return zip(a, b, [](float x, float y) { return x == y ? 1.0f : 0.0f; }); }); break;
        case 0x40: vm.binary([](float4 a, float4 b) { return zip(a, b, [](float x, float y) { return x != y ? 1.0f : 0.0f; }); }); break;
        case 0x41: {
            const float4 b = vm.pop(), a = vm.pop();
            const float4 c = vm.top();
            for (int i = 0; i < 4; ++i) vm.top()[i] = c[i] != 0 ? b[i] : a[i];
            break;
        }
        default:
            // Never continue with an invented result for an unknown instruction.
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

TfxResult evaluate_tfx(std::span<const uint8_t> bytecode, std::span<const float4> constants,
                       std::span<const float4> initial, std::vector<float4>& out, const TfxEnvironment& env) {
    out.assign(initial.begin(), initial.end());
    out.resize(std::max<size_t>(out.size(), 128));
    TfxResult result = run_tfx(bytecode, constants, out, env);
    return result;
}

} // namespace nim
