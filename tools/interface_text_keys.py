#!/usr/bin/env python3
"""GlobalStrings keys this client asks the interface for that it has no line for.

What this client prints for its own lines is the interface's wording: a key and
an English literal, and the literal is used whenever the interface has no
global of that name (interfaceString, src/game/interface_text.cpp).  That
fallback is why a made-up key survives: the line prints, in English, on a
German client, and nothing else about it looks wrong.

The interface this client installs is a newer FrameXML than the build whose
behaviour it reproduces, and GlobalStrings gets re-spelled between expansions.
3.3.5's ERR_PET_SPELL_NOPATH is PET_SPELL_NOPATH in this one, and
ERR_REMOVE_FROM_PVP_QUEUE_FACTION_CHANGE has grown a _NONE suffix.  Those are
recoverable - ask for both names and the interface's own language comes back
(interfaceStringAny).  A line the newer set dropped for good is not: its wording
lives in GlobalStrings.dbc of the old locale, which is not in the extracted data
set here, so the English literal is all there is.  Both kinds are reported, and
which kind a miss is is worth knowing before deciding what to do about it.

The interface is installed data, not repository content, and it has sat in more
than one place: name its directory on the command line, or leave it out and the
sweep looks for it where a running client keeps it.  A run with no interface
fails rather than reporting zero, because "0 of 0" and "nothing is missing" are
the same line otherwise.

Keys built at run time - "CHAT_" .. word .. "_NOTICE", SPELL_FAILED_ plus a
name out of a table - are skipped: the sweep cannot know what name comes out,
and their coverage is a different question than whether a fixed name exists.

    tools/interface_text_keys.py [<interface dir>] [<decompile's strings.tsv>]

With the second argument the sweep also says which of the misses the reference
client really did have (its binary names the key when its C code asks for it),
which is what separates a re-spelling from a line that was invented here.
"""

import argparse
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# The calls a key is handed to.  `text` and `fmt` are the parameter names the
# format helpers in include/game/interface_text.hpp and include/game/chat_log
# .hpp give their text source, so a key reaches the lookup through them too.
LOOKUPS = ("interfaceString", "interfaceStringAny", "interfaceFormat",
           "gameError", "globalStringText", "text", "fmt")
CALL = re.compile(r'\b(?:' + "|".join(LOOKUPS) + r')\s*\(')
# A literal that runs straight into a `+` is one half of a built key.
KEY = re.compile(r'"([A-Z][A-Z0-9_]{2,})"(\s*\+)?')


def code_only(src):
    """`src` with its comments blanked: a key named in prose is not a use."""
    src = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def arguments(src, start):
    """The text between a call's own parentheses, `start` at its `(`."""
    depth = 0
    for i in range(start, len(src)):
        if src[i] == "(":
            depth += 1
        elif src[i] == ")":
            depth -= 1
            if depth == 0:
                return src[start + 1:i]
    return src[start + 1:]


def asks(repo):
    """Every place this tree asks the interface for a line, as the names it
    offers in the order it offers them."""
    found = {}
    for base in (repo / "src", repo / "include"):
        for path in sorted(base.rglob("*.cpp")) + sorted(base.rglob("*.hpp")):
            src = code_only(path.read_text(encoding="utf-8", errors="replace"))
            rel = path.relative_to(repo).as_posix()
            for m in CALL.finditer(src):
                # globalString(L_, "GetCursorInfo") names a binding rather
                # than a line, which is why that spelling is not in LOOKUPS.
                # A literal running into a `+`, or ending in `_`, is one half
                # of a key built at run time: "CHAT_" .. name .. "_GET".
                names = [k.group(1) for k in KEY.finditer(arguments(src, m.end() - 1))
                         if not k.group(2) and not k.group(1).endswith("_")]
                if names:
                    found.setdefault(tuple(names), set()).add(rel)
    return found


def interface_globals(root):
    """Names the installed interface assigns as globals, which is what a
    GlobalStrings line is by the time the client reads it."""
    names = set()
    for path in root.rglob("*.lua"):
        src = path.read_text(encoding="utf-8", errors="replace")
        for m in re.finditer(r"(?m)^\s*([A-Z][A-Z0-9_]{2,})\s*=", src):
            names.add(m.group(1))
        for m in re.finditer(r'\["([A-Z][A-Z0-9_]{2,})"\]\s*=', src):
            names.add(m.group(1))
    return names


def binary_names(tsv):
    """Upper-case names the reference client's binary carries, which is how a
    key its C code asked for shows up without the dbc."""
    out = set()
    for line in Path(tsv).read_text(encoding="utf-8", errors="replace").splitlines():
        parts = line.split("\t")
        if len(parts) >= 2 and re.fullmatch(r"[A-Z][A-Z0-9_]{2,}", parts[1]):
            out.add(parts[1])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("interface", nargs="?", help="the extracted interface tree")
    ap.add_argument("strings", nargs="?", help="the decompile's strings.tsv")
    ap.add_argument("--all", action="store_true", help="list every key, not just the misses")
    args = ap.parse_args()

    interface = Path(args.interface) if args.interface else ROOT / "Data/interface"
    if not interface.is_dir():
        # Where a running client keeps the interface it extracted.
        appdata = os.environ.get("LOCALAPPDATA")
        if appdata:
            installed = Path(appdata) / "WoWee/Data/expansions"
            hits = sorted(installed.glob("*/interface")) if installed.is_dir() else []
            if hits:
                interface = hits[0]
    if not interface.is_dir():
        print("no interface to read: name its directory on the command line, "
              "and until then this sweep has seen nothing and its silence means "
              "nothing")
        return 1

    offered = asks(ROOT)
    have = interface_globals(interface)
    # One ask is satisfied by any name it offers: interfaceStringAny tries
    # them in order, so a chain whose first name this interface dropped still
    # reads in its language.
    missing = {c: v for c, v in offered.items() if not any(n in have for n in c)}

    names = binary_names(args.strings) if args.strings else None
    print(f"{interface}")
    print(f"{len({n for c in offered for n in c})} names asked for, "
          f"{len(have)} globals the interface assigns")
    print(f"{len(missing)} asks the interface has no line for, of {len(offered)}")
    for chain in sorted(missing):
        where = ", ".join(sorted(missing[chain]))
        why = ""
        if names is not None:
            why = ("  (the reference client asks by this name too: a line this "
                   "interface dropped)" if chain[0] in names else
                   "  (no such name in the reference client either: this key was "
                   "made up here)")
        print(f"  {' | '.join(chain):<46} {where}{why}")
    if args.all:
        print("\nevery name asked for:")
        for name in sorted({n for c in offered for n in c}):
            print(f"  {name}{'-' if name in have else ' MISSING'}")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
