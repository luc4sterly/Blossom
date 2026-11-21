# Blossom - CTRNAND title database writer (dbs/title.db, dbs/import.db)
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Writes a NAND title database from scratch: a DIFF container (IVFC hash tree over a flattened
# DPFS tree) holding a BDRI file system whose files are 0x80-byte title info entries, named by
# title ID. Format from 3dbrew (Title Database, Inner FAT, DISA and DIFF). The geometry below is
# the one AM uses for an empty title database: room for 0x2000 titles.

from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass

# --- DIFF container geometry -----------------------------------------------------------------

DIFF_FILE_SIZE = 0x31E400
DIFF_SECONDARY_PARTDESC = 0x200
DIFF_PRIMARY_PARTDESC = 0x330
DIFF_PARTDESC_SIZE = 0x12C
DIFF_PARTITION_OFFSET = 0x600
DIFF_PARTITION_SIZE = 0x31DE00

# (offset, size, log2 block size). DPFS offsets are relative to the partition, IVFC offsets to
# DPFS level 3.
DPFS_LEVELS = [(0, 4, 0), (8, 0x200, 7), (0x600, 0x18EC00, 9)]
IVFC_LEVELS = [(0, 0x180, 9), (0x200, 0x1760, 9), (0x1A00, 0x175C0, 9), (0x19000, 0x175A80, 9)]

DIFI_SIZE, IVFC_DESC_SIZE, DPFS_DESC_SIZE = 0x44, 0x78, 0x50

# --- BDRI geometry -------------------------------------------------------------------------

PREHEADER_SIZE = 0x80
BLOCK_SIZE = 0x80
FS_INFO_OFFSET = 0x20
DIR_HASH_OFFSET = 0x88
DIR_HASH_BUCKETS = 1
FILE_HASH_OFFSET = 0x8C
FILE_HASH_BUCKETS = 0x2009
FAT_OFFSET = 0x80B0
DATA_BLOCKS = 0x2B03
DATA_OFFSET = 0x1D900
DIR_TABLE_BLOCK, DIR_TABLE_BLOCKS, MAX_DIRS = 0, 1, 1
FILE_TABLE_BLOCK, FILE_TABLE_BLOCKS, MAX_FILES = 1, 0xB01, 0x2000
DIR_ENTRY_SIZE = 0x20
FILE_ENTRY_SIZE = 0x2C
TITLE_ENTRY_SIZE = 0x80

ROOT_DIR = 1

# CTR-9DB0 database IDs and pre-header magics
DB_TITLE = (2, b'NANDTDB\0')
DB_IMPORT = (3, b'NANDIDB\0')


class TitleDBError(Exception):
    pass


@dataclass
class TitleInfo:
    title_id: int
    size: int
    title_type: int = 0x40
    version: int = 0
    tmd_id: int = 0
    cmd_id: int = 1
    has_save: bool = False
    has_manual: bool = False
    extdata_id: int = 0
    product_code: bytes = b''
    content0_id: int = 0xFFFFFFFF
    random: int = 0

    def to_bytes(self) -> bytes:
        e = bytearray(TITLE_ENTRY_SIZE)
        struct.pack_into('<QIH', e, 0x00, self.size, self.title_type, self.version)
        struct.pack_into('<H', e, 0x10, 1 if self.has_manual else 0)  # content index of the manual
        struct.pack_into('<III', e, 0x14, self.tmd_id, self.cmd_id, 0)
        struct.pack_into('<Q', e, 0x20, self.extdata_id)
        e[0x2C] = 1                                                   # CMD format version
        e[0x30:0x40] = self.product_code[:0x10].ljust(0x10, b'\0')
        struct.pack_into('<II', e, 0x4C, self.content0_id, self.random)
        return bytes(e)


def file_hash(parent: int, title_id: int) -> int:
    """Bucket hash of the inner FAT: parent index and the 8-byte name (title ID, little-endian)."""
    h = parent ^ 0x091A2B3C
    name = title_id.to_bytes(8, 'little')
    for i in range(2):
        h = ((h >> 1) | (h << 31)) & 0xFFFFFFFF
        h ^= struct.unpack_from('<I', name, i * 4)[0]
    return h


class BDRI:
    """An empty title database file system, with title info entries added one by one."""

    def __init__(self, magic: bytes):
        size = IVFC_LEVELS[3][1]
        self.img = bytearray(size)
        self.img[0:8] = magic
        b = PREHEADER_SIZE
        struct.pack_into('<4sIQQI', self.img, b, b'BDRI', 0x30000, FS_INFO_OFFSET, size // BLOCK_SIZE, BLOCK_SIZE)
        struct.pack_into('<IIQIIQIIQIIQIIIIIIIIII', self.img, b + FS_INFO_OFFSET,
                         0, BLOCK_SIZE,
                         DIR_HASH_OFFSET, DIR_HASH_BUCKETS, 0,
                         FILE_HASH_OFFSET, FILE_HASH_BUCKETS, 0,
                         FAT_OFFSET, DATA_BLOCKS, 0,
                         DATA_OFFSET, DATA_BLOCKS, 0,
                         DIR_TABLE_BLOCK, DIR_TABLE_BLOCKS, MAX_DIRS, 0,
                         FILE_TABLE_BLOCK, FILE_TABLE_BLOCKS, MAX_FILES, 0)

        # Directory table: dummy head (2 entries in use, 3 slots) and an empty root.
        self._dir_entry(0)[0:8] = struct.pack('<II', 2, MAX_DIRS + 2)
        struct.pack_into('<I', self.img, b + DIR_HASH_OFFSET, ROOT_DIR)

        # File table: every slot is on the free (dummy) list, head -> 0x2000 -> ... -> 1 -> end.
        for k in range(MAX_FILES + 1):
            e = self._file_entry(k)
            e[0:8] = struct.pack('<II', MAX_FILES + 1, MAX_FILES + 1)
            struct.pack_into('<I', e, 0x28, MAX_FILES if k == 0 else k - 1)

        # FAT: the two tables are allocated nodes, all other blocks form one free chain of
        # single-block nodes (FAT entry = block + 1).
        self._fat_set(0, 0, 0)
        self._alloc_node(DIR_TABLE_BLOCK + 1, DIR_TABLE_BLOCKS)
        self._alloc_node(FILE_TABLE_BLOCK + 1, FILE_TABLE_BLOCKS)
        first_free = FILE_TABLE_BLOCK + FILE_TABLE_BLOCKS + 1
        # The last data block runs 0x80 bytes past the end of the image (a quirk of AM's layout),
        # so it is never handed out.
        free = list(range(first_free, DATA_BLOCKS))
        for i, k in enumerate(free):
            prev = free[i - 1] if i else 0
            nxt = free[i + 1] if i + 1 < len(free) else 0
            self._fat_set(k, prev | (0x80000000 if i == 0 else 0), nxt)
        self._fat_set(0, 0, free[0])
        self.titles: list[int] = []

    # -- raw accessors --

    def _view(self, off: int, size: int) -> memoryview:
        return memoryview(self.img)[off:off + size]

    def _dir_entry(self, k: int) -> memoryview:
        return self._view(PREHEADER_SIZE + DATA_OFFSET + DIR_TABLE_BLOCK * BLOCK_SIZE + k * DIR_ENTRY_SIZE,
                          DIR_ENTRY_SIZE)

    def _file_entry(self, k: int) -> memoryview:
        return self._view(PREHEADER_SIZE + DATA_OFFSET + FILE_TABLE_BLOCK * BLOCK_SIZE + k * FILE_ENTRY_SIZE,
                          FILE_ENTRY_SIZE)

    def _fat(self, k: int) -> tuple[int, int]:
        return struct.unpack_from('<II', self.img, PREHEADER_SIZE + FAT_OFFSET + k * 8)

    def _fat_set(self, k: int, u: int, v: int) -> None:
        struct.pack_into('<II', self.img, PREHEADER_SIZE + FAT_OFFSET + k * 8, u, v)

    def _alloc_node(self, k: int, n: int) -> None:
        """Mark FAT entries k..k+n-1 as a single-node chain."""
        if n == 1:
            self._fat_set(k, 0x80000000, 0)
        else:
            self._fat_set(k, 0x80000000, 0x80000000)
            self._fat_set(k + 1, 0x80000000 | k, k + n - 1)
            self._fat_set(k + n - 1, 0x80000000 | k, k + n - 1)

    def _pop_free_block(self) -> int:
        _, head = self._fat(0)
        if head == 0:
            raise TitleDBError('title database is full')
        _, nxt = self._fat(head)
        self._fat_set(0, 0, nxt)
        if nxt:
            _, nxt_v = self._fat(nxt)
            self._fat_set(nxt, 0x80000000, nxt_v)
        self._alloc_node(head, 1)
        return head - 1

    def _pop_free_file_entry(self) -> int:
        head = struct.unpack_from('<I', self._file_entry(0), 0x28)[0]
        if head == 0:
            raise TitleDBError('no free file entries')
        nxt = struct.unpack_from('<I', self._file_entry(head), 0x28)[0]
        struct.pack_into('<I', self._file_entry(0), 0x28, nxt)
        return head

    # -- public --

    def add(self, info: TitleInfo) -> None:
        if info.title_id in self.titles:
            raise TitleDBError(f'duplicate title {info.title_id:016x}')
        block = self._pop_free_block()
        idx = self._pop_free_file_entry()

        data_off = PREHEADER_SIZE + DATA_OFFSET + block * BLOCK_SIZE
        self.img[data_off:data_off + TITLE_ENTRY_SIZE] = info.to_bytes()

        root = self._dir_entry(ROOT_DIR)
        first_file = struct.unpack_from('<I', root, 0x0C)[0]
        bucket_off = PREHEADER_SIZE + FILE_HASH_OFFSET + (file_hash(ROOT_DIR, info.title_id) % FILE_HASH_BUCKETS) * 4
        bucket_head = struct.unpack_from('<I', self.img, bucket_off)[0]

        e = self._file_entry(idx)
        e[:] = bytes(FILE_ENTRY_SIZE)
        struct.pack_into('<IQI', e, 0x00, ROOT_DIR, info.title_id, first_file)
        struct.pack_into('<IQ', e, 0x14, block, TITLE_ENTRY_SIZE)
        struct.pack_into('<I', e, 0x28, bucket_head)

        struct.pack_into('<I', root, 0x0C, idx)
        struct.pack_into('<I', self.img, bucket_off, idx)
        self.titles.append(info.title_id)

    def read_titles(self) -> dict[int, bytes]:
        """Walk the root directory the way the console does and return {title ID: entry}."""
        out = {}
        idx = struct.unpack_from('<I', self._dir_entry(ROOT_DIR), 0x0C)[0]
        seen = 0
        while idx:
            e = self._file_entry(idx)
            parent, tid, nxt = struct.unpack_from('<IQI', e, 0)
            block, size = struct.unpack_from('<IQ', e, 0x14)
            off = PREHEADER_SIZE + DATA_OFFSET + block * BLOCK_SIZE
            out[tid] = bytes(self.img[off:off + size])
            idx = nxt
            seen += 1
            if seen > MAX_FILES:
                raise TitleDBError('file list loops')
        return out

    def lookup(self, title_id: int) -> bytes | None:
        """Find a title through the hash table, as Process9 does."""
        bucket_off = PREHEADER_SIZE + FILE_HASH_OFFSET + (file_hash(ROOT_DIR, title_id) % FILE_HASH_BUCKETS) * 4
        idx = struct.unpack_from('<I', self.img, bucket_off)[0]
        while idx:
            e = self._file_entry(idx)
            parent, tid = struct.unpack_from('<IQ', e, 0)
            if parent == ROOT_DIR and tid == title_id:
                block, size = struct.unpack_from('<IQ', e, 0x14)
                off = PREHEADER_SIZE + DATA_OFFSET + block * BLOCK_SIZE
                return bytes(self.img[off:off + size])
            idx = struct.unpack_from('<I', e, 0x28)[0]
        return None


# --- DIFF container ------------------------------------------------------------------------

def _hash_level(data: bytes, block_size: int) -> bytes:
    out = bytearray()
    for i in range(0, len(data), block_size):
        out += hashlib.sha256(data[i:i + block_size].ljust(block_size, b'\0')).digest()
    return bytes(out)


def _partdesc(master_hash: bytes) -> bytes:
    d = bytearray(DIFF_PARTDESC_SIZE)
    ivfc_off, dpfs_off, hash_off = DIFI_SIZE, DIFI_SIZE + IVFC_DESC_SIZE, DIFI_SIZE + IVFC_DESC_SIZE + DPFS_DESC_SIZE
    struct.pack_into('<8sQQQQQQBBHQ', d, 0, b'DIFI\0\0\1\0', ivfc_off, IVFC_DESC_SIZE, dpfs_off, DPFS_DESC_SIZE,
                     hash_off, 0x20, 0, 0, 0, 0)
    struct.pack_into('<8sQ', d, ivfc_off, b'IVFC\0\0\2\0', 0x20)
    for i, (off, size, log2) in enumerate(IVFC_LEVELS):
        struct.pack_into('<QQI', d, ivfc_off + 0x10 + i * 0x18, off, size, log2)
    struct.pack_into('<Q', d, ivfc_off + 0x70, IVFC_DESC_SIZE)
    struct.pack_into('<8s', d, dpfs_off, b'DPFS\0\0\1\0')
    for i, (off, size, log2) in enumerate(DPFS_LEVELS):
        struct.pack_into('<QQI', d, dpfs_off + 0x08 + i * 0x18, off, size, log2)
    d[hash_off:hash_off + 0x20] = master_hash
    return bytes(d)


def build_diff(lv4: bytes) -> bytes:
    """Wrap a BDRI image in a DIFF container. The CMAC (first 0x10 bytes) is left zero."""
    if len(lv4) != IVFC_LEVELS[3][1]:
        raise TitleDBError('wrong BDRI image size')
    lv3_size = DPFS_LEVELS[2][1]
    l3 = bytearray(lv3_size)
    levels = [bytes(lv4)]
    for _, size, log2 in reversed(IVFC_LEVELS[:3]):
        h = _hash_level(levels[-1], 1 << log2)
        if len(h) != size:
            raise TitleDBError(f'IVFC level size mismatch: {len(h):#x} != {size:#x}')
        levels.append(h)
    # levels = [lv4, lv3, lv2, lv1]
    for (off, size, _), data in zip(IVFC_LEVELS, reversed(levels)):
        l3[off:off + size] = data
    master = hashlib.sha256(levels[-1].ljust(1 << IVFC_LEVELS[0][2], b'\0')).digest()

    out = bytearray(DIFF_FILE_SIZE)
    part = DIFF_PARTITION_OFFSET
    # DPFS levels 1 and 2 stay all-zero: every pair selects its first copy, so level 3 copy 0 is live.
    out[part + DPFS_LEVELS[2][0]:part + DPFS_LEVELS[2][0] + lv3_size] = l3
    pd = _partdesc(master)
    out[DIFF_SECONDARY_PARTDESC:DIFF_SECONDARY_PARTDESC + DIFF_PARTDESC_SIZE] = pd
    out[DIFF_PRIMARY_PARTDESC:DIFF_PRIMARY_PARTDESC + DIFF_PARTDESC_SIZE] = pd
    struct.pack_into('<8sQQQQQI', out, 0x100, b'DIFF\0\0\3\0', DIFF_SECONDARY_PARTDESC, DIFF_PRIMARY_PARTDESC,
                     DIFF_PARTDESC_SIZE, DIFF_PARTITION_OFFSET, DIFF_PARTITION_SIZE, 0)  # 0 = primary active
    out[0x134:0x154] = hashlib.sha256(pd).digest()
    return bytes(out)


def sign_diff(diff: bytes, db_id: int, crypto) -> bytes:
    """Fill in the CTR-9DB0 AES-CMAC (NAND keyslot 0x0B) over the DIFF header."""
    from pyctr.crypto import Keyslot
    digest = hashlib.sha256(b'CTR-9DB0' + db_id.to_bytes(4, 'little') + diff[0x100:0x200]).digest()
    cmac = crypto.create_cmac_object(Keyslot.CMACNANDDB)
    cmac.update(digest)
    return cmac.digest() + diff[0x10:]


def build_database(db: tuple[int, bytes], titles: list[TitleInfo], crypto) -> bytes:
    db_id, magic = db
    fs = BDRI(magic)
    for t in titles:
        fs.add(t)
    return sign_diff(build_diff(bytes(fs.img)), db_id, crypto)
