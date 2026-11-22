// Blossom: pre-decoded PICA200 shader interpreter.
// Each program word is decoded once (on first use after an upload) into a compact form, and each
// operand descriptor is decoded when it is written, so the hot loop does no bit field extraction,
// no per-component swizzle decoding and no per-component destination dispatch. Arithmetic matches
// the original handlers in gpu.cpp exactly (same operation order, same 0 * inf = 0 rule); anything
// unusual (flow control, emit, invalid destinations, unknown opcodes) runs the original handler.
// BLOSSOM_SHADER=legacy selects the original interpreter for the whole program.
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

#include "common/profiler.hpp"
#include "core/arm11/gpu.hpp"

namespace
{

enum : uint8_t
{
    OP_LEGACY = 0,
    OP_NOP,
    OP_END,
    OP_ADD,
    OP_DP3,
    OP_DP4,
    OP_DPH,
    OP_MUL,
    OP_MAX,
    OP_MIN,
    OP_SGE,
    OP_SLT,
    OP_FLR,
    OP_EX2,
    OP_LG2,
    OP_RCP,
    OP_RSQ,
    OP_MOVA,
    OP_MOV,
    OP_CMP,
    OP_MAD,
};

//Four floats, with SSE where available. pmul is float24::operator* (0 * inf = 0) per lane;
//max/min are (a > b) ? a : b and (a < b) ? a : b, which is exactly what MAXPS/MINPS do.
#if defined(__SSE2__) || defined(_M_X64)
struct V4
{
    __m128 v;
    static V4 load(const float* p) { return {_mm_loadu_ps(p)}; }
    static V4 set(float a, float b, float c, float d) { return {_mm_setr_ps(a, b, c, d)}; }
    static V4 splat_bits(uint32_t b) { return {_mm_castsi128_ps(_mm_set1_epi32((int)b))}; }
    void store(float* p) const { _mm_storeu_ps(p, v); }
    V4 xor_bits(uint32_t b) const { return b ? V4{_mm_xor_ps(v, splat_bits(b).v)} : *this; }
    V4 operator+(V4 o) const { return {_mm_add_ps(v, o.v)}; }
    static V4 pmul(V4 a, V4 b)
    {
        __m128 r = _mm_mul_ps(a.v, b.v);
        __m128 fix = _mm_andnot_ps(_mm_cmpunord_ps(a.v, b.v), _mm_cmpunord_ps(r, r));
        return {_mm_andnot_ps(fix, r)};
    }
    static V4 max(V4 a, V4 b) { return {_mm_max_ps(a.v, b.v)}; }
    static V4 min(V4 a, V4 b) { return {_mm_min_ps(a.v, b.v)}; }
    static V4 select(const uint32_t* mask, V4 a, V4 b)
    {
        __m128 m = _mm_loadu_ps(reinterpret_cast<const float*>(mask));
        return {_mm_or_ps(_mm_and_ps(m, a.v), _mm_andnot_ps(m, b.v))};
    }
    float x() const { return _mm_cvtss_f32(v); }
    V4 with_w(float w) const
    {
        float t[4];
        store(t);
        t[3] = w;
        return load(t);
    }
};
#else
struct V4
{
    float f[4];
    static V4 load(const float* p) { V4 r; memcpy(r.f, p, 16); return r; }
    static V4 set(float a, float b, float c, float d) { return {{a, b, c, d}}; }
    static V4 splat_bits(uint32_t b) { float x = std::bit_cast<float>(b); return {{x, x, x, x}}; }
    void store(float* p) const { memcpy(p, f, 16); }
    V4 xor_bits(uint32_t b) const
    {
        V4 r;
        for (int j = 0; j < 4; j++)
            r.f[j] = std::bit_cast<float>(std::bit_cast<uint32_t>(f[j]) ^ b);
        return r;
    }
    V4 operator+(V4 o) const { return {{f[0] + o.f[0], f[1] + o.f[1], f[2] + o.f[2], f[3] + o.f[3]}}; }
    static V4 pmul(V4 a, V4 b)
    {
        V4 r;
        for (int j = 0; j < 4; j++)
        {
            r.f[j] = a.f[j] * b.f[j];
            if (std::isnan(r.f[j]) && !std::isnan(a.f[j]) && !std::isnan(b.f[j]))
                r.f[j] = 0.f;
        }
        return r;
    }
    static V4 max(V4 a, V4 b) { V4 r; for (int j = 0; j < 4; j++) r.f[j] = a.f[j] > b.f[j] ? a.f[j] : b.f[j]; return r; }
    static V4 min(V4 a, V4 b) { V4 r; for (int j = 0; j < 4; j++) r.f[j] = a.f[j] < b.f[j] ? a.f[j] : b.f[j]; return r; }
    static V4 select(const uint32_t* mask, V4 a, V4 b)
    {
        V4 r;
        for (int j = 0; j < 4; j++)
            r.f[j] = mask[j] ? a.f[j] : b.f[j];
        return r;
    }
    float x() const { return f[0]; }
    V4 with_w(float w) const { V4 r = *this; r.f[3] = w; return r; }
};
#endif

inline bool valid_dest(uint8_t d)
{
    return d < 0x7 || (d >= 0x10 && d < 0x20);
}

} // namespace

void GPU::shader_decode_desc(ShaderUnit& sh, uint32_t index)
{
    uint32_t od = sh.op_desc[index];
    ShaderDescDecoded& ds = sh.descs[index];
    static const int neg_shift[3] = {4, 13, 22};
    for (int k = 0; k < 3; k++)
    {
        ds.neg[k] = ((od >> neg_shift[k]) & 0x1) ? 0x80000000u : 0;
        uint32_t compsel = (od >> (neg_shift[k] + 1)) & 0xFF;
        for (int i = 0; i < 4; i++)
            ds.sel[k][3 - i] = (compsel >> (i * 2)) & 0x3;
    }
    ds.plain = 0;
    for (int k = 0; k < 3; k++)
    {
        if (!ds.neg[k] && ds.sel[k][0] == 0 && ds.sel[k][1] == 1 && ds.sel[k][2] == 2 && ds.sel[k][3] == 3)
            ds.plain |= 1 << k;
    }
    ds.wmask = 0;
    for (int i = 0; i < 4; i++)
    {
        if (od & (1 << i))
            ds.wmask |= 1 << (3 - i);
    }
    for (int j = 0; j < 4; j++)
        ds.wm[j] = (ds.wmask & (1 << j)) ? 0xFFFFFFFFu : 0;
}

void GPU::shader_reset_decode(ShaderUnit& sh)
{
    memset(sh.dec, 0, sizeof(sh.dec));
    for (uint32_t i = 0; i < 128; i++)
        shader_decode_desc(sh, i);
}

void GPU::shader_upload_code(ShaderUnit& sh, uint32_t param)
{
    //The upload offset is a word index into the 4096 word program memory
    uint32_t w = sh.code_index & 0xFFF;
    memcpy(&sh.code[w * 4], &param, 4);
    sh.dec[w].valid = 0;
    sh.code_index = (sh.code_index + 1) & 0xFFF;
}

void GPU::shader_upload_desc(ShaderUnit& sh, uint32_t param)
{
    uint32_t i = sh.op_desc_index & 0x7F;
    sh.op_desc[i] = param;
    shader_decode_desc(sh, i);
    sh.op_desc_index = (sh.op_desc_index + 1) & 0x7F;
}

void GPU::shader_decode(ShaderUnit& sh, uint32_t w)
{
    uint32_t instr;
    memcpy(&instr, &sh.code[w * 4], 4);

    ShaderDecoded d = {};
    d.op = OP_LEGACY;
    d.ix = 0xFF;

    //Format 1: dest, indexed src1 (7 bits), src2 (5 bits)
    auto fmt1 = [&](uint8_t op) {
        d.desc = instr & 0x7F;
        d.s[1] = (instr >> 7) & 0x1F;
        d.s[0] = (instr >> 12) & 0x7F;
        d.idx = (instr >> 19) & 0x3;
        d.ix = 0;
        d.dest = (instr >> 21) & 0x1F;
        d.op = valid_dest(d.dest) ? op : (uint8_t)OP_LEGACY;
    };
    //Format 1i: dest, src1 (5 bits), indexed src2 (7 bits)
    auto fmt1i = [&](uint8_t op) {
        d.desc = instr & 0x7F;
        d.s[1] = (instr >> 7) & 0x7F;
        d.s[0] = (instr >> 14) & 0x1F;
        d.idx = (instr >> 19) & 0x3;
        d.ix = 1;
        d.dest = (instr >> 21) & 0x1F;
        d.op = valid_dest(d.dest) ? op : (uint8_t)OP_LEGACY;
    };

    uint32_t opcode = instr >> 26;
    switch (opcode)
    {
        case 0x00: fmt1(OP_ADD); break;
        case 0x01: fmt1(OP_DP3); break;
        case 0x02: fmt1(OP_DP4); break;
        case 0x03: fmt1(OP_DPH); break;
        case 0x05: fmt1(OP_EX2); break;
        case 0x06: fmt1(OP_LG2); break;
        case 0x08: fmt1(OP_MUL); break;
        case 0x09: fmt1(OP_SGE); break;
        case 0x0A: fmt1(OP_SLT); break;
        case 0x0B: fmt1(OP_FLR); break;
        case 0x0C: fmt1(OP_MAX); break;
        case 0x0D: fmt1(OP_MIN); break;
        case 0x0E: fmt1(OP_RCP); break;
        case 0x0F: fmt1(OP_RSQ); break;
        case 0x12:
            fmt1(OP_MOVA);
            d.op = OP_MOVA; //no destination register
            break;
        case 0x13: fmt1(OP_MOV); break;
        case 0x1A: fmt1i(OP_SGE); break;
        case 0x1B: fmt1i(OP_SLT); break;
        case 0x21: d.op = OP_NOP; break;
        case 0x22: d.op = OP_END; break;
        case 0x2E:
        case 0x2F:
        {
            fmt1(OP_CMP);
            uint8_t op0 = (instr >> 24) & 0x7, op1 = (instr >> 21) & 0x7;
            d.dest = op0 | (op1 << 4);
            d.op = (op0 <= 5 && op1 <= 5) ? OP_CMP : OP_LEGACY;
            break;
        }
        case 0x30: case 0x31: case 0x32: case 0x33:
        case 0x34: case 0x35: case 0x36: case 0x37:
            //MADI: src1, src2 (5 bits), indexed src3 (7 bits)
            d.desc = instr & 0x1F;
            d.s[2] = (instr >> 5) & 0x7F;
            d.s[1] = (instr >> 12) & 0x1F;
            d.s[0] = (instr >> 17) & 0x1F;
            d.idx = (instr >> 22) & 0x3;
            d.ix = 2;
            d.dest = (instr >> 24) & 0x1F;
            d.op = valid_dest(d.dest) ? OP_MAD : OP_LEGACY;
            break;
        case 0x38: case 0x39: case 0x3A: case 0x3B:
        case 0x3C: case 0x3D: case 0x3E: case 0x3F:
            //MAD: src1, indexed src2 (7 bits), src3 (5 bits)
            d.desc = instr & 0x1F;
            d.s[2] = (instr >> 5) & 0x1F;
            d.s[1] = (instr >> 10) & 0x7F;
            d.s[0] = (instr >> 17) & 0x1F;
            d.idx = (instr >> 22) & 0x3;
            d.ix = 1;
            d.dest = (instr >> 24) & 0x1F;
            d.op = valid_dest(d.dest) ? OP_MAD : OP_LEGACY;
            break;
        default:
            break;
    }
    //The address register offset only applies to uniforms
    if (d.ix > 2 || d.s[d.ix] < 0x20)
        d.idx = 0;
    d.valid = 1;
    sh.dec[w] = d;
}

void GPU::exec_shader(ShaderUnit& sh)
{
    Profiler::Scope prof_scope(Profiler::GPU_SHADER);
    if (&sh == &ctx.vsh)
        stats.vsh_runs++;

    static const bool legacy = [] {
        const char* v = getenv("BLOSSOM_SHADER");
        return v && !strcmp(v, "legacy");
    }();
    if (legacy)
        exec_shader_legacy(sh);
    else
        exec_shader_fast(sh);
}

void GPU::exec_shader_fast(ShaderUnit& sh)
{
    for (int i = 0; i < sh.total_inputs; i++)
        sh.regs[sh.input_mapping[i]] = sh.input_attrs[i];

    sh.loop_ptr = 0;
    sh.call_ptr = 0;
    sh.if_ptr = 0;
    sh.pc = sh.entry_point * 4;

    uint64_t count = 0;

    //Loads operand k with its swizzle and negation applied
    auto load = [&](const ShaderDecoded& d, const ShaderDescDecoded& ds, int k) -> V4 {
        uint32_t r = d.s[k];
        if (k == d.ix && d.idx)
        {
            int off;
            switch (d.idx)
            {
                case 1: off = (int)sh.addr_reg[0].ToFloat32(); break;
                case 2: off = (int)sh.addr_reg[1].ToFloat32(); break;
                default: off = (int)sh.loop_ctr_reg; break;
            }
            r = (uint8_t)(r + off);
            if (r >= 0x80)
                return V4::splat_bits(ds.neg[k]); //past the last uniform: zero, negated
        }
        const float* src = reinterpret_cast<const float*>(&sh.regs[r]);
        if (ds.plain & (1 << k))
            return V4::load(src);
        return V4::set(src[ds.sel[k][0]], src[ds.sel[k][1]], src[ds.sel[k][2]], src[ds.sel[k][3]])
            .xor_bits(ds.neg[k]);
    };

    auto store = [&](const ShaderDecoded& d, const ShaderDescDecoded& ds, V4 v) {
        float* dst = reinterpret_cast<float*>(d.dest < 0x10 ? &sh.output_regs[d.dest] : &sh.regs[d.dest]);
        if (ds.wmask == 0xF)
            v.store(dst);
        else
            V4::select(ds.wm, v, V4::load(dst)).store(dst);
    };

    auto splat = [](float f) { return V4::set(f, f, f, f); };

    while (true)
    {
        count++;
        uint32_t w = (sh.pc >> 2) & 0xFFF;
        if (!sh.dec[w].valid)
            shader_decode(sh, w);
        const ShaderDecoded d = sh.dec[w];
        sh.pc += 4;

        const ShaderDescDecoded& ds = sh.descs[d.desc];
        switch (d.op)
        {
            case OP_NOP:
                break;
            case OP_END:
                goto done;
            case OP_ADD:
                store(d, ds, load(d, ds, 0) + load(d, ds, 1));
                break;
            case OP_MUL:
                store(d, ds, V4::pmul(load(d, ds, 0), load(d, ds, 1)));
                break;
            case OP_DP3:
            case OP_DP4:
            case OP_DPH:
            {
                V4 a = load(d, ds, 0);
                if (d.op == OP_DPH)
                    a = a.with_w(1.0f);
                float p[4];
                V4::pmul(a, load(d, ds, 1)).store(p);
                float dp = 0.0f;
                dp += p[0];
                dp += p[1];
                dp += p[2];
                if (d.op != OP_DP3)
                    dp += p[3];
                store(d, ds, splat(dp));
                break;
            }
            case OP_MAX:
                store(d, ds, V4::max(load(d, ds, 0), load(d, ds, 1)));
                break;
            case OP_MIN:
                store(d, ds, V4::min(load(d, ds, 0), load(d, ds, 1)));
                break;
            case OP_SGE:
            case OP_SLT:
            {
                float a[4], b[4], r[4];
                load(d, ds, 0).store(a);
                load(d, ds, 1).store(b);
                for (int j = 0; j < 4; j++)
                    r[j] = (d.op == OP_SGE ? a[j] >= b[j] : a[j] < b[j]) ? 1.0f : 0.0f;
                store(d, ds, V4::load(r));
                break;
            }
            case OP_FLR:
            {
                float a[4];
                load(d, ds, 0).store(a);
                for (int j = 0; j < 4; j++)
                    a[j] = std::floor(a[j]);
                store(d, ds, V4::load(a));
                break;
            }
            case OP_EX2:
                store(d, ds, splat(std::exp2(load(d, ds, 0).x())));
                break;
            case OP_LG2:
                store(d, ds, splat(std::log2(load(d, ds, 0).x())));
                break;
            case OP_RCP:
                store(d, ds, splat(1.0f / load(d, ds, 0).x()));
                break;
            case OP_RSQ:
                store(d, ds, splat(1.0f / sqrtf(load(d, ds, 0).x())));
                break;
            case OP_MOV:
                store(d, ds, load(d, ds, 0));
                break;
            case OP_MOVA:
            {
                float a[4];
                load(d, ds, 0).store(a);
                if (ds.wmask & 0x1)
                    sh.addr_reg[0] = float24::FromFloat32(a[0]);
                if (ds.wmask & 0x2)
                    sh.addr_reg[1] = float24::FromFloat32(a[1]);
                break;
            }
            case OP_CMP:
            {
                float a[4], b[4];
                load(d, ds, 0).store(a);
                load(d, ds, 1).store(b);
                for (int i = 0; i < 2; i++)
                {
                    bool v = false;
                    switch ((d.dest >> (i * 4)) & 0x7)
                    {
                        case 0: v = a[i] == b[i]; break;
                        case 1: v = a[i] != b[i]; break;
                        case 2: v = a[i] < b[i]; break;
                        case 3: v = a[i] <= b[i]; break;
                        case 4: v = a[i] > b[i]; break;
                        case 5: v = a[i] >= b[i]; break;
                    }
                    sh.cmp_regs[i] = v;
                }
                break;
            }
            case OP_MAD:
            {
                V4 a = load(d, ds, 0), b = load(d, ds, 1), c = load(d, ds, 2);
                store(d, ds, c + V4::pmul(b, a));
                break;
            }
            default:
            {
                uint32_t instr;
                memcpy(&instr, &sh.code[w * 4], 4);
                if (exec_shader_instr(sh, instr))
                    goto done;
                break;
            }
        }

        if (sh.loop_ptr | sh.if_ptr | sh.call_ptr)
            shader_flow_post(sh);
    }
done:
    if (&sh == &ctx.vsh)
        stats.vsh_instrs += count;
}
