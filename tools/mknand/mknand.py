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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

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

# Top-level folders a retail console has on each partition. Process9 mounts these at boot and
# crashed on an empty TWLN root (null pointer right after reading the root directory).
TWLN_DIRS = ['/import', '/progress', '/shared1', '/shared2', '/sys', '/ticket', '/title', '/tmp']
TWLP_DIRS = ['/photo']
CTRNAND_DIRS = ['/data', '/dbs', '/fixdata', '/private', '/ro', '/rw', '/rw/sys', '/ticket', '/title', '/tmp']

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
    """
    Build a 16-byte eMMC CID as the SDMMC response registers hold it (the 120 bits left after the
    CRC is stripped). The top byte is always 0 on real hardware, and boot9 hashes the CID with
    that byte cleared, so a random value there gives a counter boot9 never uses.
    """
    return rng.randbytes(15) + b'\x00'


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
class InstalledTitle:
    title_id: str
    version: int
    size: int
    contents: list[str]
    info: 'object' = None  # titledb.TitleInfo for dbs/title.db


def is_twl_title(title_id: str) -> bool:
    """DSiWare / TWL system titles (0004800x...) live in TWL NAND and only TWL_FIRM uses them."""
    return title_id[3:5] == '48'


def find_cias(paths: list[str]) -> list[str]:
    found = []
    for path in paths:
        if os.path.isdir(path):
            for root, _, files in os.walk(path):
                found += [os.path.join(root, f) for f in files if f.lower().endswith('.cia')]
        else:
            found.append(path)
    return sorted(found)


def roundup(x: int, align: int) -> int:
    return (x + align - 1) // align * align


def _extdata_id(cia, boot9_path: str) -> int:
    """Extdata ID from content 0's extended header (0 if there is none or it can't be decrypted)."""
    try:
        from pyctr.type.ncch import NCCHReader, NCCHSection
        with cia.open_raw_section(0) as src:
            ncch = NCCHReader(src, crypto=make_crypto(boot9_path), closefd=False)
            if not ncch.check_for_extheader():
                return 0
            with ncch.open_raw_section(NCCHSection.ExtendedHeader) as ex:
                ex.seek(0x230)
                return int.from_bytes(ex.read(8), 'little')
    except Exception:
        return 0


# AM's content metadata: /title/<hi>/<lo>/content/cmd/<cmd id>.cmd. Process9 reads it to find the
# installed contents of a NAND title. Layout and CMAC inputs from ihaveamac/custom-install (MIT).
CMD_MISSING_ID = b'\xff\xff\xff\xff'
CMD_MISSING_CMAC = b'MISSING CONTENT!'
CMD_KEYSLOT = 0x30  # pyctr's CMACSDNAND: keyY comes from movable.sed


def build_movable(rng: random.Random) -> bytes:
    """
    A 0x120-byte movable.sed without CMAC: "SEED", a self-signed LocalFriendCodeSeed_B and a random
    keyY. LFCS_B is a 0x100 RSA signature over 0x10 bytes (8 zero bytes + the 8-byte LFCS), and
    the LFCS is also the first half of the keyY. Process9 checks that signature at boot, so it is
    self-signed like the NCSD header and needs --dev-unsigned.
    """
    sed = bytearray(0x120)
    sed[0:4] = b'SEED'
    sed[0x110:0x120] = rng.randbytes(0x10)   # keyY for keyslots 0x30/0x34/0x3A; [0:8] is the LFCS
    sed[0x08:0x108] = self_sign(bytes(sed[0x108:0x118]))
    return bytes(sed)


def build_cmd(contents: list[tuple[int, str, bytes]], crypto, keyslot: int = CMD_KEYSLOT,
              cmd_id: int = 1) -> bytes:
    """
    Build a .cmd file. `contents` holds (content index, content id as hex, the content's NCCH
    header at 0x100..0x200). `crypto` needs `keyslot` ready (movable.sed loaded for 0x30).
    """
    by_index = {}
    for cindex, cid_hex, ncch_head in contents:
        id_bytes = int(cid_hex, 16).to_bytes(4, 'little')
        c = crypto.create_cmac_object(keyslot)
        c.update(hashlib.sha256(ncch_head + cindex.to_bytes(4, 'little') + id_bytes).digest())
        by_index[cindex] = (id_bytes, c.digest())
    count = max(by_index) + 1 if by_index else 0
    ids, cmacs = [], []
    for i in range(count):
        if i in by_index:
            ids.append(by_index[i][0])
            cmacs.append(by_index[i][1])
        else:
            ids.append(CMD_MISSING_ID)
            cmacs.append(CMD_MISSING_CMAC)
    installed = sorted((v[0] for v in by_index.values()), key=lambda b: int.from_bytes(b, 'little'))
    header = struct.pack('<IIII', cmd_id, count, len(installed), 1)
    c = crypto.create_cmac_object(keyslot)
    c.update(header)
    return header + c.digest() + b''.join(ids) + b''.join(installed) + b''.join(cmacs)


def _cmd_inputs(cia) -> list[tuple[int, str, bytes]]:
    out = []
    for record in cia.content_info:
        with cia.open_raw_section(record.cindex) as src:
            src.seek(0x100)
            out.append((record.cindex, record.id, src.read(0x100)))
    return out


def install_titles(volume, cia_paths: list[str], boot9_path: str, log=print,
                   write_files: bool = True, cmd_crypto=None,
                   cmd_keyslot: int = CMD_KEYSLOT) -> list[InstalledTitle]:
    """
    Install CTR titles from CIAs into a mounted CTRNAND file system (a pyfs-style object).

    Each title goes to /title/<high>/<low>/content/ as 00000000.tmd plus one <content id>.app per
    content in the CIA, with the title-key layer removed (NCCH encryption stays, as on a console),
    and content/cmd/00000001.cmd when `cmd_crypto` (with movable.sed loaded) is given.
    Returns what dbs/title.db needs for each title. With write_files=False (volume may be None)
    it only computes that.
    """
    from pyctr.type.cia import CIAReader, CIASection
    from titledb import TitleInfo

    installed = []
    for path in cia_paths:
        with CIAReader(path, crypto=make_crypto(boot9_path), load_contents=False) as cia:
            tid = cia.tmd.title_id
            if is_twl_title(tid):
                log(f'skip {tid}: TWL title (TWL NAND is not filled yet)')
                continue
            base = f'/title/{tid[0:8]}/{tid[8:16]}/content'
            if write_files:
                volume.makedirs(base, recreate=True)
                with cia.open_raw_section(CIASection.TitleMetadata) as src, \
                        volume.openbin(f'{base}/00000000.tmd', 'w') as dst:
                    dst.write(src.read())
            total = 0
            names = []
            content0_head = b''
            for record in cia.content_info:
                if record.cindex == 0:
                    with cia.open_raw_section(0) as src:
                        content0_head = src.read(0x200)
                name = f'{record.id.lower()}.app'
                if write_files:
                    with cia.open_raw_section(record.cindex) as src, volume.openbin(f'{base}/{name}', 'w') as dst:
                        while chunk := src.read(0x100000):
                            dst.write(chunk)
                total += record.size
                names.append(name)
            if write_files and cmd_crypto is not None:
                volume.makedirs(f'{base}/cmd', recreate=True)
                with volume.openbin(f'{base}/cmd/00000001.cmd', 'w') as dst:
                    dst.write(build_cmd(_cmd_inputs(cia), cmd_crypto, cmd_keyslot))
            tmd = cia.tmd
            on_disk = sum(roundup(r.size, 0x8000) for r in cia.content_info)
            with cia.open_raw_section(CIASection.TitleMetadata) as src:
                on_disk += roundup(len(src.read()), 0x8000)
            info = TitleInfo(
                title_id=int(tid, 16),
                size=on_disk,
                title_type=int.from_bytes(getattr(tmd, '_u_title_type', b'\0\0\0\x40'), 'big') or 0x40,
                version=int(tmd.title_version),
                tmd_id=0,
                has_save=bool(tmd.save_size),
                has_manual=any(r.cindex == 1 for r in cia.content_info),
                extdata_id=_extdata_id(cia, boot9_path),
                product_code=content0_head[0x150:0x160].rstrip(b'\0') if content0_head[0x100:0x104] == b'NCCH' else b'',
                content0_id=next((int(r.id, 16) for r in cia.content_info if r.cindex == 0), 0xFFFFFFFF),
                random=random.getrandbits(32),
            )
            installed.append(InstalledTitle(tid, int(tmd.title_version), total, names, info))
    return installed


def write_title_databases(volume, infos, crypto) -> None:
    """dbs/title.db with one entry per installed title, and an empty dbs/import.db."""
    import titledb
    with volume.openbin('/dbs/title.db', 'w') as f:
        f.write(titledb.build_database(titledb.DB_TITLE, infos, crypto))
    with volume.openbin('/dbs/import.db', 'w') as f:
        f.write(titledb.build_database(titledb.DB_IMPORT, [], crypto))


def allocated_ranges(volume_head: bytes) -> list[tuple[int, int]]:
    """
    Byte ranges (offset, length) of a FAT16 volume that hold data: the reserved sector, FATs and
    root directory, plus every allocated cluster. `volume_head` must cover at least the FAT area.
    """
    bps, spc, reserved, nfats, root_entries = struct.unpack_from('<HBHBH', volume_head, 0x0B)
    fat_sectors = struct.unpack_from('<H', volume_head, 0x16)[0]
    meta_len = (reserved + nfats * fat_sectors) * bps + root_entries * 32
    cluster = spc * bps
    fat = volume_head[reserved * bps:(reserved + fat_sectors) * bps]
    ranges = [(0, meta_len)]
    for n in range(2, len(fat) // 2):
        if struct.unpack_from('<H', fat, n * 2)[0]:
            off = meta_len + (n - 2) * cluster
            if ranges[-1][0] + ranges[-1][1] == off:
                ranges[-1] = (ranges[-1][0], ranges[-1][1] + cluster)
            else:
                ranges.append((off, cluster))
    return ranges


DB_FILES = {'title.db': 2, 'import.db': 3, 'ticket.db': 0, 'certs.db': 1, 'tmp_t.db': 4, 'tmp_i.db': 5}


class MountedCTRNAND:
    """Decrypt an image's CTRNAND to a temporary file and mount it; `save()` encrypts it back."""

    def __init__(self, image: str, boot9_path: str, writable: bool = False):
        from pyctr.type.nand import NAND
        from pyfatfs.PyFatFS import PyFatFS
        self.image, self.writable = image, writable
        self.crypto = make_crypto(boot9_path)
        self.nand = NAND(image, mode='rb+' if writable else 'rb', crypto=self.crypto)
        self.tmp = image + '.ctrnand.tmp'
        with self.nand.open_ctr_partition(0) as src, open(self.tmp, 'wb') as dst:
            while chunk := src.read(0x400000):
                dst.write(chunk)
        self.fs = PyFatFS(self.tmp)

    def find(self, folder: str, name: str) -> str | None:
        for entry in self.fs.listdir(folder):
            if entry.lower() == name.lower():
                return f'{folder}/{entry}'
        return None

    def save(self) -> None:
        self.fs.close()
        self.fs = None
        with open(self.tmp, 'rb') as f:
            head = f.read(0x100000)
        meta_len = allocated_ranges(head)[0][1]
        with self.nand.open_ctr_partition(0) as dst:
            write_volume_encrypted(self.tmp, dst, meta_len)

    def close(self) -> None:
        if self.fs is not None:
            self.fs.close()
        self.nand.close()
        if os.path.exists(self.tmp):
            os.remove(self.tmp)


def inspect_dbs(image: str, boot9_path: str) -> list[str]:
    """Describe every database in an image's CTRNAND /dbs (geometry, CMAC check with our keys)."""
    import titledb
    lines = []
    m = MountedCTRNAND(image, boot9_path)
    try:
        for entry in sorted(m.fs.listdir('/dbs')):
            raw = m.fs.readbytes(f'/dbs/{entry}')
            name = entry.lower()
            line = f'{entry}: {len(raw):#x} bytes'
            if raw[0x100:0x104] == b'DIFF' and name in DB_FILES:
                line += ', CMAC ' + ('OK' if titledb.check_cmac(raw, DB_FILES[name], m.crypto) else 'MISMATCH')
                try:
                    lv4 = titledb.read_diff(raw)
                    line += f', magic {lv4[0:8]!r}'
                    if lv4[0x80:0x84] == b'BDRI':
                        fs = titledb.BDRI.from_image(lv4)
                        g = fs.g
                        line += (f', BDRI block {g.block_size:#x}, data blocks {g.data_blocks:#x}, '
                                 f'max files {g.max_files:#x}, buckets {g.file_hash_buckets:#x}, '
                                 f'entries {len(fs.titles)}')
                except Exception as e:  # noqa: BLE001 - this is a diagnostic
                    line += f', could not parse: {e}'
            lines.append(line)
    finally:
        m.close()
    return lines


def update_dbs(image: str, boot9_path: str, cia_paths: list[str], log=print) -> int:
    """
    Add the titles from the CIAs to the title.db that AM created in the image on first boot,
    keeping AM's geometry, and re-sign it with the image's keys. Returns how many were added.
    """
    import titledb
    infos = [t.info for t in install_titles(None, cia_paths, boot9_path, log, write_files=False)]
    m = MountedCTRNAND(image, boot9_path, writable=True)
    try:
        path = m.find('/dbs', 'title.db')
        if path is None:
            raise MkNandError('this image has no dbs/title.db yet: boot it once in Blossom first')
        raw = m.fs.readbytes(path)
        before = len(titledb.BDRI.from_image(titledb.read_diff(raw)).titles)
        new = titledb.add_titles_to_existing(raw, titledb.DB_TITLE, infos, m.crypto)
        with m.fs.openbin(path, 'r+') as f:
            f.write(new)
        after = len(titledb.BDRI.from_image(titledb.read_diff(new)).titles)
        m.save()
    finally:
        m.close()
    return after - before


def fix_titles(image: str, boot9_path: str, cia_paths: list[str], log=print,
               keyslot: int = CMD_KEYSLOT, movable: str = 'keep') -> int:
    """
    Write content/cmd/00000001.cmd for every installed title in an existing image, and
    /private/movable.sed if there is none (its keyY is the CMAC key). Returns how many cmds.
    movable: 'keep' uses the image's movable.sed if it has one; 'new' always writes a fresh
    self-signed one; 'none' removes it and signs the cmds with an all-zero keyY (for testing).
    """
    from pyctr.type.cia import CIAReader
    m = MountedCTRNAND(image, boot9_path, writable=True)
    written = 0
    try:
        m.fs.makedirs('/private', recreate=True)
        path = m.find('/private', 'movable.sed')
        if movable == 'none':
            if path is not None:
                m.fs.remove(path)
                log(f'removed {path}')
            sed = bytes(0x10)
        elif path is None or movable == 'new':
            if path is not None:
                m.fs.remove(path)
            sed = build_movable(random.Random(os.urandom(32)))
            with m.fs.openbin('/private/movable.sed', 'w') as f:
                f.write(sed)
            log('created /private/movable.sed')
        else:
            sed = m.fs.readbytes(path)
            log(f'using {path} ({len(sed):#x} bytes)')
        m.crypto.setup_sd_key(sed)
        for cia_path in cia_paths:
            with CIAReader(cia_path, crypto=make_crypto(boot9_path), load_contents=False) as cia:
                tid = cia.tmd.title_id
                base = f'/title/{tid[0:8]}/{tid[8:16]}/content'
                if is_twl_title(tid) or not m.fs.isdir(base):
                    continue
                m.fs.makedirs(f'{base}/cmd', recreate=True)
                with m.fs.openbin(f'{base}/cmd/00000001.cmd', 'w') as f:
                    f.write(build_cmd(_cmd_inputs(cia), m.crypto, keyslot))
                written += 1
        m.save()
    finally:
        m.close()
    return written


def write_volume_encrypted(src_path: str, dst, meta_len: int, chunk: int = 0x400000) -> None:
    """
    Copy a plaintext FAT16 volume into an encrypted partition file object: the FAT area and every
    allocated cluster. Free clusters are left as they are (they can hold anything).
    """
    with open(src_path, 'rb') as src:
        head = src.read(meta_len)
        for off, length in allocated_ranges(head):
            src.seek(off)
            dst.seek(off)
            while length > 0:
                data = src.read(min(chunk, length))
                if not data:
                    break
                dst.write(data)
                length -= len(data)


def write_fat_volume(nand, section, entry, label: bytes, volume_id: int, fill, out_path: str) -> None:
    """Format a plaintext FAT16 copy of one partition, let `fill` populate it, encrypt it into place."""
    from pyctr.fileio import SubsectionIO
    from pyfatfs.PyFatFS import PyFatFS

    lba, count = entry[4], entry[5]
    data, meta_len = build_fat16_boot_and_tables(count, label, volume_id)
    tmp_path = out_path + '.volume.tmp'
    try:
        with open(tmp_path, 'wb') as f:
            f.truncate(count * NAND_MEDIA_UNIT)
            f.write(data)
        volume = PyFatFS(tmp_path)
        try:
            fill(volume)
        finally:
            volume.close()
        with nand.open_raw_section(section) as part:
            dst = SubsectionIO(part, lba * NAND_MEDIA_UNIT, count * NAND_MEDIA_UNIT)
            write_volume_encrypted(tmp_path, dst, meta_len)
            dst.close()
    finally:
        if os.path.exists(tmp_path):
            os.remove(tmp_path)


@dataclass
class BuildResult:
    otp_plain: bytes
    otp_enc: bytes
    cid: bytes
    titles: list[InstalledTitle]


def build_nand(out_path: str, boot9_path: str, firm: bytes, seed: str | None,
               cia_paths: list[str] | None = None, log=print,
               cmd_keyslot: int = CMD_KEYSLOT) -> BuildResult:
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

        titles = []

        def fill_twln(volume):
            for d in TWLN_DIRS:
                volume.makedirs(d, recreate=True)

        def fill_twlp(volume):
            for d in TWLP_DIRS:
                volume.makedirs(d, recreate=True)

        def fill_ctrnand(volume):
            nonlocal titles
            for d in CTRNAND_DIRS:
                volume.makedirs(d, recreate=True)
            movable = build_movable(rng)
            with volume.openbin('/private/movable.sed', 'w') as f:
                f.write(movable)
            crypto.setup_sd_key(movable)
            if cia_paths:
                titles = install_titles(volume, cia_paths, boot9_path, log, cmd_crypto=crypto,
                                        cmd_keyslot=cmd_keyslot)
            write_title_databases(volume, [t.info for t in titles], crypto)

        volumes = [
            (NANDSection.TWLNAND, TWL_MBR_ENTRIES[0], b'TWL-NAND', fill_twln),
            (NANDSection.TWLNAND, TWL_MBR_ENTRIES[1], b'TWL-PHOTO', fill_twlp),
            (NANDSection.CTRNAND, CTR_MBR_ENTRIES[0], b'CTR-NAND', fill_ctrnand),
        ]
        for section, entry, label, fill in volumes:
            write_fat_volume(nand, section, entry, label, rng.getrandbits(32), fill, out_path)
    finally:
        nand.close()

    # 3. Self-sign the final header (0x100..0x200 now holds the encrypted TWL MBR).
    with open(out_path, 'rb+') as f:
        f.seek(0x100)
        signed_part = f.read(0x100)
        f.seek(0)
        f.write(self_sign(signed_part))

    return BuildResult(otp_plain=otp_plain, otp_enc=otp_enc, cid=cid, titles=titles)


def verify_title_db(raw: bytes, titles: list[InstalledTitle]) -> list[str]:
    """Read dbs/title.db back through pyctr's DIFF reader (checks every IVFC hash) and look up each title."""
    import io
    import titledb
    from pyctr.crypto import CryptoEngine
    from pyctr.type.save.diff import DIFF
    from pyctr.type.save.partdesc.ivfc import IVFCLevel4Reader

    problems = []
    diff = DIFF(io.BytesIO(raw), crypto=CryptoEngine(setup_b9_keys=False))
    lv4 = IVFCLevel4Reader(diff.partitions[0].ivfc_hash_tree, verify=True).read()
    if b'\xDD' * 0x200 in lv4:
        problems.append('title.db: IVFC hash check failed')
    fs = titledb.BDRI(b'')
    fs.img = bytearray(lv4)
    if lv4[0:8] != titledb.DB_TITLE[1]:
        problems.append('title.db: wrong magic')
    for t in titles:
        entry = fs.lookup(int(t.title_id, 16))
        if entry is None:
            problems.append(f'title.db: {t.title_id} not found')
        elif entry != t.info.to_bytes():
            problems.append(f'title.db: {t.title_id} entry differs')
    if len(fs.read_titles()) != len(titles):
        problems.append('title.db: root directory lists the wrong number of titles')
    return problems


def verify_titles(nand, path: str, titles: list[InstalledTitle]) -> list[str]:
    """Decrypt CTRNAND to a temporary file, mount it and check every installed file is there."""
    from pyfatfs.PyFatFS import PyFatFS

    problems = []
    tmp_path = path + '.verify.tmp'
    try:
        with nand.open_ctr_partition(0) as src, open(tmp_path, 'wb') as dst:
            while chunk := src.read(0x400000):
                dst.write(chunk)
        volume = PyFatFS(tmp_path)
        try:
            for t in titles:
                base = f'/title/{t.title_id[0:8]}/{t.title_id[8:16]}/content'
                for name in ['00000000.tmd', 'cmd/00000001.cmd'] + t.contents:
                    if not volume.exists(f'{base}/{name}'):
                        problems.append(f'missing {base}/{name}')
                size = sum(volume.getsize(f'{base}/{n}') for n in t.contents if volume.exists(f'{base}/{n}'))
                if size != t.size:
                    problems.append(f'{t.title_id}: contents are {size} bytes, expected {t.size}')
            problems += verify_title_db(volume.readbytes('/dbs/title.db'), titles)
        finally:
            volume.close()
    finally:
        if os.path.exists(tmp_path):
            os.remove(tmp_path)
    return problems


def verify_nand(path: str, boot9_path: str, firm: bytes, titles: list[InstalledTitle] | None = None) -> list[str]:
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
        if titles:
            problems += verify_titles(nand, path, titles)
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
    p.add_argument('--cias', nargs='+', metavar='PATH',
                   help='system title CIAs (files or folders) to install into CTRNAND; '
                        'TWL titles are skipped for now')
    p.add_argument('--seed', help='make the fake OTP/CID reproducible from this text')
    p.add_argument('--no-verify', action='store_true', help='skip re-reading the image after building it')
    p.add_argument('--inspect-dbs', metavar='IMAGE',
                   help='do not build: list the databases in an image\'s CTRNAND and check their CMACs')
    p.add_argument('--update-dbs', metavar='IMAGE',
                   help='do not build: add the --cias titles to the title.db AM created on first boot')
    p.add_argument('--fix-titles', metavar='IMAGE',
                   help='do not build: write content/cmd/00000001.cmd for the --cias titles already '
                        'in an image (and /private/movable.sed if missing)')
    p.add_argument('--movable', choices=['keep', 'new', 'none'], default='keep',
                   help='with --fix-titles: keep the image\'s movable.sed (default), write a new '
                        'self-signed one, or remove it')
    p.add_argument('--cmd-keyslot', default='0x30',
                   help='AES-CMAC keyslot for .cmd files (default 0x30; 0x0B to try the NAND DB key)')
    p.add_argument('--explain', metavar='IMAGE',
                   help='do not build: print the hashes boot9 computes for an existing image, '
                        'to compare with a SHA log from Blossom')
    if argv is None:
        argv = sys.argv[1:]
    if '--inspect-dbs' in argv or '--update-dbs' in argv or '--fix-titles' in argv:
        e = argparse.ArgumentParser()
        e.add_argument('--boot9', required=True)
        e.add_argument('--inspect-dbs')
        e.add_argument('--update-dbs')
        e.add_argument('--fix-titles')
        e.add_argument('--cmd-keyslot', default='0x30')
        e.add_argument('--movable', choices=['keep', 'new', 'none'], default='keep')
        e.add_argument('--cias', nargs='+')
        eargs, _ = e.parse_known_args(argv)
        try:
            if eargs.fix_titles:
                if not eargs.cias:
                    print('error: --fix-titles needs --cias', file=sys.stderr)
                    return 1
                n = fix_titles(eargs.fix_titles, eargs.boot9, find_cias(eargs.cias),
                               keyslot=int(eargs.cmd_keyslot, 0), movable=eargs.movable)
                print(f'wrote {n} cmd files')
                if not (eargs.update_dbs or eargs.inspect_dbs):
                    return 0
            if eargs.update_dbs:
                if not eargs.cias:
                    print('error: --update-dbs needs --cias', file=sys.stderr)
                    return 1
                added = update_dbs(eargs.update_dbs, eargs.boot9, find_cias(eargs.cias))
                print(f'added {added} titles to dbs/title.db')
            for line in inspect_dbs(eargs.inspect_dbs or eargs.update_dbs, eargs.boot9):
                print(line)
        except MkNandError as err:
            print(f'error: {err}', file=sys.stderr)
            return 1
        return 0
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

        cias = find_cias(args.cias) if args.cias else []
        result = build_nand(args.out, args.boot9, firm, args.seed, cias,
                            cmd_keyslot=int(args.cmd_keyslot, 0))
        if cias:
            print(f'installed {len(result.titles)} titles into CTRNAND')
        device_id = struct.unpack_from('<I', result.otp_plain, 4)[0]
        print(f'wrote {args.out} ({O3DS_NAND_SIZE:#x} bytes), fake DeviceId {device_id:#010x}')

        if not args.no_verify:
            problems = verify_nand(args.out, args.boot9, firm, result.titles)
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
