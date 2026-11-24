#!/usr/bin/env python3
"""Textures a descriptor set is written with once, and then let go of.

    tools/fallback_texture_check.py

A shadow pass binds one set for everything it casts solid (createShadowParamsSet),
written once, at start-up, with the renderer's white texture. A renderer that lets
that texture go when the world changes and makes another leaves the set with a view
of one that is gone: what is read through it is whatever the memory holds next.

CharacterRenderer::clear() did. After some map changes every opaque part of every
character cast nothing at all - the hair and the cut-out items, which bring a set of
their own, were the whole of a unit's shadow (T9, 2026-11-24).

Counts the texture members named in a createShadowParamsSet call that are reset
anywhere but where the renderer itself goes.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
MEMBER = re.compile(r"(\w+)->getImageView\(\)")
FUNCTION = re.compile(r"^(?=\w)[^\n(]*?\b(\w+)::(~?\w+)\(", re.M)
ENDS = {"shutdown", "destroy"}


def main():
    sources = sorted((ROOT / "src" / "rendering").glob("*.cpp"))
    if not sources:
        print("Found no sources. The zero below means the scan broke.")
        return 1
    by_class = {}
    for path in sources:
        text = path.read_text(encoding="utf-8", errors="ignore")
        heads = [(m.start(), m.group(1), m.group(2)) for m in FUNCTION.finditer(text)]
        by_class[path] = (text, heads)

    def owner(heads, at):
        last = ("", "")
        for start, cls, fn in heads:
            if start > at:
                break
            last = (cls, fn)
        return last

    members = set()
    for path, (text, heads) in by_class.items():
        for call in re.finditer(r"createShadowParamsSet\(", text):
            m = MEMBER.search(text, call.start(), call.start() + 400)
            if m:
                members.add((owner(heads, call.start())[0], m.group(1)))

    found = []
    for path, (text, heads) in by_class.items():
        for m in re.finditer(r"\b(\w+)\.reset\(\)", text):
            cls, fn = owner(heads, m.start())
            if (cls, m.group(1)) in members and fn not in ENDS and not fn.startswith("~"):
                line = text.count("\n", 0, m.start()) + 1
                found.append(f"{path.relative_to(ROOT).as_posix()}:{line}  {m.group(1)} reset in {cls}::{fn}")

    print(f"{len(members)} texture(s) a set is written with once")
    print(f"{len(found)} of them let go of while the set is kept:")
    for f in found:
        print("  " + f)
    if len(members) < 4:
        print("Fewer than the four shadow passes' - the scan broke.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
