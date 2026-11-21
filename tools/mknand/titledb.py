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


@dataclass
class Geometry:
    """Where things are inside a BDRI image (offsets relative to the BDRI header, after the pre-header)."""
    block_size: int = BLOCK_SIZE
    dir_hash_offset: int = DIR_HASH_OFFSET
    dir_hash_buckets: int = DIR_HASH_BUCKETS
    file_hash_offset: int = FILE_HASH_OFFSET
    file_hash_buckets: int = FILE_HASH_BUCKETS
    fat_offset: int = FAT_OFFSET
    fat_entries: int = DATA_BLOCKS
    data_offset: int = DATA_OFFSET
    data_blocks: int = DATA_BLOCKS
    dir_table_block: int = DIR_TABLE_BLOCK
    dir_table_blocks: int = DIR_TABLE_BLOCKS
    max_dirs: int = MAX_DIRS
    file_table_block: int = FILE_TABLE_BLOCK
    file_table_blocks: int = FILE_TABLE_BLOCKS
    max_files: int = MAX_FILES

    FS_INFO = '<IIQIIQIIQIIQIIIIIIIIII'

    @classmethod
    def parse(cls, img: bytes) -> 'Geometry':
        b = PREHEADER_SIZE
        if img[b:b + 4] != b'BDRI':
            raise TitleDBError('BDRI magic not found')
        fs_info_off = struct.unpack_from('<Q', img, b + 8)[0]
        v = struct.unpack_from(cls.FS_INFO, img, b + fs_info_off)
        return cls(block_size=v[1], dir_hash_offset=v[2], dir_hash_buckets=v[3], file_hash_offset=v[5],
                   file_hash_buckets=v[6], fat_offset=v[8], fat_entries=v[9], data_offset=v[11],
                   data_blocks=v[12], dir_table_block=v[14], dir_table_blocks=v[15], max_dirs=v[16],
                   file_table_block=v[18], file_table_blocks=v[19], max_files=v[20])


class BDRI:
    """
    A title database file system. `BDRI(magic)` makes an empty one with AM's default geometry;
    `BDRI.from_image(img)` opens an existing one (for example one AM created on first boot).
    """

    def __init__(self, magic: bytes, geometry: Geometry | None = None):
        g = self.g = geometry or Geometry()
        size = IVFC_LEVELS[3][1]
        self.img = bytearray(size)
        self.img[0:8] = magic
        b = PREHEADER_SIZE
        struct.pack_into('<4sIQQI', self.img, b, b'BDRI', 0x30000, FS_INFO_OFFSET, size // g.block_size, g.block_size)
        struct.pack_into(Geometry.FS_INFO, self.img, b + FS_INFO_OFFSET,
                         0, g.block_size,
                         g.dir_hash_offset, g.dir_hash_buckets, 0,
                         g.file_hash_offset, g.file_hash_buckets, 0,
                         g.fat_offset, g.fat_entries, 0,
                         g.data_offset, g.data_blocks, 0,
                         g.dir_table_block, g.dir_table_blocks, g.max_dirs, 0,
                         g.file_table_block, g.file_table_blocks, g.max_files, 0)

        # Directory table: dummy head (2 entries in use) and an empty root.
        self._dir_entry(0)[0:8] = struct.pack('<II', 2, g.max_dirs + 2)
        struct.pack_into('<I', self.img, b + g.dir_hash_offset, ROOT_DIR)

        # File table: every slot is on the free (dummy) list, head -> max -> ... -> 1 -> end.
        for k in range(g.max_files + 1):
            e = self._file_entry(k)
            e[0:8] = struct.pack('<II', g.max_files + 1, g.max_files + 1)
            struct.pack_into('<I', e, 0x28, g.max_files if k == 0 else k - 1)

        # FAT: the two tables are allocated nodes, all other blocks form one free chain of
        # single-block nodes (FAT entry = block + 1).
        self._fat_set(0, 0, 0)
        self._alloc_node(g.dir_table_block + 1, g.dir_table_blocks)
        self._alloc_node(g.file_table_block + 1, g.file_table_blocks)
        first_free = g.file_table_block + g.file_table_blocks + 1
        # The last data block runs 0x80 bytes past the end of the image (a quirk of AM's layout),
        # so it is never handed out.
        free = list(range(first_free, g.data_blocks))
        for i, k in enumerate(free):
            prev = free[i - 1] if i else 0
            nxt = free[i + 1] if i + 1 < len(free) else 0
            self._fat_set(k, prev | (0x80000000 if i == 0 else 0), nxt)
        self._fat_set(0, 0, free[0])
        self.titles: list[int] = []

    @classmethod
    def from_image(cls, img: bytes) -> 'BDRI':
        self = cls.__new__(cls)
        self.img = bytearray(img)
        self.g = Geometry.parse(self.img)
        self.titles = list(self.read_titles())
        return self

    # -- raw accessors --

    def _view(self, off: int, size: int) -> memoryview:
        return memoryview(self.img)[off:off + size]

    def _data(self, block: int) -> int:
        return PREHEADER_SIZE + self.g.data_offset + block * self.g.block_size

    def _dir_entry(self, k: int) -> memoryview:
        return self._view(self._data(self.g.dir_table_block) + k * DIR_ENTRY_SIZE, DIR_ENTRY_SIZE)

    def _file_entry(self, k: int) -> memoryview:
        return self._view(self._data(self.g.file_table_block) + k * FILE_ENTRY_SIZE, FILE_ENTRY_SIZE)

    def _fat(self, k: int) -> tuple[int, int]:
        return struct.unpack_from('<II', self.img, PREHEADER_SIZE + self.g.fat_offset + k * 8)

    def _fat_set(self, k: int, u: int, v: int) -> None:
        struct.pack_into('<II', self.img, PREHEADER_SIZE + self.g.fat_offset + k * 8, u, v)

    def _alloc_node(self, k: int, n: int) -> None:
        """Mark FAT entries k..k+n-1 as a single-node chain."""
        if n == 1:
            self._fat_set(k, 0x80000000, 0)
        else:
            self._fat_set(k, 0x80000000, 0x80000000)
            self._fat_set(k + 1, 0x80000000 | k, k + n - 1)
            self._fat_set(k + n - 1, 0x80000000 | k, k + n - 1)

    def _pop_free_block(self) -> int:
        """Take the first block of the free chain (splitting a multi-block free node if needed)."""
        _, head = self._fat(0)
        if head == 0:
            raise TitleDBError('title database is full')
        u, v = self._fat(head)
        multi = bool(v & 0x80000000)
        nxt = v & 0x7FFFFFFF
        if multi:
            # Free node head..end: give away `head`, the rest becomes a node starting at head+1.
            _, end = self._fat(head + 1)
            new = head + 1
            if new == end:
                self._fat_set(new, 0x80000000, nxt)
            else:
                self._fat_set(new, 0x80000000, nxt | 0x80000000)
                self._fat_set(new + 1, 0x80000000 | new, end)
                self._fat_set(end, 0x80000000 | new, end)
            if nxt:
                _, nv = self._fat(nxt)
                self._fat_set(nxt, new, nv)
            self._fat_set(0, 0, new)
        else:
            self._fat_set(0, 0, nxt)
            if nxt:
                _, nv = self._fat(nxt)
                self._fat_set(nxt, 0x80000000, nv)
        self._alloc_node(head, 1)
        return head - 1

    def _pop_free_file_entry(self) -> int:
        head = struct.unpack_from('<I', self._file_entry(0), 0x28)[0]
        if head:
            nxt = struct.unpack_from('<I', self._file_entry(head), 0x28)[0]
            struct.pack_into('<I', self._file_entry(0), 0x28, nxt)
            return head
        # No recycled entry: take a fresh one after the high-water mark.
        count, maximum = struct.unpack_from('<II', self._file_entry(0), 0)
        if count >= maximum:
            raise TitleDBError('no free file entries')
        struct.pack_into('<I', self._file_entry(0), 0, count + 1)
        return count

    def _bucket_off(self, title_id: int) -> int:
        return (PREHEADER_SIZE + self.g.file_hash_offset
                + (file_hash(ROOT_DIR, title_id) % self.g.file_hash_buckets) * 4)

    # -- public --

    def add(self, info: TitleInfo) -> None:
        if info.title_id in self.titles:
            raise TitleDBError(f'duplicate title {info.title_id:016x}')
        if TITLE_ENTRY_SIZE > self.g.block_size:
            raise TitleDBError('entries larger than one block are not supported')
        block = self._pop_free_block()
        idx = self._pop_free_file_entry()

        data_off = self._data(block)
        self.img[data_off:data_off + TITLE_ENTRY_SIZE] = info.to_bytes()

        root = self._dir_entry(ROOT_DIR)
        first_file = struct.unpack_from('<I', root, 0x0C)[0]
        bucket_off = self._bucket_off(info.title_id)
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
            off = self._data(block)
            out[tid] = bytes(self.img[off:off + size])
            idx = nxt
            seen += 1
            if seen > self.g.max_files:
                raise TitleDBError('file list loops')
        return out

    def lookup(self, title_id: int) -> bytes | None:
        """Find a title through the hash table, as Process9 does."""
        idx = struct.unpack_from('<I', self.img, self._bucket_off(title_id))[0]
        while idx:
            e = self._file_entry(idx)
            parent, tid = struct.unpack_from('<IQ', e, 0)
            if parent == ROOT_DIR and tid == title_id:
                block, size = struct.unpack_from('<IQ', e, 0x14)
                off = self._data(block)
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


def read_diff(raw: bytes) -> bytes:
    """Return the IVFC level 4 content (the BDRI image) of any DIFF file, checking its hashes."""
    import io
    from pyctr.crypto import CryptoEngine
    from pyctr.type.save.diff import DIFF
    from pyctr.type.save.partdesc.ivfc import IVFCLevel4Reader
    diff = DIFF(io.BytesIO(raw), crypto=CryptoEngine(setup_b9_keys=False))
    data = IVFCLevel4Reader(diff.partitions[0].ivfc_hash_tree, verify=False).read()
    return data


def rewrite_diff(raw: bytes, lv4: bytes) -> bytes:
    """
    Put new level-4 content into an existing DIFF file, keeping its geometry: rebuild the whole
    IVFC tree, flatten the DPFS tree onto its first copies and update the partition descriptor
    and header hash. The CMAC is not touched (call sign_diff afterwards).
    """
    from pyctr.type.save.partition import load_partdesc, partdesc_to_bytes

    out = bytearray(raw)
    h = out[0x100:0x200]
    if h[0:8] != b'DIFF\0\0\3\0':
        raise TitleDBError('not a DIFF file')
    sec_off, pri_off, pd_size, part_off, part_size = struct.unpack_from('<QQQQQ', h, 8)
    active = struct.unpack_from('<I', h, 0x30)[0]
    pd_off = pri_off if active == 0 else sec_off
    difi, ivfc, dpfs, master = load_partdesc(bytes(out[pd_off:pd_off + pd_size]))
    if len(lv4) != ivfc.lv4.size:
        raise TitleDBError(f'level 4 size {len(lv4):#x} != {ivfc.lv4.size:#x}')

    levels = [bytes(lv4)]
    for lvl in (ivfc.lv3, ivfc.lv2, ivfc.lv1):
        data = _hash_level(levels[-1], lvl.block_size)
        levels.append(data.ljust(lvl.size, b'\0')[:lvl.size])
    lv1 = levels[-1]
    master = [hashlib.sha256(lv1[i:i + ivfc.lv1.block_size].ljust(ivfc.lv1.block_size, b'\0')).digest()
              for i in range(0, len(lv1), ivfc.lv1.block_size)]

    l3 = bytearray(out[part_off + dpfs.lv3.offset:part_off + dpfs.lv3.offset + dpfs.lv3.size])
    for lvl, data in zip((ivfc.lv1, ivfc.lv2, ivfc.lv3), (levels[3], levels[2], levels[1])):
        l3[lvl.offset:lvl.offset + lvl.size] = data
    if difi.enable_external_ivfc_lv4:
        o = part_off + difi.external_ivfc_lv4_offset
        out[o:o + len(lv4)] = lv4
    else:
        l3[ivfc.lv4.offset:ivfc.lv4.offset + len(lv4)] = lv4

    # DPFS: all selector bits 0, so the first copy of each level-3 block is the live one.
    for lvl in (dpfs.lv1, dpfs.lv2):
        o = part_off + lvl.offset
        out[o:o + lvl.size * 2] = bytes(lvl.size * 2)
    o = part_off + dpfs.lv3.offset
    out[o:o + len(l3)] = l3

    difi = difi._replace(dpfs_tree_lv1_selector=0)
    pd = partdesc_to_bytes(difi, ivfc, dpfs, master, pd_size)
    out[pd_off:pd_off + pd_size] = pd
    out[0x134:0x154] = hashlib.sha256(pd).digest()
    return bytes(out)


def check_cmac(raw: bytes, db_id: int, crypto) -> bool:
    return sign_diff(raw, db_id, crypto)[:0x10] == raw[:0x10]


def add_titles_to_existing(raw: bytes, db: tuple[int, bytes], titles: list[TitleInfo], crypto) -> bytes:
    """Add title entries to a database AM created (keeping its geometry) and re-sign it."""
    db_id, _ = db
    fs = BDRI.from_image(read_diff(raw))
    for t in titles:
        if t.title_id not in fs.titles:
            fs.add(t)
    return sign_diff(rewrite_diff(raw, bytes(fs.img)), db_id, crypto)
