# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for mknand. They never touch real keys: the end-to-end test uses a random stand-in
# for boot9.bin and tells pyctr to accept it, so the whole pipeline (OTP -> keys -> encrypted
# NAND -> re-read) runs with fake keys. That checks the image is self-consistent; whether real
# boot9 accepts it can only be tested in Blossom with your own boot9.bin.

import hashlib
import io
import os
import random
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mknand  # noqa: E402


class PureBuilders(unittest.TestCase):
    def test_otp_magic_and_hash(self):
        otp = mknand.build_otp_plain(random.Random(1))
        self.assertEqual(len(otp), 0x100)
        self.assertEqual(struct.unpack_from('<I', otp, 0)[0], 0xDEADB00F)
        self.assertEqual(otp[0xE0:], hashlib.sha256(otp[:0xE0]).digest())
        self.assertEqual(otp[0x80:0x90], bytes(0x10))

    def test_exefs_layout_matches_blossom_reader(self):
        otp, cid = bytes(range(256)), b'C' * 16
        exefs = mknand.build_exefs([('otp', otp), ('nand_cid', cid)])
        # Blossom: offset in header + 0x400 (exefs at 0x200 + 0x200 header) = NAND offset
        name0, off0, size0 = struct.unpack_from('<8sII', exefs, 0)
        name1, off1, size1 = struct.unpack_from('<8sII', exefs, 0x10)
        self.assertEqual(name0.rstrip(b'\0'), b'otp')
        self.assertEqual(name1.rstrip(b'\0'), b'nand_cid')
        self.assertEqual(exefs[0x200 + off0:0x200 + off0 + size0], otp)
        self.assertEqual(exefs[0x200 + off1:0x200 + off1 + size1], cid)
        self.assertEqual(exefs[0x1E0:0x200], hashlib.sha256(otp).digest())

    def test_ncsd_is_old3ds_for_blossom(self):
        hdr = mknand.build_ncsd_header_body()
        self.assertEqual(hdr[0x100:0x104], b'NCSD')
        # Blossom's EMMC::is_n3ds() looks for crypt type 3 here
        self.assertNotIn(3, hdr[0x118:0x120])
        end = max(o + s for _, _, o, s in mknand.O3DS_PARTITIONS)
        self.assertEqual(end, mknand.O3DS_NAND_SIZE)

    def test_self_signature_is_pkcs1(self):
        sig = mknand.self_sign(b'x' * 0x100)
        self.assertEqual(len(sig), 0x100)
        self.assertEqual(sig[:3], b'\x00\x01\xff')
        self.assertEqual(sig[-32:], hashlib.sha256(b'x' * 0x100).digest())

    def test_fat16_sizes(self):
        for entry in mknand.TWL_MBR_ENTRIES + mknand.CTR_MBR_ENTRIES:
            data, _ = mknand.build_fat16_boot_and_tables(entry[5], b'TEST', 1)
            self.assertEqual(data[0x36:0x3E], b'FAT16   ')
            self.assertEqual(data[0x1FE:0x200], b'\x55\xAA')

    def test_mbr_partitions_fit(self):
        twl_end = max(e[4] + e[5] for e in mknand.TWL_MBR_ENTRIES) * 0x200
        self.assertLessEqual(twl_end, mknand.O3DS_PARTITIONS[0][3])
        ctr_end = max(e[4] + e[5] for e in mknand.CTR_MBR_ENTRIES) * 0x200
        self.assertLessEqual(ctr_end, mknand.O3DS_PARTITIONS[4][3])

    def test_cid_top_byte_is_zero(self):
        # boot9 hashes the CID with byte 15 cleared; a non-zero byte there breaks the NAND counter.
        for seed in range(20):
            self.assertEqual(mknand.build_cid(random.Random(seed))[15], 0)

    def test_rejects_encrypted_firm(self):
        with self.assertRaises(mknand.MkNandError):
            mknand.check_firm(b'\x12\x34' * 100)


try:
    import pyctr.crypto.engine as pyctr_engine
except ImportError:  # pragma: no cover
    pyctr_engine = None


@unittest.skipIf(pyctr_engine is None, 'pyctr is not installed')
class EndToEndWithFakeKeys(unittest.TestCase):
    def test_build_and_verify(self):
        with tempfile.TemporaryDirectory() as tmp:
            fake_b9 = random.Random(9).randbytes(0x10000)
            b9_path = os.path.join(tmp, 'fake_boot9.bin')
            with open(b9_path, 'wb') as f:
                f.write(fake_b9)
            pyctr_engine.BOOT9_PROT_HASH = hashlib.sha256(fake_b9[0x8000:]).hexdigest()

            firm = b'FIRM' + random.Random(3).randbytes(0x20000)
            out = os.path.join(tmp, 'nand.bin')
            result = mknand.build_nand(out, b9_path, firm, seed='test')
            self.assertEqual(os.path.getsize(out), mknand.O3DS_NAND_SIZE)
            self.assertEqual(mknand.verify_nand(out, b9_path, firm), [])

            # What Blossom's EMMC::parse_essentials() would load
            with open(out, 'rb') as f:
                f.seek(0x200)
                ex = f.read(0x200)
                found = {}
                for i in range(0, 0xA0, 0x10):
                    name, off, size = struct.unpack_from('<8sII', ex, i)
                    if name.rstrip(b'\0'):
                        f.seek(off + 0x400)
                        found[name.rstrip(b'\0')] = f.read(size)
            self.assertEqual(found[b'otp'], result.otp_enc)
            self.assertEqual(found[b'nand_cid'], result.cid)
            self.assertNotEqual(result.otp_enc[:4], b'\x0f\xb0\xad\xde')  # stored encrypted

            hashes = dict((label.split()[0], value) for label, value in mknand.explain_hashes(out, b9_path))
            self.assertEqual(hashes['sha256(otp_dec[0:0xE0])'], hashes['otp_dec[0xE0:0x100]'])
            self.assertEqual(hashes['sha256(nand_cid)'], hashlib.sha256(result.cid).hexdigest())


class TitleDB(unittest.TestCase):
    def test_entries_found_through_hash_table_and_directory(self):
        import titledb
        fs = titledb.BDRI(b'NANDTDB\0')
        tids = [0x0004013000008002 + i * 0x100 for i in range(200)]
        for t in tids:
            fs.add(titledb.TitleInfo(title_id=t, size=0x8000, version=1))
        for t in tids:
            self.assertIsNotNone(fs.lookup(t))
        self.assertEqual(sorted(fs.read_titles()), sorted(tids))
        self.assertIsNone(fs.lookup(0x0004013000009999))

    @unittest.skipIf(pyctr_engine is None, 'pyctr is not installed')
    def test_diff_reads_back_with_valid_hashes(self):
        import titledb
        from pyctr.crypto import CryptoEngine
        from pyctr.type.save.diff import DIFF
        from pyctr.type.save.partdesc.ivfc import IVFCLevel4Reader
        fs = titledb.BDRI(b'NANDTDB\0')
        fs.add(titledb.TitleInfo(title_id=0x0004013000008002, size=0x8000))
        raw = titledb.build_diff(bytes(fs.img))
        diff = DIFF(io.BytesIO(raw), crypto=CryptoEngine(setup_b9_keys=False))
        self.assertEqual(IVFCLevel4Reader(diff.partitions[0].ivfc_hash_tree).read(), bytes(fs.img))


def _am_style_empty_db(titledb):
    """An empty title.db like AM might make: smaller tables and one multi-block free node."""
    g = titledb.Geometry(file_hash_buckets=0x101, max_files=0x40, file_table_blocks=0x17)
    fs = titledb.BDRI(b'NANDTDB\0', g)
    first = g.file_table_block + g.file_table_blocks + 1
    last = g.data_blocks - 1
    fs._fat_set(0, 0, first)
    fs._fat_set(first, 0x80000000, 0x80000000)
    fs._fat_set(first + 1, 0x80000000 | first, last)
    fs._fat_set(last, 0x80000000 | first, last)
    # file table never used: high-water mark 1, empty recycle list
    e0 = fs._file_entry(0)
    struct.pack_into('<II', e0, 0, 1, g.max_files + 1)
    struct.pack_into('<I', e0, 0x28, 0)
    return fs


class ExistingDatabases(unittest.TestCase):
    def test_add_to_am_style_db(self):
        import titledb
        fs = _am_style_empty_db(titledb)
        img = titledb.BDRI.from_image(bytes(fs.img))
        tids = [0x0004013000001502 + i * 0x100 for i in range(30)]
        for t in tids:
            img.add(titledb.TitleInfo(title_id=t, size=0x8000, version=t & 0xFFFF))
        for t in tids:
            entry = img.lookup(t)
            self.assertIsNotNone(entry)
            self.assertEqual(struct.unpack_from('<H', entry, 0x0C)[0], t & 0xFFFF)
        blocks = [struct.unpack_from('<I', img._file_entry(i), 0x14)[0] for i in range(1, 31)]
        self.assertEqual(len(set(blocks)), 30)

    @unittest.skipIf(pyctr_engine is None, 'pyctr is not installed')
    def test_rewrite_diff_keeps_geometry_and_hashes(self):
        import titledb
        fs = _am_style_empty_db(titledb)
        raw = titledb.build_diff(bytes(fs.img))
        img = titledb.BDRI.from_image(titledb.read_diff(raw))
        img.add(titledb.TitleInfo(title_id=0x0004013000008002, size=0x8000))
        new = titledb.rewrite_diff(raw, bytes(img.img))
        self.assertEqual(len(new), len(raw))
        self.assertEqual(titledb.read_diff(new), bytes(img.img))
        from pyctr.crypto import CryptoEngine
        from pyctr.type.save.diff import DIFF
        from pyctr.type.save.partdesc.ivfc import IVFCLevel4Reader
        d = DIFF(io.BytesIO(new), crypto=CryptoEngine(setup_b9_keys=False))
        self.assertNotIn(b'\xDD' * 0x200, IVFCLevel4Reader(d.partitions[0].ivfc_hash_tree, verify=True).read())


class _FakeRecord:
    def __init__(self, cid, cindex, size):
        self.id, self.cindex, self.size = cid, cindex, size


class _FakeTMD:
    def __init__(self, tid):
        self.title_id, self.title_version = tid, 0x2C10
        self.save_size = 0x4000
        self._u_title_type = b'\0\0\0\x40'


class _FakeCIA:
    """Stands in for pyctr's CIAReader: decrypted sections keyed by the CIA path's title ID."""
    def __init__(self, path, crypto=None, load_contents=True):
        tid = os.path.basename(path)[:16]
        self.tmd = _FakeTMD(tid)
        rnd = random.Random(tid)
        self.data = {-1: b'TMD!' + rnd.randbytes(0x200), 0: rnd.randbytes(0x30000), 1: rnd.randbytes(0x1234)}
        self.content_info = [_FakeRecord('0000001a', 0, len(self.data[0])), _FakeRecord('0000002B', 1, len(self.data[1]))]

    def __enter__(self):
        return self

    def __exit__(self, *a):
        pass

    def open_raw_section(self, section):
        return io.BytesIO(self.data[int(section)])


@unittest.skipIf(pyctr_engine is None, 'pyctr is not installed')
class InstallTitlesWithFakeCIAs(unittest.TestCase):
    def test_titles_land_in_ctrnand(self):
        import pyctr.type.cia as cia_mod
        from pyfatfs.PyFatFS import PyFatFS
        from pyctr.type.nand import NAND

        real = cia_mod.CIAReader
        cia_mod.CIAReader = _FakeCIA
        try:
            with tempfile.TemporaryDirectory() as tmp:
                fake_b9 = random.Random(9).randbytes(0x10000)
                b9_path = os.path.join(tmp, 'fake_boot9.bin')
                with open(b9_path, 'wb') as f:
                    f.write(fake_b9)
                pyctr_engine.BOOT9_PROT_HASH = hashlib.sha256(fake_b9[0x8000:]).hexdigest()
                cias = [os.path.join(tmp, '0004013000001502.cia'), os.path.join(tmp, '0004800542383841.cia')]
                for c in cias:
                    open(c, 'wb').close()

                firm = b'FIRM' + bytes(0x1000)
                out = os.path.join(tmp, 'nand.bin')
                result = mknand.build_nand(out, b9_path, firm, 'titles', cias, log=lambda *_: None)
                self.assertEqual([t.title_id for t in result.titles], ['0004013000001502'])  # TWL one skipped
                self.assertEqual(mknand.verify_nand(out, b9_path, firm, result.titles), [])

                # Check the bytes themselves through an independent decrypt + mount
                fake = _FakeCIA(cias[0])
                nand = NAND(out, crypto=mknand.make_crypto(b9_path))
                vol_path = os.path.join(tmp, 'ctr.img')
                with nand.open_ctr_partition(0) as src, open(vol_path, 'wb') as dst:
                    dst.write(src.read())
                nand.close()
                vol = PyFatFS(vol_path)
                base = '/title/00040130/00001502/content'
                self.assertEqual(vol.readbytes(f'{base}/00000000.tmd'), fake.data[-1])
                self.assertEqual(vol.readbytes(f'{base}/0000001a.app'), fake.data[0])
                self.assertEqual(vol.readbytes(f'{base}/0000002b.app'), fake.data[1])
                for d in mknand.CTRNAND_DIRS:
                    self.assertTrue(vol.isdir(d), d)
                self.assertTrue(vol.exists('/dbs/import.db'))
                movable = vol.readbytes('/private/movable.sed')
                self.assertEqual((len(movable), movable[:4]), (0x120, b'SEED'))
                self.assertEqual(movable[0x108:0x110], bytes(8))
                lfcs = vol.readbytes('/rw/sys/LocalFriendCodeSeed_B')
                self.assertEqual((len(lfcs), lfcs), (0x110, movable[0x08:0x118]))
                self.assertEqual(lfcs[:0x100], mknand.self_sign(lfcs[0x100:]))
                si = vol.readbytes('/rw/sys/SecureInfo_A')
                self.assertEqual((len(si), si[0x100], si[0x102:0x104]), (0x111, 2, b'EW'))
                self.assertEqual(si[:0x100], mknand.self_sign(si[0x100:]))
                self.assertEqual(movable[0x08:0x108], mknand.self_sign(movable[0x108:0x118]))
                cmd = vol.readbytes(f'{base}/cmd/00000001.cmd')
                crypto = mknand.make_crypto(b9_path)
                crypto.setup_keys_from_otp(result.otp_enc)
                crypto.setup_sd_key(movable)
                self.assertEqual(cmd, mknand.build_cmd(mknand._cmd_inputs(fake), crypto))
                self.assertEqual(mknand.verify_title_db(vol.readbytes('/dbs/title.db'), result.titles), [])
                vol.close()

                nand = NAND(out, crypto=mknand.make_crypto(b9_path))
                with nand.open_twl_partition(0) as src, open(vol_path, 'wb') as dst:
                    dst.write(src.read())
                nand.close()
                vol = PyFatFS(vol_path)
                for d in mknand.TWLN_DIRS:
                    self.assertTrue(vol.isdir(d), d)
                vol.close()
        finally:
            cia_mod.CIAReader = real

    def test_update_dbs_on_an_am_made_title_db(self):
        import pyctr.type.cia as cia_mod
        import titledb
        real = cia_mod.CIAReader
        cia_mod.CIAReader = _FakeCIA
        try:
            with tempfile.TemporaryDirectory() as tmp:
                fake_b9 = random.Random(9).randbytes(0x10000)
                b9_path = os.path.join(tmp, 'fake_boot9.bin')
                with open(b9_path, 'wb') as f:
                    f.write(fake_b9)
                pyctr_engine.BOOT9_PROT_HASH = hashlib.sha256(fake_b9[0x8000:]).hexdigest()
                cia = os.path.join(tmp, '0004013000008002.cia')
                open(cia, 'wb').close()
                out = os.path.join(tmp, 'nand.bin')
                mknand.build_nand(out, b9_path, b'FIRM' + bytes(0x100), 'am', [], log=lambda *_: None)

                # Pretend AM replaced the databases on first boot
                m = mknand.MountedCTRNAND(out, b9_path, writable=True)
                m.fs.remove('/dbs/title.db')
                with m.fs.openbin('/dbs/TITLE.DB', 'w') as f:
                    f.write(titledb.sign_diff(titledb.build_diff(bytes(_am_style_empty_db(titledb).img)),
                                              2, m.crypto))
                m.save()
                m.close()

                self.assertEqual(mknand.update_dbs(out, b9_path, [cia], log=lambda *_: None), 1)
                lines = mknand.inspect_dbs(out, b9_path)
                title_line = [l for l in lines if l.lower().startswith('title.db')][0]
                self.assertIn('CMAC OK', title_line)
                self.assertIn('entries 1', title_line)
                self.assertIn('max files 0x40', title_line)
        finally:
            cia_mod.CIAReader = real


    def test_check_code_on_fake_content(self):
        # The fake contents are not NCCH, so it must say so instead of crashing
        import pyctr.type.cia as cia_mod
        real = cia_mod.CIAReader
        cia_mod.CIAReader = _FakeCIA
        try:
            with tempfile.TemporaryDirectory() as tmp:
                fake_b9 = random.Random(9).randbytes(0x10000)
                b9_path = os.path.join(tmp, 'fake_boot9.bin')
                with open(b9_path, 'wb') as f:
                    f.write(fake_b9)
                pyctr_engine.BOOT9_PROT_HASH = hashlib.sha256(fake_b9[0x8000:]).hexdigest()
                cia = os.path.join(tmp, '0004013000008002.cia')
                open(cia, 'wb').close()
                out = os.path.join(tmp, 'nand.bin')
                mknand.build_nand(out, b9_path, b'FIRM' + bytes(0x100), 'code', [cia], log=lambda *_: None)
                lines = mknand.check_code(out, b9_path, '00040130 00008002')
                self.assertEqual(len(lines), 2)
                self.assertTrue(all('error' in l for l in lines), lines)
                lines = mknand.title_deps(out, b9_path, '00040130 00008002')
                self.assertEqual(len(lines), 2)
                self.assertTrue(all('error' in l for l in lines), lines)
        finally:
            cia_mod.CIAReader = real

    def test_fix_titles_writes_cmd_and_movable(self):
        import pyctr.type.cia as cia_mod
        real = cia_mod.CIAReader
        cia_mod.CIAReader = _FakeCIA
        try:
            with tempfile.TemporaryDirectory() as tmp:
                fake_b9 = random.Random(9).randbytes(0x10000)
                b9_path = os.path.join(tmp, 'fake_boot9.bin')
                with open(b9_path, 'wb') as f:
                    f.write(fake_b9)
                pyctr_engine.BOOT9_PROT_HASH = hashlib.sha256(fake_b9[0x8000:]).hexdigest()
                cia = os.path.join(tmp, '0004013000008002.cia')
                open(cia, 'wb').close()
                out = os.path.join(tmp, 'nand.bin')
                mknand.build_nand(out, b9_path, b'FIRM' + bytes(0x100), 'fix', [cia], log=lambda *_: None)
                # Like an image built before cmd files existed
                m = mknand.MountedCTRNAND(out, b9_path, writable=True)
                m.fs.remove('/title/00040130/00008002/content/cmd/00000001.cmd')
                m.fs.remove('/private/movable.sed')
                m.save()
                m.close()

                self.assertEqual(mknand.fix_titles(out, b9_path, [cia], log=lambda *_: None, region='USA'), 1)
                m = mknand.MountedCTRNAND(out, b9_path)
                try:
                    movable = m.fs.readbytes('/private/movable.sed')
                    cmd = m.fs.readbytes('/title/00040130/00008002/content/cmd/00000001.cmd')
                    self.assertEqual(m.fs.readbytes('/rw/sys/SecureInfo_A')[0x100], 1)
                    self.assertEqual(m.fs.readbytes('/rw/sys/LocalFriendCodeSeed_B'), movable[0x08:0x118])
                    m.crypto.setup_sd_key(movable)
                    self.assertEqual(cmd, mknand.build_cmd(mknand._cmd_inputs(_FakeCIA(cia)), m.crypto))
                finally:
                    m.close()
        finally:
            cia_mod.CIAReader = real


@unittest.skipIf(pyctr_engine is None, 'pyctr is not installed')
class CmdFormat(unittest.TestCase):
    def test_layout_and_cmacs(self):
        from Cryptodome.Hash import CMAC
        from Cryptodome.Cipher import AES
        from pyctr.crypto import CryptoEngine
        crypto = CryptoEngine(setup_b9_keys=False)
        key = bytes(range(16))
        crypto.set_normal_key(0x0B, key)
        heads = [bytes([i]) * 0x100 for i in range(3)]
        cmd = mknand.build_cmd([(0, '0000000a', heads[0]), (2, '00000005', heads[2])], crypto)
        cmd_id, count, installed, one = struct.unpack_from('<IIII', cmd, 0)
        self.assertEqual((cmd_id, count, installed, one), (1, 3, 2, 1))
        def mac(d):
            return CMAC.new(key, d, ciphermod=AES).digest()
        self.assertEqual(cmd[0x10:0x20], mac(cmd[:0x10]))
        ids = [cmd[0x20 + 4 * i:0x24 + 4 * i] for i in range(3)]
        self.assertEqual(ids, [b'\x0a\0\0\0', b'\xff' * 4, b'\x05\0\0\0'])
        self.assertEqual(cmd[0x2C:0x34], b'\x05\0\0\0\x0a\0\0\0')  # sorted
        cmacs = cmd[0x34:]
        self.assertEqual(len(cmacs), 0x30)
        self.assertEqual(cmacs[0:0x10], mac(hashlib.sha256(heads[0] + bytes(4) + b'\x0a\0\0\0').digest()))
        self.assertEqual(cmacs[0x10:0x20], b'MISSING CONTENT!')
        self.assertEqual(cmacs[0x20:], mac(hashlib.sha256(heads[2] + b'\x02\0\0\0\x05\0\0\0').digest()))


@unittest.skipIf(pyctr_engine is None, 'pyctr is not installed')
class AESLog(unittest.TestCase):
    def _log(self, key_x: bytes, key_y: bytes, normal: bytes) -> list[str]:
        # CNT bits 23|25: Blossom then stores each FIFO word little-endian, in order
        words = lambda b: [int.from_bytes(b[i:i + 4], 'little') for i in range(0, 16, 4)]
        lines = ['[AES] Write CNT: $02800000', '[AES] KEYCNT: $A5']
        lines += [f'[AES] Write XFIFO: ${w:08X}' for w in words(key_x)]
        lines += [f'[AES] Write YFIFO: ${w:08X}' for w in words(key_y)]
        lines.append('[AES] Generated key: ' + normal.hex())
        return lines

    def test_good_and_bad_keys(self):
        import aeslog
        from pyctr.crypto import CryptoEngine
        real_x = random.Random(1).getrandbits(128)
        key_y = random.Random(2).randbytes(16)
        normal = CryptoEngine.keygen_manual(real_x, int.from_bytes(key_y, 'big'))
        good = aeslog.check(self._log(real_x.to_bytes(16, 'big'), key_y, normal), 0x25, real_x)
        self.assertEqual(len(good), 2)
        self.assertTrue(good[0].endswith('MATCH'), good)
        self.assertNotIn('MISMATCH', good[1])

        swapped = real_x.to_bytes(16, 'big')[::-1]
        bad = aeslog.check(self._log(swapped, key_y, normal), 0x25, real_x)
        self.assertIn('would match with bytes reversed', bad[0])
        self.assertIn('scrambler(real keyX, keyY) MATCH', bad[1])


if __name__ == '__main__':
    unittest.main()
