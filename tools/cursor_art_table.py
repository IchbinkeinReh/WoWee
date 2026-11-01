#!/usr/bin/env python3
"""Check include/ui/cursor_art.hpp against the cursor list in the real client.

The table in that header is a transcription of one in the executable: an array
of fifty-four pointers to art names, slot for slot, where the slot number *is*
the cursor kind SetCursor's names resolve to.  A transcription error there is
invisible at the chair - a row copied one along still names a cursor, and it is
a cursor that shows on screen.  So the comparison is run against the binary
itself, and the array is found by its shape rather than by a pinned address: a
null pointer, then 'Point', then fifty-one more, then 'UnablevehichleCursor',
then null again.

Three more things are checked while the file is open, because each of them is a
way the table can be right about the binary and wrong about this client:

  * the base half against the "can't" half - kind n+26 has to be kind n's art
    with `Unable` in front of it, and its name the same name with `_ERROR` in
    place of `_CURSOR`.  Both halves were read off the same array, so an entry
    dropped in one of them breaks this at once;
  * the comment beside each art row, which is what says which name reaches which
    entry.  Those comments were written by hand and drift the way the array was
    read by hand;
  * that every named art has a file in the interface this client loads from.  The
    names come out of build 12340, and a cursor whose .blp is not installed
    paints nothing while the name still reads fine.

Not wired into sweep_guard.py: it needs the reference executable, which is not
somewhere a build machine has.  Run it by hand against a copy:

    tools/cursor_art_table.py "<Wow.exe>" [--interface <interface dir>]
"""
import argparse
import pathlib
import re
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
HEADER = ROOT / "include" / "ui" / "cursor_art.hpp"

# SetCursor's own name table, and the art array's first and last entries.  The
# names in the binary's list are lower case on disk, so the file check folds.
FIRST_ART, LAST_ART = "Point", "UnablevehichleCursor"


def sections(path):
    """(vma, size, raw offset) per section, plus the image base."""
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    # SizeOfOptionalHeader is the word at PE+0x14; PE+0x10 is SizeOfInitialized
    # Data, which is zero in an exe and sends the section table to the start of
    # the optional header when the two are confused.
    optSize = struct.unpack_from("<H", data, pe + 0x14)[0]
    imageBase = struct.unpack_from("<I", data, pe + 0x34)[0]
    out = []
    for i in range(nsec):
        off = pe + 0x18 + optSize + i * 0x28
        name = data[off:off + 8].rstrip(b"\0").decode("latin-1")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, off + 8)
        out.append((imageBase + vaddr, min(vsize, rawsize), rawptr, name))
    return data, imageBase, out


def findArtArray(data, sectionsHere):
    """The art-name pointer array, located by its shape.

    A null slot, then 'Point', then fifty-one more names ending on
    'UnablevehichleCursor', then a null again.  Every section is scanned rather
    than the one the array is expected in, so a shift between builds costs a
    search and not a wrong answer - the shape is what is checked, and a false
    positive would have to carry the same fifty-four names.
    """
    def cstr(vma):
        for vaddr, size, rawptr, _ in sectionsHere:
            if vaddr <= vma < vaddr + size:
                off = rawptr + (vma - vaddr)
                end = data.find(b"\0", off, off + 80)
                return data[off:end].decode("latin-1") if end >= 0 else None
        return None

    for vaddr, size, rawptr, _ in sectionsHere:
        for off in range(rawptr, rawptr + size - 4 * 54, 4):
            slot0 = struct.unpack_from("<I", data, off)[0]
            if slot0 != 0:
                continue
            if cstr(struct.unpack_from("<I", data, off + 4)[0]) != FIRST_ART:
                continue
            names, good = [], True
            for i in range(1, 53):
                ptr = struct.unpack_from("<I", data, off + i * 4)[0]
                stem = cstr(ptr) if ptr else None
                if stem is None:
                    good = False
                    break
                names.append((i, stem))
            if not good or len(names) != 52 or names[-1][1] != LAST_ART:
                continue
            if struct.unpack_from("<I", data, off + 53 * 4)[0] != 0:
                continue
            # vaddr already carries the image base; only the byte offset inside
            # the section is added here.
            return vaddr + (off - rawptr), names
    return None, None


def headerTables(path):
    """The art rows and the name rows of the header, with the row comments."""
    src = path.read_text(encoding="utf-8")

    def block(marker):
        start = src.index(marker)
        return src[start:src.index("};", start)]

    art = re.findall(r'"((?:[^"\\]|\\.)*)"', block("kArt[]"))
    names = re.findall(r'"([A-Z_]+)"', block("kNames[]"))
    comments = re.findall(r'"((?:[^"\\]|\\.)*)",\s*// *(\d+) +([A-Z][A-Z_]*)',
                          block("kArt[]"))
    return art, names, comments


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe", help="the reference executable (build 12340)")
    ap.add_argument("--header", default=str(HEADER))
    ap.add_argument("--interface", default=None,
                    help="the installed interface dir, to check the art is there")
    args = ap.parse_args()

    data, _, secs = sections(pathlib.Path(args.exe))
    where, binaryArt = findArtArray(data, secs)
    if binaryArt is None:
        print(f"{args.exe}: no cursor art array found - not build 12340?")
        return 1
    print(f"art array at {hex(where)}, {len(binaryArt)} names "
          f"({binaryArt[0][1]} .. {binaryArt[-1][1]})")

    art, names, comments = headerTables(pathlib.Path(args.header))
    problems = 0

    if len(art) != 54:
        print(f"  {len(art)} art entries here, expected 54 (the array's 54 slots)")
        problems += 1
    for kind, stem in binaryArt:
        here = art[kind] if kind < len(art) else None
        if here != stem:
            print(f"  kind {kind}: header says {here!r}, the binary has {stem!r}")
            problems += 1
    for slot in (0, 53):
        if slot < len(art) and art[slot]:
            print(f"  kind {slot} holds {art[slot]!r}; the binary leaves that slot empty")
            problems += 1

    if len(names) != 52:
        print(f"  {len(names)} names here, expected 52")
        problems += 1
    for i in range(min(26, len(names))):
        base, error = names[i], names[i + 26] if i + 26 < len(names) else None
        if error != base.replace("_CURSOR", "_ERROR_CURSOR"):
            print(f"  name {i}: {base} pairs with {error}, not "
                  f"{base.replace('_CURSOR', '_ERROR_CURSOR')}")
            problems += 1
        stem = art[i + 1] if i + 1 < len(art) else None
        unable = art[i + 27] if i + 27 < len(art) else None
        if unable != "Unable" + (stem or ""):
            print(f"  kind {i+1} art {stem!r} pairs with {unable!r}, not "
                  f"'Unable{stem}'")
            problems += 1

    # The unlabelled rows - kind 0 and the custom slot - are not in `comments`, so
    # the i-th comment is the row whose kind is i+1.
    for i, (stem, num, label) in enumerate(comments):
        kind = i + 1
        if int(num) != kind:
            print(f"  art row {kind} ({stem!r}) is commented as kind {num}")
            problems += 1
        if kind <= len(names) and label != names[kind - 1]:
            print(f"  art row {kind} ({stem!r}) is commented {label}, the name table "
                  f"reaches it as {names[kind-1]}")
            problems += 1

    if args.interface:
        cursorDir = pathlib.Path(args.interface) / "cursor"
        if not cursorDir.is_dir():
            print(f"  {cursorDir}: no cursor folder there")
            problems += 1
        else:
            have = {f.name.lower() for f in cursorDir.iterdir()}
            missing = [a for a in art if a and a.lower() + ".blp" not in have]
            print(f"  {len([a for a in art if a])} named, {len(missing)} without a file "
                  f"in {cursorDir}: {' '.join(missing)}")
            problems += len(missing)

    print("the table and the binary agree" if not problems else f"{problems} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
