#!/usr/bin/env python3
# Blossom - synthetic Old 3DS NAND builder
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds a raw Old 3DS NAND image that Blossom can boot through the emulated
# ARM9 bootrom, without a NAND dump from a real console:
#
#   * a made-up OTP (correct magic and SHA-256, encrypted with the OTP key
#     from *your* boot9.bin) and a made-up eMMC CID;
#   * an NCSD header with the standard Old 3DS partition table, "self-signed"
#     (see --help for what that means and why Blossom needs --dev-unsigned);
#   * essential.exefs at 0x200 with "otp" and "nand_cid", as GodMode9 writes it;
#   * FIRM0/FIRM1 holding NATIVE_FIRM, taken from its system title CIA;
#   * TWL and CTR MBRs, and empty FAT16 file systems on TWLN, TWLP and CTRNAND.
#
# All crypto (console keys from the OTP, NAND counters from the CID, CIA title
# keys) goes through pyctr (MIT, https://github.com/ihaveamac/pyctr).
#
# Nothing here ships keys or firmware: everything comes from files on your
# own machine.

from __future__ import annotations

import argparse
import hashlib
import logging
import os
import random
import struct
import sys
from dataclasses import dataclass

NAND_MEDIA_UNIT = 0x200
OTP_MAGIC = 0xDEADB00F

# Old 3DS NCSD layout (byte offsets / sizes). The image size field is in media
# units; 0x200000 MU is what every Old 3DS NAND header reports, and the real
# minimum image size for it is 0x3AF00000.
O3DS_IMAGE_SIZE_MU = 0x200000
O3DS_NAND_SIZE = 0x3AF00000

# (fs_type, crypt_type, offset, size)
O3DS_PARTITIONS = [
    (1, 1, 0x00000000, 0x0B100000),  # TWL area (TWL MBR, TWLN, TWLP)
    (4, 2, 0x0B100000, 0x00030000),  # AGB_FIRM save
    (3, 2, 0x0B130000, 0x00400000),  # FIRM0
    (3, 2, 0x0B530000, 0x00400000),  # FIRM1
    (1, 2, 0x0B930000, 0x2F5D0000),  # CTR area (CTR MBR, CTRNAND)
]

# Partitions inside the TWL area, in sectors relative to NAND offset 0.
# These are the values every retail TWL MBR uses.
TWL_MBR_ENTRIES = [
    # boot, chs_start, type, chs_end, lba, count
    (0x00, b'\x03\x18\x00', 0x06, b'\x01\xa0\x3f', 0x00000097, 0x00047DA9),  # TWLN
    (0x00, b'\x04\x8e\x40', 0x06, b'\x01\xa0\xc3', 0x0004808D, 0x000105B3),  # TWLP
]

# Partition inside the CTR area, in sectors relative to the CTR area start.
# Matches the CTRNAND partition GodMode9 expects on Old 3DS (0x0B95CA00, 0x2F3E3600).
CTR_MBR_ENTRIES = [
    (0x00, b'\x00\x00\x00', 0x06, b'\x00\x00\x00', 0x00000165, 0x00179F1B),
]

FIRM_PART_SIZE = 0x400000

# DER prefix of a PKCS#1 v1.5 DigestInfo for SHA-256.
SHA256_DIGESTINFO = bytes.fromhex('3031300d060960864801650304020105000420')

# NATIVE_FIRM title for Old 3DS.
O3DS_NATIVE_FIRM_TID = '0004013800000002'


class MkNandError(Exception):
    pass


# --------------------------------------------------------------------------------------------
# Pure builders (no keys needed). These are what the unit tests check.
# --------------------------------------------------------------------------------------------

def build_otp_plain(rng: random.Random, device_id: int | None = None) -> bytes:
    """Build a 0x100-byte decrypted OTP that passes boot9's hash check."""
    otp = bytearray(0x100)
    struct.pack_into('<I', otp, 0x00, OTP_MAGIC)
    struct.pack_into('<I', otp, 0x04, device_id if device_id is not None else rng.getrandbits(32))
    otp[0x08:0x18] = rng.randbytes(0x10)          # fallback keyY, also the TWL console ID source
    otp[0x18] = 5                                 # OTP version (>= 5: little-endian expiry)
    otp[0x19] = 0                                 # retail CTCert issuer
    otp[0x1A:0x20] = bytes([113, 6, 14, 10, 0, 0])  # manufacturing date: 2013-06-14 10:00:00
    struct.pack_into('<I', otp, 0x20, 0)          # CTCert expiry (unused offline)
    otp[0x24:0x44] = b'\0\0' + rng.randbytes(0x1E)  # CTCert ECDSA private key (fake, < curve order)
    otp[0x44:0x80] = rng.randbytes(0x3C)          # CTCert signature (fake, only matters online)
    otp[0x80:0x90] = bytes(0x10)
    otp[0x90:0xE0] = rng.randbytes(0x50)          # console key generation data
    otp[0xE0:0x100] = hashlib.sha256(otp[0:0xE0]).digest()
    return bytes(otp)


def build_cid(rng: random.Random) -> bytes:
    """Build a plausible-looking 16-byte eMMC CID. Only its SHA-256/SHA-1 matter for crypto."""
    return rng.randbytes(16)


def build_exefs(files: list[tuple[str, bytes]]) -> bytes:
    """Build an ExeFS image (0x200 header + 0x200-aligned files), like GodMode9's essential.exefs."""
    if len(files) > 10:
        raise MkNandError('ExeFS holds at most 10 files')
    header = bytearray(0x200)
    body = bytearray()
    for i, (name, data) in enumerate(files):
        encoded = name.encode('ascii')
        if len(encoded) > 8:
            raise MkNandError(f'ExeFS name too long: {name}')
        offset = len(body)
        struct.pack_into('<8sII', header, i * 0x10, encoded, offset, len(data))
        header[0x1E0 - i * 0x20:0x200 - i * 0x20] = hashlib.sha256(data).digest()
        body += data
        body += bytes(-len(body) % 0x200)
    return bytes(header + body)


def build_mbr_entries(entries) -> bytes:
    """Build the 0x42 bytes at 0x1BE of an MBR (4 entries + 0x55AA)."""
    out = bytearray(0x42)
    for i, (boot, chs_start, ptype, chs_end, lba, count) in enumerate(entries):
        struct.pack_into('<B3sB3sII', out, i * 0x10, boot, chs_start, ptype, chs_end, lba, count)
    out[0x40:0x42] = b'\x55\xAA'
    return bytes(out)


def build_ncsd_header_body() -> bytearray:
    """Build the 0x200-byte NCSD header with a zero signature and a zero TWL MBR."""
    hdr = bytearray(0x200)
    hdr[0x100:0x104] = b'NCSD'
    struct.pack_into('<I', hdr, 0x104, O3DS_IMAGE_SIZE_MU)
    struct.pack_into('<Q', hdr, 0x108, 0)  # media ID 0 = NAND
    for i, (fs_type, crypt_type, offset, size) in enumerate(O3DS_PARTITIONS):
        hdr[0x110 + i] = fs_type
        hdr[0x118 + i] = crypt_type
        struct.pack_into('<II', hdr, 0x120 + i * 8, offset // NAND_MEDIA_UNIT, size // NAND_MEDIA_UNIT)
    return hdr


def self_sign(message: bytes) -> bytes:
    """
    Return a 0x100-byte "signature" that is the PKCS#1 v1.5 SHA-256 block of `message` itself.

    No real key can verify this. Blossom started with --dev-unsigned treats a signature that is
    already a well-formed PKCS#1 block as if the RSA exponent were 1, so boot9 sees exactly the
    block it expects and accepts the header.
    """
    digest = hashlib.sha256(message).digest()
    pad = 0x100 - 3 - len(SHA256_DIGESTINFO) - len(digest)
    return b'\x00\x01' + b'\xff' * pad + b'\x00' + SHA256_DIGESTINFO + digest


def build_fat16_boot_and_tables(total_sectors: int, label: bytes, volume_id: int) -> tuple[bytes, int]:
    """
    Build the reserved sector, both FATs and the root directory of an empty FAT16 volume.

    Returns (data, length). `data` must be written at the start of the partition; everything after
    it is free cluster space and can stay as it is.
    """
    bytes_per_sector = 0x200
    reserved = 1
    num_fats = 2
    root_entries = 512
    root_sectors = root_entries * 32 // bytes_per_sector

    for spc in (1, 2, 4, 8, 16, 32, 64):
        # FAT size in sectors, solved iteratively.
        fat_sectors = 1
        while True:
            data_sectors = total_sectors - reserved - num_fats * fat_sectors - root_sectors
            clusters = data_sectors // spc
            needed = ((clusters + 2) * 2 + bytes_per_sector - 1) // bytes_per_sector
            if needed <= fat_sectors:
                break
            fat_sectors = needed
        if 4085 <= clusters < 65525:
            break
    else:
        raise MkNandError(f'cannot fit FAT16 in {total_sectors} sectors')

    bs = bytearray(bytes_per_sector)
    bs[0:3] = b'\xEB\x3C\x90'
    bs[3:11] = b'BLOSSOM '
    struct.pack_into('<HBHBHHBHHHII', bs, 0x0B,
                     bytes_per_sector, spc, reserved, num_fats, root_entries,
                     total_sectors if total_sectors < 0x10000 else 0,
                     0xF8, fat_sectors, 0x3F, 0xFF, 0,
                     total_sectors if total_sectors >= 0x10000 else 0)
    bs[0x24] = 0x80                     # drive number
    bs[0x26] = 0x29                     # extended boot signature
    struct.pack_into('<I', bs, 0x27, volume_id)
    bs[0x2B:0x36] = label.ljust(11, b' ')[:11]
    bs[0x36:0x3E] = b'FAT16   '
    bs[0x1FE:0x200] = b'\x55\xAA'

    fat = bytearray(fat_sectors * bytes_per_sector)
    fat[0:4] = b'\xF8\xFF\xFF\xFF'

    root = bytearray(root_sectors * bytes_per_sector)
    root[0:11] = label.ljust(11, b' ')[:11]
    root[11] = 0x08                     # volume label entry

    data = bytes(bs) + bytes(fat) * num_fats + bytes(root)
    return data, len(data)


def check_firm(firm: bytes) -> None:
    if firm[0:4] != b'FIRM':
        raise MkNandError('FIRM magic not found: the NATIVE_FIRM image is missing or still encrypted')
    if len(firm) > FIRM_PART_SIZE:
        raise MkNandError(f'FIRM is {len(firm):#x} bytes, larger than the {FIRM_PART_SIZE:#x} partition')


# --------------------------------------------------------------------------------------------
# Steps that need pyctr and your boot9.bin
# --------------------------------------------------------------------------------------------

def make_crypto(boot9_path: str):
    from pyctr.crypto import CryptoEngine
    crypto = CryptoEngine(setup_b9_keys=False)
    crypto.setup_keys_from_boot9_file(boot9_path)
    return crypto


def extract_native_firm(cia_path: str, boot9_path: str) -> bytes:
    """Pull the FIRM image out of a NATIVE_FIRM system title CIA (title key via the common key)."""
    from pyctr.type.cia import CIAReader
    with CIAReader(cia_path, crypto=make_crypto(boot9_path)) as cia:
        tid = cia.tmd.title_id.lower()
        if tid != O3DS_NATIVE_FIRM_TID:
            print(f'warning: {cia_path} is title {tid}, not Old 3DS NATIVE_FIRM ({O3DS_NATIVE_FIRM_TID})',
                  file=sys.stderr)
        ncch = cia.contents[0]
        if ncch.exefs is None:
            raise MkNandError('NATIVE_FIRM content has no ExeFS')
        with ncch.exefs.open('.firm') as f:
            return f.read()


@dataclass
class BuildResult:
    otp_plain: bytes
    otp_enc: bytes
    cid: bytes


def build_nand(out_path: str, boot9_path: str, firm: bytes, seed: str | None) -> BuildResult:
    from pyctr.type.nand import NAND, NANDSection

    check_firm(firm)
    rng = random.Random(seed if seed is not None else os.urandom(32))

    otp_plain = build_otp_plain(rng)
    cid = build_cid(rng)

    crypto = make_crypto(boot9_path)
    crypto.setup_keys_from_otp(otp_plain)
    otp_enc = crypto.otp_enc

    # 1. Raw skeleton: sparse file, plaintext NCSD header, essential.exefs.
    header = build_ncsd_header_body()
    essential = build_exefs([('otp', otp_enc), ('nand_cid', cid)])
    twln_start = TWL_MBR_ENTRIES[0][4] * NAND_MEDIA_UNIT
    if 0x200 + len(essential) > 0x96 * NAND_MEDIA_UNIT or 0x200 + len(essential) > twln_start:
        raise MkNandError('essential.exefs is too big')

    with open(out_path, 'wb') as f:
        f.truncate(O3DS_NAND_SIZE)
        f.seek(0)
        f.write(header)
        f.write(essential)

    # 2. Encrypted parts, through pyctr with the keys derived from our OTP and counters from our CID.
    # The MBRs are still garbage at this point, so pyctr logs errors while opening: silence them.
    pyctr_log = logging.getLogger('pyctr')
    old_level = pyctr_log.level
    pyctr_log.setLevel(logging.CRITICAL)
    try:
        nand = NAND(out_path, mode='rb+', crypto=crypto, cid=cid, auto_raise_exceptions=False)
    finally:
        pyctr_log.setLevel(old_level)
    try:
        with nand.open_raw_section(NANDSection.TWLNAND) as f:
            f.seek(0x1BE)
            f.write(build_mbr_entries(TWL_MBR_ENTRIES))

        with nand.open_raw_section(NANDSection.CTRNAND) as f:
            f.seek(0x1BE)
            f.write(build_mbr_entries(CTR_MBR_ENTRIES))

        for section in (NANDSection.FIRM0, NANDSection.FIRM1):
            with nand.open_raw_section(section) as f:
                f.write(firm)

        volumes = [
            (NANDSection.TWLNAND, TWL_MBR_ENTRIES[0], b'TWL-NAND'),
            (NANDSection.TWLNAND, TWL_MBR_ENTRIES[1], b'TWL-PHOTO'),
            (NANDSection.CTRNAND, CTR_MBR_ENTRIES[0], b'CTR-NAND'),
        ]
        for section, entry, label in volumes:
            lba, count = entry[4], entry[5]
            data, _ = build_fat16_boot_and_tables(count, label, rng.getrandbits(32))
            with nand.open_raw_section(section) as f:
                f.seek(lba * NAND_MEDIA_UNIT)
                f.write(data)
    finally:
        nand.close()

    # 3. Self-sign the final header (0x100..0x200 now holds the encrypted TWL MBR).
    with open(out_path, 'rb+') as f:
        f.seek(0x100)
        signed_part = f.read(0x100)
        f.seek(0)
        f.write(self_sign(signed_part))

    return BuildResult(otp_plain=otp_plain, otp_enc=otp_enc, cid=cid)


def verify_nand(path: str, boot9_path: str, firm: bytes) -> list[str]:
    """
    Reopen the image like a NAND dump (OTP and CID from essential.exefs only) and check every part.
    Returns a list of problems; empty means it looks right.
    """
    from pyctr.type.nand import NAND, NANDSection

    problems = []
    nand = NAND(path, crypto=make_crypto(boot9_path), auto_raise_exceptions=False)
    try:
        if nand.essential is None:
            problems.append('essential.exefs not found at 0x200')
        if nand.twl_partitions != [(e[4] * NAND_MEDIA_UNIT, e[5] * NAND_MEDIA_UNIT) for e in TWL_MBR_ENTRIES] + [(0, 0)] * 2:
            problems.append(f'TWL MBR did not decrypt as expected: {nand.twl_partitions}')
        if nand.ctr_partitions[:1] != [(e[4] * NAND_MEDIA_UNIT, e[5] * NAND_MEDIA_UNIT) for e in CTR_MBR_ENTRIES]:
            problems.append(f'CTR MBR did not decrypt as expected: {nand.ctr_partitions}')
        for section in (NANDSection.FIRM0, NANDSection.FIRM1):
            with nand.open_raw_section(section) as f:
                if f.read(len(firm)) != firm:
                    problems.append(f'{section.name} does not decrypt to the NATIVE_FIRM image')
        for name, opener in (('TWLN', lambda: nand.open_twl_partition(0)),
                             ('TWLP', lambda: nand.open_twl_partition(1)),
                             ('CTRNAND', lambda: nand.open_ctr_partition(0))):
            with opener() as f:
                bs = f.read(0x200)
                if bs[0x36:0x3E] != b'FAT16   ' or bs[0x1FE:0x200] != b'\x55\xAA':
                    problems.append(f'{name} has no FAT16 boot sector')
    finally:
        nand.close()

    with open(path, 'rb') as f:
        hdr = f.read(0x200)
    if hdr[0:0x100] != self_sign(hdr[0x100:0x200]):
        problems.append('NCSD self-signature does not match the header')
    return problems


def explain_hashes(path: str, boot9_path: str) -> list[tuple[str, str]]:
    """
    List the SHA hashes boot9 computes while booting this image, to compare against a SHA engine
    log from Blossom. Every value comes from the image's own (fake) OTP and CID.
    """
    from pyctr.type.exefs import ExeFSReader as Reader

    with open(path, 'rb') as f:
        f.seek(0x200)
        ex = Reader(f, closefd=False)
        with ex.open('otp') as o:
            otp_enc = o.read(0x100)
        with ex.open('nand_cid') as c:
            cid = c.read(0x10)
        f.seek(0)
        hdr = f.read(0x200)
        f.seek(O3DS_PARTITIONS[2][2])
        firm0_raw = f.read(0x100)

    crypto = make_crypto(boot9_path)
    crypto.setup_keys_from_otp(otp_enc)
    otp_dec = crypto.otp_dec
    return [
        ('sha256(otp_dec[0:0xE0])  (OTP check, must equal otp_dec[0xE0:])', hashlib.sha256(otp_dec[:0xE0]).hexdigest()),
        ('otp_dec[0xE0:0x100]      (stored OTP hash)', otp_dec[0xE0:].hex()),
        ('sha256(otp_enc[0:0x90])  (New 3DS key sector key)', hashlib.sha256(otp_enc[:0x90]).hexdigest()),
        ('sha256(otp_dec[0x90:0xAC] + boot9 data)  (console keyX/Y)',
         hashlib.sha256(otp_dec[0x90:0xAC] + crypto.b9_extdata_otp).hexdigest()),
        ('sha256(otp_enc[0:0xE0])  (OTP check, if the OTP was not decrypted)', hashlib.sha256(otp_enc[:0xE0]).hexdigest()),
        ('sha256(nand_cid)         (CTR/FIRM counter)', hashlib.sha256(cid).hexdigest()),
        ('sha1(nand_cid)           (TWL counter)', hashlib.sha1(cid).hexdigest()),
        ('sha256(NCSD 0x100:0x200) (NCSD signature)', hashlib.sha256(hdr[0x100:0x200]).hexdigest()),
        ('FIRM0 first 16 raw bytes (encrypted)', firm0_raw[:16].hex()),
    ]


def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        description='Build a synthetic Old 3DS NAND for Blossom from your own boot9 and NATIVE_FIRM.',
        epilog='The NCSD header is "self-signed": no real key signs it, so start Blossom with '
               '--dev-unsigned. NATIVE_FIRM keeps its real Nintendo signature.')
    p.add_argument('--boot9', required=True, help='your boot9.bin (64 KB or the 32 KB protected half)')
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument('--native-firm-cia', help=f'Old 3DS NATIVE_FIRM CIA ({O3DS_NATIVE_FIRM_TID}.cia)')
    src.add_argument('--firm', help='an already decrypted NATIVE_FIRM .firm image')
    p.add_argument('-o', '--out', default='nand_synth.bin', help='output image (default: nand_synth.bin)')
    p.add_argument('--seed', help='make the fake OTP/CID reproducible from this text')
    p.add_argument('--no-verify', action='store_true', help='skip re-reading the image after building it')
    p.add_argument('--explain', metavar='IMAGE',
                   help='do not build: print the hashes boot9 computes for an existing image, '
                        'to compare with a SHA log from Blossom')
    if argv is None:
        argv = sys.argv[1:]
    if '--explain' in argv:
        # --explain only needs --boot9; the FIRM source is not required.
        e = argparse.ArgumentParser()
        e.add_argument('--boot9', required=True)
        e.add_argument('--explain', required=True)
        eargs, _ = e.parse_known_args(argv)
        for label, value in explain_hashes(eargs.explain, eargs.boot9):
            print(f'{value}  {label}')
        return 0
    args = p.parse_args(argv)

    try:
        if args.firm:
            with open(args.firm, 'rb') as f:
                firm = f.read()
        else:
            firm = extract_native_firm(args.native_firm_cia, args.boot9)

        result = build_nand(args.out, args.boot9, firm, args.seed)
        device_id = struct.unpack_from('<I', result.otp_plain, 4)[0]
        print(f'wrote {args.out} ({O3DS_NAND_SIZE:#x} bytes), fake DeviceId {device_id:#010x}')

        if not args.no_verify:
            problems = verify_nand(args.out, args.boot9, firm)
            if problems:
                for problem in problems:
                    print(f'verify: {problem}', file=sys.stderr)
                return 1
            print('verify: OK (essential.exefs, TWL/CTR MBRs, FIRM0/1, FAT16 volumes, NCSD self-signature)')
    except MkNandError as e:
        print(f'error: {e}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
