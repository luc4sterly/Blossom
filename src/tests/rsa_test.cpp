//RSA engine checks: the exponent FIFO must accept both a full 0x100-byte exponent and the single
//word (0x00010001) that NATIVE_FIRM's Process9 writes after clearing a slot's "key set" bit.
#include <cstdio>
#include <cstring>
#include <gmp.h>
#include "core/arm9/interrupt9.hpp"
#include "core/arm9/rsa.hpp"
#include "core/cpu/arm.hpp"

static int failures = 0;

static void check(bool ok, const char* what)
{
    printf("%s: %s\n", ok ? "OK" : "FAIL", what);
    if (!ok)
        failures++;
}

//Write a 0x100-byte big-endian number to a FIFO, most significant word first
static void write_be(RSA& rsa, uint32_t addr, const uint8_t* bytes)
{
    for (int i = 0; i < 0x100; i += 4)
    {
        uint32_t v = (bytes[i] << 24) | (bytes[i + 1] << 16) | (bytes[i + 2] << 8) | bytes[i + 3];
        rsa.write32(addr + i, v);
    }
}

static void to_bytes(mpz_t x, uint8_t* out)
{
    size_t count = 0;
    memset(out, 0, 0x100);
    uint8_t tmp[0x100];
    mpz_export(tmp, &count, 1, 1, 1, 0, x);
    memcpy(out + 0x100 - count, tmp, count);
}

static bool run(RSA& rsa, bool short_exponent, const uint8_t* mod, const uint8_t* msg, const uint8_t* expected)
{
    const uint32_t cnt = 1 << 9; //keyslot 0, little-endian words (as the ARM9 writes them), normal word order
    rsa.write32(0x1000B000, cnt);
    rsa.write32(0x1000B100, 0); //clear "key set"
    if (short_exponent)
        rsa.write32(0x1000B200, 0x00010001);
    else
    {
        uint8_t exp[0x100] = {};
        exp[0xFD] = 1;
        exp[0xFF] = 1;
        write_be(rsa, 0x1000B200, exp);
    }
    write_be(rsa, 0x1000B400, mod);
    write_be(rsa, 0x1000B800, msg);
    rsa.write32(0x1000B000, cnt | 1);

    uint8_t out[0x100];
    for (int i = 0; i < 0x100; i += 4)
    {
        uint32_t v = rsa.read32(0x1000B800 + i);
        out[i] = v >> 24;
        out[i + 1] = v >> 16;
        out[i + 2] = v >> 8;
        out[i + 3] = v;
    }
    return memcmp(out, expected, 0x100) == 0;
}

int main()
{
    ARM_CPU cpu(nullptr, 9, nullptr, nullptr);
    Interrupt9 int9(&cpu);
    int9.write_ie(0);
    RSA rsa(&int9);
    rsa.reset();

    mpz_t n, m, e, r;
    mpz_inits(n, m, e, r, NULL);
    gmp_randstate_t st;
    gmp_randinit_default(st);
    gmp_randseed_ui(st, 3);
    mpz_urandomb(n, st, 2048);
    mpz_setbit(n, 2047);
    mpz_setbit(n, 0);
    mpz_urandomm(m, st, n);
    mpz_set_ui(e, 0x10001);
    mpz_powm(r, m, e, n);

    uint8_t mod[0x100], msg[0x100], expected[0x100];
    to_bytes(n, mod);
    to_bytes(m, msg);
    to_bytes(r, expected);

    check(run(rsa, false, mod, msg, expected), "full 0x100-byte exponent");
    check(run(rsa, true, mod, msg, expected), "single-word exponent after clearing the slot");
    check(run(rsa, false, mod, msg, expected), "full exponent again after a short one");

    mpz_clears(n, m, e, r, NULL);
    gmp_randclear(st);
    return failures ? 1 : 0;
}
