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


class _FakeRecord:
    def __init__(self, cid, cindex, size):
        self.id, self.cindex, self.size = cid, cindex, size


class _FakeTMD:
    def __init__(self, tid):
        self.title_id, self.title_version = tid, 0x2C10


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


if __name__ == '__main__':
    unittest.main()
