#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Zero-dependency .po -> .ymo converter.

Byte-compatible with po2ymo.py (which requires the translate-toolkit package):
same FNV-1a 32-bit hashing over UTF-16LE encoded (msgctxt + msgid) strings and
the same binary layout: [uint16 len][(uint32 hash, uint16 offset) * len][data].

Usage: python po2ymo_stdlib.py <in.po> <out.ymo>
"""
import sys


FNV1_32_INIT = 0x811C9DC5
FNV_32_PRIME = 0x01000193


def fnv1a_32(data, hval=FNV1_32_INIT):
    for byte in data:
        hval ^= byte
        hval = (hval * FNV_32_PRIME) & 0xFFFFFFFF
    return hval


def _unquote(s):
    """Strip surrounding quotes and decode the C escapes gettext uses."""
    if s.startswith('"') and s.endswith('"'):
        s = s[1:-1]
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == '\\' and i + 1 < len(s):
            n = s[i + 1]
            if n == 'n':
                out.append('\n')
            elif n == 't':
                out.append('\t')
            elif n == 'r':
                out.append('\r')
            elif n == '"':
                out.append('"')
            elif n == '\\':
                out.append('\\')
            else:
                out.append(n)
            i += 2
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def parse_po(path):
    """Parse a gettext .po file; yield (context, source, target) for each
    translated entry (non-translated and empty entries are skipped)."""
    with open(path, encoding='utf-8-sig') as f:
        lines = f.read().splitlines()

    cur = {}
    key = None
    entries = []

    def flush():
        # Skip the po header entry (msgid "") — it is metadata, not a
        # translation; translate-toolkit's pofile treats it the same way.
        if cur.get('msgid') and cur.get('msgstr'):
            entries.append((cur.get('msgctxt', ''), cur['msgid'], cur['msgstr']))
        cur.clear()

    for line in lines:
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        if line.startswith('msgctxt '):
            flush()
            key = 'msgctxt'
            cur[key] = _unquote(line[len('msgctxt '):].strip())
        elif line.startswith('msgid '):
            flush()
            key = 'msgid'
            cur[key] = _unquote(line[len('msgid '):].strip())
        elif line.startswith('msgstr '):
            key = 'msgstr'
            cur[key] = _unquote(line[len('msgstr '):].strip())
        elif key is not None and line.startswith('"'):
            cur[key] = cur.get(key, '') + _unquote(line)
    flush()
    return entries


def po2ymo(infile, outfile):
    units = {}
    for context, source, target in parse_po(infile):
        if context:
            source = context + '\x04' + source
        h = fnv1a_32(source.encode('utf-16-le'))
        units[h] = target.encode('utf-16-le') + b'\x00\x00'

    outfile.write(len(units).to_bytes(2, 'little'))
    offset = 2 + len(units) * (4 + 2)
    for h in units:
        outfile.write(h.to_bytes(4, 'little'))
        outfile.write(offset.to_bytes(2, 'little'))
        offset += len(units[h])

    for data in units.values():
        outfile.write(data)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        print('usage: po2ymo_stdlib.py <infile> <outfile>')
        sys.exit(1)
    with open(sys.argv[2], 'wb') as outfile:
        po2ymo(sys.argv[1], outfile)
