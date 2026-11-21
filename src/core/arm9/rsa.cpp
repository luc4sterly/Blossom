#include <cstdio>
#include <cstring>
#include <sstream>
#include "common/common.hpp"
#include "core/arm9/interrupt9.hpp"
#include "core/arm9/rsa.hpp"

#define printf(fmt,...) (void)(0)

RSA::RSA(Interrupt9* int9) : int9(int9)
{

}

void RSA::set_accept_self_signed(bool accept)
{
    accept_self_signed = accept;
    self_signed_logged = false;
}

//True if x, as a 2048-bit big-endian block, starts with 00 01 FF (PKCS#1 v1.5 signature padding)
static bool looks_like_pkcs1_block(mpz_t x)
{
    mpz_t top;
    mpz_init(top);
    mpz_tdiv_q_2exp(top, x, 2048 - 24);
    bool result = mpz_cmp_ui(top, 0x01FF) == 0;
    mpz_clear(top);
    return result;
}

void RSA::reset()
{
    memset(&RSA_CNT, 0, sizeof(RSA_CNT));
    memset(keys, 0, sizeof(keys));
    msg_ctr = 0;
    rsa_op_count = 0;
}

uint8_t RSA::read8(uint32_t addr)
{
    if (addr >= 0x1000B800 && addr < 0x1000B900)
    {
        int index = addr & 0xFF;
        if (!RSA_CNT.word_order)
            index = 0xFF - index;
        return msg[index];
    }
    printf("[RSA] Unrecognized read8 $%08X\n", addr);
    return 0;
}

uint32_t RSA::read32(uint32_t addr)
{
    uint32_t reg = 0;
    if (addr >= 0x1000B100 && addr < 0x1000B140)
    {
        int index = (addr / 0x10) & 0x3;
        int reg = (addr / 4) & 0x3;

        switch (reg)
        {
            case 0:
                //reg |= keys[index].key_set;
                reg = 1;
                reg |= keys[index].write_protect << 1;
                printf("[RSA] Read key%d cnt\n", index);
                return reg;
            case 1:
                printf("[RSA] Read key%d size\n", index);
                return 0x40;
        }
    }

    if (addr >= 0x1000B800 && addr < 0x1000B900)
    {
        int index = addr & 0xFF;
        if (!RSA_CNT.word_order)
            index = 0xFC - index;
        uint32_t value = *(uint32_t*)&msg[index];
        if (!RSA_CNT.big_endian)
            value = bswp32(value);
        printf("[RSA] Read TXT $%08X: $%08X\n", addr, value);
        return value;
    }

    switch (addr)
    {
        case 0x1000B000:
            reg |= RSA_CNT.keyslot << 4;
            reg |= RSA_CNT.big_endian << 8;
            reg |= RSA_CNT.word_order << 9;
            printf("[RSA] Read CNT: $%08X\n", reg);
            break;
        default:
            printf("[RSA] Unrecognized read32 $%08X\n", addr);
            return 0;
    }
    return reg;
}

void RSA::write8(uint32_t addr, uint8_t value)
{
    if (addr >= 0x1000B200 && addr < 0x1000B300)
    {
        printf("[RSA] Write8 key%d exp: $%02X\n", RSA_CNT.keyslot, value);
        RSA_KeySlot* key = &keys[RSA_CNT.keyslot];
        //if (RSA_CNT.big_endian)
            //value = bswp32(value);

        //The exponent FIFO shifts in from the low end, so a short exponent works.
        memmove(key->exp, key->exp + 1, 0xFF);
        key->exp[0xFF] = value;
        return;
    }

    if (addr >= 0x1000B400 && addr < 0x1000B500)
    {
        printf("[RSA] Write8 key%d mod: $%02X\n", RSA_CNT.keyslot, value);
        RSA_KeySlot* key = &keys[RSA_CNT.keyslot];
        //if (RSA_CNT.big_endian)
            //value = bswp32(value);

        int index = key->mod_ctr;
        if (!RSA_CNT.word_order)
            index = 0xFF - index;

        key->mod[index] = value;
        key->mod_ctr++;
        if (key->mod_ctr >= 0x100)
        {
            key->mod_ctr = 0;
        }
        return;
    }

    if (addr >= 0x1000B800 && addr < 0x1000B900)
    {
        printf("[RSA] Write TXT: $%02X (%d)\n", value, msg_ctr);
        if (!RSA_CNT.word_order)
            msg[0xFF - msg_ctr] = value;
        else
            msg[msg_ctr] = value;

        msg_ctr++;
        if (msg_ctr >= 0x100)
            msg_ctr = 0;
        return;
    }

    printf("[RSA] Unrecognized write8 $%08X\n", addr);
}

void RSA::write32(uint32_t addr, uint32_t value)
{
    if (addr >= 0x1000B100 && addr < 0x1000B140)
    {
        int index = (addr / 0x10) & 0x3;
        int reg = (addr / 4) & 0x3;

        switch (reg)
        {
            case 0:
                printf("[RSA] Write32 key%d cnt: $%08X\n", index, value);
                keys[index].write_protect = value & (1 << 1);
                //Clearing the "key set" bit starts a new key: the exponent FIFO and the modulus
                //write position start over. Software such as Process9 then writes the exponent as
                //a single word (0x10001), not as the full 0x100 bytes.
                if (!(value & 0x1) && !keys[index].write_protect)
                {
                    memset(keys[index].exp, 0, sizeof(keys[index].exp));
                    keys[index].exp_ctr = 0;
                    keys[index].mod_ctr = 0;
                }
                return;
        }
    }

    if (addr >= 0x1000B200 && addr < 0x1000B300)
    {
        printf("[RSA] Write32 key%d exp: $%08X\n", RSA_CNT.keyslot, value);
        RSA_KeySlot* key = &keys[RSA_CNT.keyslot];
        if (!RSA_CNT.big_endian)
            value = bswp32(value);

        //The exponent FIFO shifts in from the low end: writing the full 0x100 bytes gives the same
        //result as before, and a single word (e.g. 0x00010001) gives exponent 0x10001.
        memmove(key->exp, key->exp + 4, 0xFC);
        memcpy(&key->exp[0xFC], &value, 4);
        return;
    }

    if (addr >= 0x1000B400 && addr < 0x1000B500)
    {
        printf("[RSA] Write32 key%d mod: $%08X\n", RSA_CNT.keyslot, value);
        RSA_KeySlot* key = &keys[RSA_CNT.keyslot];
        if (!RSA_CNT.big_endian)
            value = bswp32(value);

        int index = key->mod_ctr;
        if (!RSA_CNT.word_order)
            index = 0xFC - index;

        *(uint32_t*)&key->mod[index] = value;
        key->mod_ctr += 4;
        if (key->mod_ctr >= 0x100)
        {
            key->mod_ctr = 0;
        }
        return;
    }

    if (addr >= 0x1000B800 && addr < 0x1000B900)
    {
        printf("[RSA] Write TXT: $%08X (%d)\n", value, msg_ctr);
        if (!RSA_CNT.big_endian)
            value = bswp32(value);
        if (!RSA_CNT.word_order)
            *(uint32_t*)&msg[0xFC - msg_ctr] = value;
        else
            *(uint32_t*)&msg[msg_ctr] = value;

        msg_ctr += 4;
        if (msg_ctr >= 0x100)
            msg_ctr = 0;
        return;
    }

    switch (addr)
    {
        case 0x1000B000:
            printf("[RSA] Write CNT: $%08X\n", value);
            RSA_CNT.keyslot = (value >> 4) & 0x3;
            RSA_CNT.big_endian = (value >> 8) & 0x1;
            RSA_CNT.word_order = (value >> 9) & 0x1;

            if (value & 0x1)
                do_rsa_op();
            break;
        default:
            printf("[RSA] Unrecognized write32 $%08X: $%08X\n", addr, value);
    }
}

void RSA::convert_to_bignum(uint8_t *src, mpz_t dest)
{
    mpz_t base, temp;

    mpz_init(temp);
    mpz_init_set_ui(dest, 0);
    mpz_init_set_ui(base, 1);
    for (int i = 0xFF; i >= 0; i--)
    {
        mpz_set_ui(temp, src[i]);
        mpz_mul(temp, temp, base);
        mpz_add(dest, dest, temp);
        mpz_mul_ui(base, base, 256);
    }
}

void RSA::do_rsa_op()
{
    mpz_t gmp_msg, gmp_b, gmp_e, gmp_m;

    RSA_KeySlot* key = &keys[RSA_CNT.keyslot];
    mpz_inits(gmp_msg, NULL);
    convert_to_bignum((uint8_t*)msg, gmp_b);
    convert_to_bignum((uint8_t*)key->exp, gmp_e);
    convert_to_bignum((uint8_t*)key->mod, gmp_m);

    printf("Msg: %s\n", mpz_get_str(NULL, 16, gmp_b));
    printf("Exp: %s\n", mpz_get_str(NULL, 16, gmp_e));
    printf("Mod: %s\n", mpz_get_str(NULL, 16, gmp_m));

    mpz_powm(gmp_msg, gmp_b, gmp_e, gmp_m);
    printf("Result: %s\n", mpz_get_str(NULL, 16, gmp_msg));

    rsa_op_count++;
    bool accepted_self_signed = false;

    //A real signature decrypts to a PKCS#1 block. A "self-signed" one, written by tools/mknand for
    //headers no real key can sign (the NAND NCSD header), already *is* the block. In development
    //mode, treat it as if the exponent were 1 so the verifier sees the block it expects. Real
    //signatures are never touched, because they already decrypt to a valid block.
    if (accept_self_signed && !looks_like_pkcs1_block(gmp_msg) && looks_like_pkcs1_block(gmp_b))
    {
        if (!self_signed_logged)
        {
            fprintf(stderr, "[RSA] --dev-unsigned: accepting a self-signed block (keyslot %d)\n",
                    RSA_CNT.keyslot);
            self_signed_logged = true;
        }
        mpz_set(gmp_msg, gmp_b);
        accepted_self_signed = true;
    }

    //Public information only: which keyslot, the exponent and modulus sizes, and whether the
    //result is a PKCS#1 signature block.
    fprintf(stderr, "[RSA] op #%d keyslot %d: exponent %zu bits, modulus %zu bits, result %s%s\n",
            rsa_op_count, RSA_CNT.keyslot, mpz_sizeinbase(gmp_e, 2), mpz_sizeinbase(gmp_m, 2),
            looks_like_pkcs1_block(gmp_msg) ? "is a PKCS#1 block" : "is not a PKCS#1 block",
            accepted_self_signed ? " (self-signed, --dev-unsigned)" : "");

    convert_from_bignum(gmp_msg, msg);
    int9->assert_irq(22);
}

void RSA::convert_from_bignum(mpz_t src, uint8_t* dest)
{
    std::string str = mpz_get_str(NULL, 16, src);

    while (str.length() < 0x200)
        str = '0' + str;

    for (int i = 0; i < 0x100; i++)
    {
        uint8_t chr1 = str[(i * 2)];
        uint8_t chr2 = str[(i * 2) + 1];

        if (chr1 >= 'a')
            chr1 = (chr1 - 'a') + 10;
        else
            chr1 -= '0';

        if (chr2 >= 'a')
            chr2 = (chr2 - 'a') + 10;
        else
            chr2 -= '0';

        uint8_t byte = (chr1 << 4) | chr2;

        dest[i] = byte;
    }
}
