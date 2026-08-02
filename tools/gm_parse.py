# SPDX-FileCopyrightText: 2026 Braden Atzert
# SPDX-License-Identifier: AGPL-3.0-or-later
"""
GameMaker data.win chunk parser.

Extracts the asset name tables (objects, scripts, functions, variables, rooms,
sprites) from a YYC-compiled data.win. YYC strips the CODE chunk bytecode, but
STRG/OBJT/SCPT/FUNC/VARI still carry the real internal names, which is exactly
what the mod needs in order to resolve runtime instances by name.
"""

import json
import struct
import sys
from pathlib import Path


class Reader:
    def __init__(self, buf):
        self.buf = buf
        self.pos = 0

    def u32(self):
        v = struct.unpack_from("<I", self.buf, self.pos)[0]
        self.pos += 4
        return v

    def i32(self):
        v = struct.unpack_from("<i", self.buf, self.pos)[0]
        self.pos += 4
        return v

    def name4(self):
        v = self.buf[self.pos:self.pos + 4].decode("ascii", "replace")
        self.pos += 4
        return v


def read_chunks(buf):
    """Return {chunk_name: (start_offset, size)} for the top-level FORM."""
    r = Reader(buf)
    form = r.name4()
    if form != "FORM":
        raise ValueError(f"not a GameMaker data file (magic={form!r})")
    total = r.u32()
    end = min(r.pos + total, len(buf))

    chunks = {}
    while r.pos + 8 <= end:
        cname = r.name4()
        csize = r.u32()
        chunks[cname] = (r.pos, csize)
        r.pos += csize
        # Chunks are 4-byte aligned in some versions; tolerate either.
        if r.pos % 4:
            r.pos += 4 - (r.pos % 4)
    return chunks


def read_pointer_list(buf, start):
    """Standard GM list: count, then `count` absolute file offsets."""
    r = Reader(buf)
    r.pos = start
    count = r.u32()
    if count > 2_000_000:
        raise ValueError(f"implausible list count {count} at {start}")
    return [r.u32() for _ in range(count)]


def read_strings(buf, chunks):
    """
    Parse STRG. Returns (by_data_offset, ordered_list).

    Asset structs reference strings by the offset of the *character data*,
    i.e. 4 bytes past the length prefix, so that's what we key on.
    """
    if "STRG" not in chunks:
        return {}, []
    start, size = chunks["STRG"]
    offsets = read_pointer_list(buf, start)

    by_off = {}
    ordered = []
    for off in offsets:
        if off + 4 > len(buf):
            continue
        length = struct.unpack_from("<I", buf, off)[0]
        if length > 1_000_000 or off + 4 + length > len(buf):
            continue
        s = buf[off + 4:off + 4 + length].decode("utf-8", "replace")
        by_off[off + 4] = s
        ordered.append(s)
    return by_off, ordered


def named_list(buf, chunks, chunk, strings):
    """
    Read a chunk whose entries begin with a string pointer (OBJT, SCPT, ...).
    Returns [{index, name, offset}].
    """
    if chunk not in chunks:
        return []
    start, size = chunks[chunk]
    try:
        offsets = read_pointer_list(buf, start)
    except ValueError:
        return []

    out = []
    for i, off in enumerate(offsets):
        if off + 4 > len(buf):
            continue
        name_ptr = struct.unpack_from("<I", buf, off)[0]
        out.append({
            "index": i,
            "name": strings.get(name_ptr, f"<unresolved:{name_ptr}>"),
            "offset": off,
        })
    return out


def read_func_or_vari(buf, chunks, chunk, strings):
    """
    FUNC/VARI are flat arrays of {name_ptr, occurrence_count, first_address},
    not pointer lists. FUNC may be wrapped in a sub-list on newer versions, so
    fall back to a flat scan and keep whatever resolves to a real string.
    """
    if chunk not in chunks:
        return []
    start, size = chunks[chunk]
    end = start + size

    out = []
    seen = set()
    pos = start
    while pos + 12 <= end:
        name_ptr, occurrences, first_addr = struct.unpack_from("<III", buf, pos)
        if name_ptr in strings:
            nm = strings[name_ptr]
            if nm not in seen:
                seen.add(nm)
                out.append({"name": nm, "occurrences": occurrences})
            pos += 12
        else:
            pos += 4
    return out


def main():
    path = Path(sys.argv[1])
    outdir = Path(sys.argv[2])
    outdir.mkdir(parents=True, exist_ok=True)

    buf = path.read_bytes()
    print(f"loaded {path} ({len(buf):,} bytes)")

    chunks = read_chunks(buf)
    print(f"chunks: {', '.join(f'{k}({v[1]:,})' for k, v in chunks.items())}")

    strings, ordered = read_strings(buf, chunks)
    print(f"strings: {len(ordered):,}")

    result = {"chunks": {k: {"offset": v[0], "size": v[1]} for k, v in chunks.items()}}
    for chunk, key in (("OBJT", "objects"), ("SCPT", "scripts"), ("ROOM", "rooms"),
                       ("SPRT", "sprites"), ("SOND", "sounds")):
        entries = named_list(buf, chunks, chunk, strings)
        result[key] = [e["name"] for e in entries]
        print(f"{key}: {len(entries):,}")

    for chunk, key in (("FUNC", "functions"), ("VARI", "variables")):
        entries = read_func_or_vari(buf, chunks, chunk, strings)
        result[key] = [e["name"] for e in entries]
        print(f"{key}: {len(entries):,}")

    (outdir / "assets.json").write_text(json.dumps(result, indent=1), encoding="utf-8")
    (outdir / "strings.txt").write_text("\n".join(ordered), encoding="utf-8")
    print(f"wrote {outdir/'assets.json'} and {outdir/'strings.txt'}")


if __name__ == "__main__":
    main()
