#!/usr/bin/env python3
"""pack_bnr.py — pack / inspect 3DS HOME-menu banners (CBMD, .bnr).

Layout per https://www.3dbrew.org/wiki/CBMD:
  0x00  "CBMD" magic
  0x04  u32 zero
  0x08  u32 offset of the common (language-neutral) LZ11-compressed CGFX
  0x0C  u32[13] offsets of per-language CGFXs (0 = use common)
  0x40  0x44 bytes zero padding
  0x84  u32 offset of the (uncompressed) CWAV
  0x88  header ends; compressed common CGFX follows, then the CWAV

The CGFX is LZ11-compressed ("\\x11" + 24-bit decompressed size, then
flag-byte groups).  Decompressed CGFXs larger than 0x80000 (512 KB) are
not supported by the HOME menu.

Subcommands:
  extract-cwav  in.bnr out.cwav       pull the CWAV chunk out of a banner
  pack          in.cgfx in.cwav out.bnr
  verify        banner.bnr [ref.cwav] structural unpack + optional CWAV
                                      byte-identity check
"""

import struct
import sys

CGFX_DECOMP_CAP = 0x80000


# ------------------------------------------------------------------- LZ11

def lz11_compress(data: bytes) -> bytes:
    """Greedy LZ11 with a 4 KB window (format per GBATEK / 3dbrew)."""
    n = len(data)
    out = bytearray()
    out += struct.pack("<I", 0x11 | (n << 8))

    # hash chains over 3-byte prefixes
    heads: dict[bytes, list[int]] = {}
    pos = 0
    tokens: list = []  # each: bytes (literal) or (disp, length)

    def find_match(p):
        if p + 3 > n:
            return None
        best_len = 0
        best_disp = 0
        for cand in reversed(heads.get(data[p:p + 3], ())):
            if p - cand > 0x1000:
                break
            length = 0
            limit = min(n - p, 0xFFFF + 0x111)
            while length < limit and data[cand + length] == data[p + length]:
                length += 1
            if length > best_len:
                best_len, best_disp = length, p - cand
                if length >= 0x100:  # good enough, stop searching
                    break
        return (best_disp, best_len) if best_len >= 3 else None

    def index(p):
        if p + 3 <= n:
            heads.setdefault(data[p:p + 3], []).append(p)

    while pos < n:
        m = find_match(pos)
        if m:
            disp, length = m
            tokens.append(m)
            for i in range(length):
                index(pos + i)
            pos += length
        else:
            tokens.append(data[pos:pos + 1])
            index(pos)
            pos += 1

    # serialize: flag byte then 8 tokens, MSB first, flag bit 1 = reference
    for group in (tokens[i:i + 8] for i in range(0, len(tokens), 8)):
        flags = 0
        body = bytearray()
        for bit, tok in enumerate(group):
            if isinstance(tok, bytes):
                body += tok
                continue
            flags |= 0x80 >> bit
            disp, length = tok
            d = disp - 1
            if length <= 0x10:
                body += bytes(((length - 1) << 4 | d >> 8, d & 0xFF))
            elif length <= 0x110:
                lv = length - 0x11
                body += bytes((lv >> 4, (lv & 0xF) << 4 | d >> 8, d & 0xFF))
            else:
                lv = length - 0x111
                body += bytes((0x10 | lv >> 12, lv >> 4 & 0xFF,
                               (lv & 0xF) << 4 | d >> 8, d & 0xFF))
        out.append(flags)
        out += body
    return bytes(out)


def lz11_decompress(data: bytes) -> bytes:
    if data[0] != 0x11:
        raise ValueError("not LZ11 (leading byte 0x%02X)" % data[0])
    size = struct.unpack("<I", data[:4])[0] >> 8
    out = bytearray()
    pos = 4
    while len(out) < size:
        flags = data[pos]
        pos += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if not flags & (0x80 >> bit):
                out.append(data[pos])
                pos += 1
                continue
            b0 = data[pos]
            ind = b0 >> 4
            if ind == 0:
                length = (b0 & 0xF) << 4 | data[pos + 1] >> 4
                length += 0x11
                disp = (data[pos + 1] & 0xF) << 8 | data[pos + 2]
                pos += 3
            elif ind == 1:
                length = ((b0 & 0xF) << 12 | data[pos + 1] << 4
                          | data[pos + 2] >> 4) + 0x111
                disp = (data[pos + 2] & 0xF) << 8 | data[pos + 3]
                pos += 4
            else:
                length = ind + 1
                disp = (b0 & 0xF) << 8 | data[pos + 1]
                pos += 2
            disp += 1
            for _ in range(length):
                out.append(out[-disp])
    return bytes(out)


# ------------------------------------------------------------------- CBMD

def parse_header(bnr: bytes):
    if bnr[:4] != b"CBMD":
        raise ValueError("bad magic %r" % bnr[:4])
    cgfx_off = struct.unpack("<I", bnr[8:12])[0]
    langs = struct.unpack("<13I", bnr[0x0C:0x40])
    cwav_off = struct.unpack("<I", bnr[0x84:0x88])[0]
    return cgfx_off, langs, cwav_off


def pack(cgfx: bytes, cwav: bytes) -> bytes:
    if cgfx[:4] != b"CGFX":
        raise ValueError("input CGFX has bad magic %r" % cgfx[:4])
    if len(cgfx) > CGFX_DECOMP_CAP:
        raise ValueError("CGFX %d bytes exceeds HOME-menu cap %d"
                         % (len(cgfx), CGFX_DECOMP_CAP))
    if cwav[:4] != b"CWAV":
        raise ValueError("input CWAV has bad magic %r" % cwav[:4])
    comp = lz11_compress(cgfx)
    assert lz11_decompress(comp) == cgfx, "LZ11 round-trip failed"
    # THE 2026-08-12 DOUBLE-FREEZE FIX. The CWAV lands at 0x88 + len(comp),
    # and len(comp) is an LZ11 output length -- effectively a coin flip mod 4.
    # The HOME menu's CWAV parser does 32-bit loads (sample rate at INFO+0x4C);
    # an odd offset data-aborts the whole menu process (ARM11 alignment fault,
    # reconstructed to the byte from the console's exception dump). bannertool
    # always padded to 16; we regenerated three banners without padding and
    # two of the three landed misaligned -- both froze consoles, and the
    # geometry/billboard/layout took the blame for a day. Pad to 16 like
    # bannertool (only 4 is evidenced as required; 16 is free margin). The
    # LZ11 stream carries its own decompressed size, so trailing zeros are
    # never read.
    comp += b"\0" * (-(0x88 + len(comp)) % 16)
    assert (0x88 + len(comp)) % 16 == 0
    header = (b"CBMD" + struct.pack("<I", 0) + struct.pack("<I", 0x88)
              + struct.pack("<13I", *([0] * 13)) + b"\x00" * 0x44
              + struct.pack("<I", 0x88 + len(comp)))
    assert len(header) == 0x88
    return header + comp + cwav


def verify(bnr: bytes, ref_cwav: bytes | None) -> None:
    cgfx_off, langs, cwav_off = parse_header(bnr)
    print("size          %d" % len(bnr))
    print("common CGFX @ 0x%X" % cgfx_off)
    print("language offsets: %s"
          % ("all 0 (use common)" if not any(langs) else langs))
    print("CWAV        @ 0x%X (%d bytes)" % (cwav_off, len(bnr) - cwav_off))
    assert cgfx_off == 0x88, "unexpected CGFX offset"
    assert 0x88 < cwav_off <= len(bnr), "CWAV offset out of range"
    cgfx = lz11_decompress(bnr[cgfx_off:cwav_off])
    print("CGFX decompresses to %d bytes (cap %d, margin %d)"
          % (len(cgfx), CGFX_DECOMP_CAP, CGFX_DECOMP_CAP - len(cgfx)))
    assert cgfx[:4] == b"CGFX", "decompressed CGFX has bad magic"
    assert len(cgfx) <= CGFX_DECOMP_CAP, "CGFX over HOME-menu cap"
    cwav = bnr[cwav_off:]
    assert cwav_off % 4 == 0, \
        "CWAV at %d is not 4-aligned -- the HOME menu will data-abort" % cwav_off
    assert cwav[:4] == b"CWAV", "CWAV has bad magic"
    if ref_cwav is not None:
        assert cwav == ref_cwav, "CWAV differs from reference"
        print("CWAV is byte-identical to the reference")
    print("OK")


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    cmd, *rest = args
    if cmd == "extract-cwav":
        bnr = open(rest[0], "rb").read()
        _, _, cwav_off = parse_header(bnr)
        cwav = bnr[cwav_off:]
        assert cwav[:4] == b"CWAV"
        open(rest[1], "wb").write(cwav)
        print("wrote %s (%d bytes)" % (rest[1], len(cwav)))
    elif cmd == "pack":
        cgfx = open(rest[0], "rb").read()
        cwav = open(rest[1], "rb").read()
        out = pack(cgfx, cwav)
        open(rest[2], "wb").write(out)
        print("wrote %s (%d bytes; CGFX %d -> %d compressed)"
              % (rest[2], len(out), len(cgfx), len(out) - 0x88 - len(cwav)))
    elif cmd == "verify":
        bnr = open(rest[0], "rb").read()
        ref = open(rest[1], "rb").read() if len(rest) > 1 else None
        verify(bnr, ref)
    else:
        sys.exit("unknown subcommand %r\n%s" % (cmd, __doc__))


if __name__ == "__main__":
    main()
