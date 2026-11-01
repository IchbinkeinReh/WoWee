#!/usr/bin/env python3
"""CVars whose default can never match what FrameXML compares them against.

An unknown CVar answers "0" here, and for most of them that is right: nearly
every CVar in the interface is a boolean, and "0" is off.

The exception is the ones that hold a *name*. UpdatePaperdollStats takes
GetCVar("playerStatLeftDropdown") and compares it against five category names,
filling the column from whichever matches. "0" matches none of them, so it
wrote nothing - and wrote nothing *successfully*, running to completion with no
error and no warning. The character sheet showed two empty panels under the
model and there was nothing anywhere to say why.

That is the shape this looks for: a CVar compared against string literals, none
of which is "0", and which lua_system_api.cpp has no explicit answer for.

**The comparison is often a line away from the call.** uiparent.lua writes

    local lastTalkedToGM = GetCVar("lastTalkedToGM");
    if ( lastTalkedToGM ~= "" ) then GMChatFrame_LoadUI(); GMChatFrame:Show()

and "0" is not "", so the GM chat window opened on every login for a player no
GM had ever written to. Matching only `GetCVar("x") == "y"` on one line walked
straight past it, which is the same blindness the for-limit check had. Both the
direct form and an assignment compared later in the same function are checked
now.

Note what it does **not** report, because the first version did and every one
was a false positive: a CVar compared against "1" at all. That is a number,
and "0" is its sibling - off for a boolean, and the first of a set for
showBattlefieldMinimap, which is compared against "1" and "2" and means off at
zero. Requiring "0" itself to appear was too strict, since nothing ever
compares against the off value; it tests for the on ones and falls through.

**Half of the interface was invisible to it until 2026-11-01.** The file list
was two globs below whichever directory was named - `*.lua` and
`addons/*/*.lua` - and `FrameXML/addons` does not exist, so naming FrameXML read
135 files and closed every load-on-demand panel: the talent tree, the auction
house, the guild bank, the macro editor, the trainer. For this sweep that is the
worst half to close, because the bug it exists for is a CVar holding a *name*
rather than a boolean, and most of the panels that hold such a CVar are addons.
It asks the loader which files it opens, and either spelling of the path is
accepted. The same bug, recorded in full where it was first found, is in
framexml_for_limit_check.py's header.
"""
import re
import pathlib

# Paths resolve against the repository, not the working directory.
REPO = pathlib.Path(__file__).resolve().parent.parent

import collections
import sys

# "Data/interface" as one string, not "Data" / "interface": both sweep_guard and
# tools_run_check read this source for the input names they know, and a split
# spelling hides the missing one, so the tool got run with no interface at all
# and reported a fault ("no such directory: …") instead of a skip.
fx = (pathlib.Path(sys.argv[1]) if len(sys.argv) > 1
      else REPO / "Data/interface" / "framexml")
if not fx.is_dir():
    sys.exit(f"no such directory: {fx}")
# Either spelling of the path; loaded_files() walks from the interface folder.
if fx.name.lower() == "framexml":
    fx = fx.parent

# The files the loader opens, from the one place that knows.
#
# This had its own two globs - `*.lua` and `addons/*/*.lua` below the named
# directory - which is the half-tree bug framexml_for_limit_check.py records in
# full: named at FrameXML, `framexml/addons` does not exist, so every
# load-on-demand panel was closed. 135 files read, and the talent tree, the
# auction house, the guild bank, the macro editor and the trainer were not among
# them. That matters more here than anywhere: a CVar holding a *name* rather
# than a boolean is exactly the shape that came out of a settings panel, and
# half the settings panels live in an addon.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from framexml_source import loaded_files, without_comments  # noqa: E402

# CVars the client answers deliberately, however that answer is spelled.
sysapi = (REPO / "src/addons/lua_system_api.cpp").read_text(encoding="utf-8")
# Folded, because lua_GetCVar folds: the client's CVar names are not
# case-sensitive and the interface spells "uiscale" and "uiScale" both ways.
known = {m.lower() for m in re.findall(r'n == "(\w+)"', sysapi)}

compared = collections.defaultdict(set)
files = sorted(p for p in loaded_files(fx) if p.suffix.lower() == ".lua")
for f in files:
    # Comments blanked and strings left alone, which is the other half of the
    # shared rule: this sweep reads the literal a comparison runs against, so
    # the string is its subject and only the comment is noise. It had neither
    # rule, so a commented-out comparison counted as a live one - a disabled
    # settings row reading as a default that can never match.
    t = without_comments(f.read_text(encoding="utf-8", errors="ignore"))
    for cv, val in re.findall(r'GetCVar\(\s*"(\w+)"\s*\)\s*==\s*"([^"]*)"', t):
        compared[cv].add(val)
    for val, cv in re.findall(r'"([^"]*)"\s*==\s*GetCVar\(\s*"(\w+)"\s*\)', t):
        compared[cv].add(val)
    # `local x = GetCVar("y")` then `x ~= ""` or `x == "z"` further down.
    lines = t.splitlines()
    pending = {}
    for i, line in enumerate(lines):
        if re.match(r'\s*(?:local\s+)?function\b', line):
            pending.clear()
        for var, cv in re.findall(r'\b(\w+)\s*=\s*GetCVar\(\s*"(\w+)"\s*\)', line):
            pending[var] = cv
        for var, val in re.findall(r'\b(\w+)\s*[=~]=\s*"([^"]*)"', line):
            if var in pending:
                compared[pending[var]].add(val)

faults = {cv: vals for cv, vals in compared.items()
          if cv.lower() not in known and "0" not in vals and "1" not in vals}

print(f"scanned {len(files)} files, {len(compared)} CVars compared against a literal")
# The count on both branches: "none whose default is unreachable" is prose, and
# a sweep whose clean branch has no number in it cannot be graded against a
# ceiling, so it never was one.
if faults:
    print(f"\n{len(faults)} whose default \"0\" can never match:\n")
    for cv, vals in sorted(faults.items()):
        print(f"  {cv:32} compared against {sorted(vals)}")
    print("\nEach needs an explicit default in lua_GetCVar, or the code reading")
    print("it silently takes no branch at all.")
else:
    print("\n0 whose default \"0\" can never match")
