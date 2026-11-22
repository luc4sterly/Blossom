// Blossom: the pre-decoded shader interpreter must give bit-identical results to the original one.
// Runs random vertex programs (arithmetic, compares, address register indexing, loops) on random
// inputs and uniforms, including signed zeros, infinities and NaNs, with both interpreters.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "core/arm11/gpu.hpp"
#include "core/emulator.hpp"

namespace {

uint32_t rng_state = 12345;
uint32_t rnd()
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

float special()
{
    switch (rnd() % 12)
    {
        case 0: return 0.0f;
        case 1: return -0.0f;
        case 2: return std::numeric_limits<float>::infinity();
        case 3: return -std::numeric_limits<float>::infinity();
        case 4: return std::numeric_limits<float>::quiet_NaN();
        case 5: return (float)(int)(rnd() % 12) - 3.0f; //small ints, for MOVA
        case 6: return 1.0f;
        default: return ((int)(rnd() % 20001) - 10000) / 997.0f;
    }
}

uint32_t dest()
{
    return (rnd() & 1) ? rnd() % 7 : 0x10 + rnd() % 16;
}

uint32_t random_instr()
{
    static const uint32_t fmt1_ops[] = {0x00, 0x01, 0x02, 0x03, 0x08, 0x0C, 0x0E, 0x0F, 0x12, 0x13};
    uint32_t desc_lo = rnd() % 32; //also used by MAD/MADI, which only see 32 descriptors
    switch (rnd() % 8)
    {
        case 0: case 1: case 2: case 3:
        {
            uint32_t op = fmt1_ops[rnd() % (sizeof(fmt1_ops) / 4)];
            uint32_t src1 = (rnd() & 1) ? 0x20 + rnd() % 96 : rnd() % 32;
            return (op << 26) | (dest() << 21) | ((rnd() % 4) << 19) | (src1 << 12) |
                   ((rnd() % 32) << 7) | desc_lo;
        }
        case 4: //SLTI
        {
            uint32_t src2 = (rnd() & 1) ? 0x20 + rnd() % 96 : rnd() % 32;
            return (0x1Bu << 26) | (dest() << 21) | ((rnd() % 4) << 19) | ((rnd() % 32) << 14) |
                   (src2 << 7) | desc_lo;
        }
        case 5: //CMP
        {
            uint32_t src1 = (rnd() & 1) ? 0x20 + rnd() % 96 : rnd() % 32;
            return (0x2Eu << 26) | ((rnd() % 6) << 24) | ((rnd() % 6) << 21) | ((rnd() % 4) << 19) |
                   (src1 << 12) | ((rnd() % 32) << 7) | desc_lo;
        }
        case 6: //MADI
        {
            uint32_t src3 = (rnd() & 1) ? 0x20 + rnd() % 96 : rnd() % 32;
            return (0x30u << 26) | (dest() << 24) | ((rnd() % 4) << 22) | ((rnd() % 32) << 17) |
                   ((rnd() % 32) << 12) | (src3 << 5) | desc_lo;
        }
        default: //MAD
        {
            uint32_t src2 = (rnd() & 1) ? 0x20 + rnd() % 96 : rnd() % 32;
            return (0x38u << 26) | (dest() << 24) | ((rnd() % 4) << 22) | ((rnd() % 32) << 17) |
                   (src2 << 10) | ((rnd() % 32) << 5) | desc_lo;
        }
    }
}

std::vector<uint32_t> random_program()
{
    std::vector<uint32_t> p;
    int n = 4 + rnd() % 40;
    for (int i = 0; i < n; i++)
    {
        if (rnd() % 10 == 0 && p.size() + 6 < 4000)
        {
            //LOOP over a few instructions: exercises loop_ctr indexing
            uint32_t body = 1 + rnd() % 4;
            uint32_t end = (uint32_t)p.size() + body; //last instruction of the body
            p.push_back((0x29u << 26) | ((rnd() % 4) << 22) | (end << 10));
            for (uint32_t b = 0; b < body; b++)
                p.push_back(random_instr());
            continue;
        }
        p.push_back(random_instr());
    }
    p.push_back(0x22u << 26); //END
    return p;
}

//Bit-identical, except that any NaN matches any NaN: which NaN an operation on two NaNs returns
//depends on the operand order the compiler picks for commutative operations
bool same(const void* a, const void* b, size_t n)
{
    const float* x = (const float*)a;
    const float* y = (const float*)b;
    for (size_t i = 0; i < n / 4; i++)
    {
        if (std::isnan(x[i]) && std::isnan(y[i]))
            continue;
        if (memcmp(&x[i], &y[i], 4))
            return false;
    }
    return true;
}

} // namespace

int main()
{
    auto e = std::make_unique<Emulator>();
    e->reset();
    GPU& g = e->gpu_for_tests();

    int failures = 0;
    const int programs = 3000;
    uint64_t instrs = 0;
    for (int t = 0; t < programs; t++)
    {
        std::vector<uint32_t> prog = random_program();
        g.test_write_reg(0x2CB, 0);
        for (uint32_t w : prog)
            g.test_write_reg(0x2CC, w);
        g.test_write_reg(0x2D5, 0);
        for (int i = 0; i < 128; i++)
            g.test_write_reg(0x2D6, rnd() & 0x7FFFFFFF);
        g.test_write_reg(0x2BA, 0);
        g.test_write_reg(0x2B9, 15);
        g.test_write_reg(0x2BB, 0x76543210);
        g.test_write_reg(0x2BC, 0xFEDCBA98);
        for (int i = 0; i < 4; i++)
            g.test_write_reg(0x2B1 + i, (rnd() % 4) | ((rnd() % 8) << 8) | ((rnd() % 3) << 16));
        g.test_write_reg(0x2C0, 0x80000000); //float32 uniforms from index 0
        for (int i = 0; i < 96 * 4; i++)
        {
            float f = special();
            uint32_t bits;
            memcpy(&bits, &f, 4);
            g.test_write_reg(0x2C1, bits);
        }

        ShaderUnit& sh = g.test_vsh();
        for (int i = 0; i < 16; i++)
        {
            for (int j = 0; j < 4; j++)
            {
                sh.input_attrs[i][j] = float24::FromFloat32(special());
                sh.regs[0x10 + i][j] = float24::FromFloat32(special());
                sh.output_regs[i][j] = float24::FromFloat32(0.0f);
            }
        }
        sh.addr_reg[0] = float24::FromFloat32((float)(int)(rnd() % 9) - 4.0f);
        sh.addr_reg[1] = float24::FromFloat32((float)(int)(rnd() % 9) - 4.0f);
        sh.loop_ctr_reg = rnd() % 8;
        sh.cmp_regs[0] = sh.cmp_regs[1] = false;

        auto start = std::make_unique<ShaderUnit>(sh);
        g.test_exec_vsh(true);
        auto ref = std::make_unique<ShaderUnit>(sh);
        sh = *start;
        g.test_exec_vsh(false);
        instrs += prog.size();

        bool ok = same(ref->output_regs, sh.output_regs, sizeof(sh.output_regs)) &&
                  same(ref->regs, sh.regs, sizeof(sh.regs)) && ref->cmp_regs[0] == sh.cmp_regs[0] &&
                  ref->cmp_regs[1] == sh.cmp_regs[1] &&
                  same(&ref->addr_reg, &sh.addr_reg, sizeof(sh.addr_reg)) && ref->pc == sh.pc;
        if (!ok)
        {
            if (failures < 5)
            {
                printf("program %d differs (%zu words): out %d regs %d cmp %d addr %d pc %d\n", t, prog.size(),
                       !same(ref->output_regs, sh.output_regs, sizeof(sh.output_regs)),
                       !same(ref->regs, sh.regs, sizeof(sh.regs)),
                       ref->cmp_regs[0] != sh.cmp_regs[0] || ref->cmp_regs[1] != sh.cmp_regs[1],
                       !same(&ref->addr_reg, &sh.addr_reg, sizeof(sh.addr_reg)), ref->pc != sh.pc);
                if (getenv("SHADER_DUMP"))
                    for (uint32_t w : prog)
                        printf("    %08X op %02X\n", w, w >> 26);
                for (int i = 0; i < 16; i++)
                {
                    if (!same(&ref->output_regs[i], &sh.output_regs[i], 16))
                        printf("  o%d: legacy %g %g %g %g fast %g %g %g %g\n", i, ref->output_regs[i][0].ToFloat32(),
                               ref->output_regs[i][1].ToFloat32(), ref->output_regs[i][2].ToFloat32(),
                               ref->output_regs[i][3].ToFloat32(), sh.output_regs[i][0].ToFloat32(),
                               sh.output_regs[i][1].ToFloat32(), sh.output_regs[i][2].ToFloat32(),
                               sh.output_regs[i][3].ToFloat32());
                }
            }
            failures++;
        }
    }
    //Speed of both interpreters on a typical transform program: 8 DP4 against uniforms, MOVs, MADs, MULs
    g.test_write_reg(0x2CB, 0);
    int bench_len = 0;
    for (int i = 0; i < 8; i++, bench_len++)
        g.test_write_reg(0x2CC, (0x02u << 26) | ((0x10 + i) << 21) | ((0x20 + i) << 12) | (0 << 7) | 0);
    for (int i = 0; i < 8; i++, bench_len++)
        g.test_write_reg(0x2CC, (0x38u << 26) | ((0x10 + i) << 24) | (1u << 17) | ((0x28 + i) << 10) | ((0x10 + i) << 5) | 1);
    for (int i = 0; i < 8; i++, bench_len++)
        g.test_write_reg(0x2CC, (0x08u << 26) | ((0x10 + i) << 21) | ((0x10 + i) << 12) | (2 << 7) | 2);
    for (int i = 0; i < 7; i++, bench_len++)
        g.test_write_reg(0x2CC, (0x13u << 26) | (i << 21) | ((0x10 + i) << 12) | 3);
    g.test_write_reg(0x2CC, 0x22u << 26);
    bench_len++;
    g.test_write_reg(0x2D5, 0);
    const uint32_t xyzw = 0xF | (0x1B << 5) | (0x1B << 14) | (0x1B << 23);
    for (int i = 0; i < 4; i++)
        g.test_write_reg(0x2D6, xyzw);
    for (int legacy = 1; legacy >= 0; legacy--)
    {
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 200000; i++)
            g.test_exec_vsh(legacy);
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        printf("%s: %.1f ns per instruction\n", legacy ? "legacy" : "fast", s * 1e9 / 200000 / bench_len);
    }
    printf("%d of %d random shader programs differ (%llu instructions)\n", failures, programs,
           (unsigned long long)instrs);
    return failures ? 1 : 0;
}
