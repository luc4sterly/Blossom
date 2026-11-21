"""
Check Blossom's AES engine log for one keyslot against the known 3DS keyX (pyctr's table).

Replays the "[AES] ..." lines Blossom prints (CNT, KEYCNT, XFIFO, YFIFO, "Generated key") with
the same FIFO rules as src/core/arm9/aes.cpp, then says whether the keyX the firmware wrote and
each normal key Blossom generated match what pyctr computes. It only prints MATCH / MISMATCH and
which byte order would have matched, never key material.

    python tools/mknand/aeslog.py blossom.log [--slot 0x25]
"""
from __future__ import annotations

import argparse
import re
import sys

LINE = re.compile(r'\[AES\] (Write CNT|KEYCNT|Write XFIFO|Write YFIFO|Generated key): \$?([0-9A-Fa-f]+)')
MASK128 = (1 << 128) - 1
KEY_CONST = 0x1FF9E9AAC5FE0408024591DC5D52768A


def rol(x: int, n: int) -> int:
    return ((x << n) | (x >> (128 - n))) & MASK128


def scramble(key_x: int, key_y: int) -> int:
    return rol((rol(key_x, 2) ^ key_y) + KEY_CONST & MASK128, 87)


def variants(b: bytes) -> dict[str, bytes]:
    words = [b[i:i + 4] for i in range(0, 16, 4)]
    return {
        'as is': b,
        'bytes reversed': b[::-1],
        'word order reversed': b''.join(words[::-1]),
        'bytes swapped in each word': b''.join(w[::-1] for w in words),
    }


def which(actual: bytes, expected: bytes) -> str:
    for name, v in variants(actual).items():
        if v == expected:
            return 'MATCH' if name == 'as is' else f'MISMATCH (would match with {name})'
    return 'MISMATCH (no simple byte order fixes it)'


class Replay:
    """The key FIFO part of aes.cpp: input_vector() with CNT bits 23 (big endian) and 25 (word order)."""

    def __init__(self):
        self.in_big_endian = False
        self.in_word_order = False
        self.keycnt = 0
        self.x = bytearray(16)
        self.y = bytearray(16)
        self.x_ctr = self.y_ctr = 0
        self.keys_x: dict[int, bytes] = {}
        self.keys_y: dict[int, bytes] = {}

    def _put(self, vec: bytearray, index: int, value: int) -> None:
        if not self.in_word_order:
            index = 3 - index
        if not self.in_big_endian:
            value = int.from_bytes(value.to_bytes(4, 'little'), 'big')
        vec[index * 4:index * 4 + 4] = value.to_bytes(4, 'little')

    def cnt(self, value: int) -> None:
        word_order = bool(value & (1 << 25))
        if word_order != self.in_word_order:
            self.x_ctr = self.y_ctr = 0
        self.in_big_endian = bool(value & (1 << 23))
        self.in_word_order = word_order

    def xfifo(self, value: int) -> int | None:
        self._put(self.x, self.x_ctr, value)
        self.x_ctr += 1
        if self.x_ctr >= 4:
            self.x_ctr = 0
            self.keys_x[self.keycnt & 0x3F] = bytes(self.x)
            return self.keycnt & 0x3F
        return None

    def yfifo(self, value: int) -> int | None:
        self._put(self.y, self.y_ctr, value)
        self.y_ctr += 1
        if self.y_ctr >= 4:
            self.y_ctr = 0
            self.keys_y[self.keycnt & 0x3F] = bytes(self.y)
            return self.keycnt & 0x3F
        return None


def check(lines, slot: int, expected_x: int) -> list[str]:
    r = Replay()
    out = []
    pending_gen = None
    n_x = n_gen = 0
    for line in lines:
        m = LINE.search(line)
        if not m:
            continue
        kind, val = m.group(1), m.group(2)
        if kind == 'Generated key':
            if pending_gen is not None:
                n_gen += 1
                key_y = int.from_bytes(r.keys_y[slot], 'big')
                key_x = int.from_bytes(r.keys_x.get(slot, bytes(16)), 'big')
                printed = bytes.fromhex(val)
                with_written = scramble(key_x, key_y).to_bytes(16, 'big')
                with_real = scramble(expected_x, key_y).to_bytes(16, 'big')
                out.append(f'normal key #{n_gen} for {slot:#04x}: vs scrambler(written keyX, keyY) '
                           f'{which(printed, with_written)}; vs scrambler(real keyX, keyY) '
                           f'{which(printed, with_real)}')
                pending_gen = None
            continue
        value = int(val, 16)
        if kind == 'Write CNT':
            r.cnt(value)
        elif kind == 'KEYCNT':
            r.keycnt = value
        elif kind == 'Write XFIFO':
            if r.xfifo(value) == slot:
                n_x += 1
                out.append(f'keyX write #{n_x} to {slot:#04x}: '
                           f'{which(r.keys_x[slot], expected_x.to_bytes(16, "big"))}')
        elif kind == 'Write YFIFO':
            if r.yfifo(value) == slot:
                pending_gen = slot
    if not n_x:
        out.append(f'no complete keyX write to {slot:#04x} found')
    if not n_gen:
        out.append(f'no normal key generated for {slot:#04x}')
    return out


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    p.add_argument('log')
    p.add_argument('--slot', default='0x25')
    p.add_argument('--dev', action='store_true', help='compare with the dev-unit keyX instead of retail')
    args = p.parse_args(argv)
    from pyctr.crypto.engine import base_key_x
    slot = int(args.slot, 0)
    if slot not in base_key_x:
        print(f'pyctr has no fixed keyX for {slot:#x}', file=sys.stderr)
        return 1
    expected = base_key_x[slot][1 if args.dev else 0]
    with open(args.log, errors='replace') as f:
        for line in check(f, slot, expected):
            print(line)
    return 0


if __name__ == '__main__':
    sys.exit(main())
