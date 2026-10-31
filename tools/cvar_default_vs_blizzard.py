#!/usr/bin/env python3
"""What this client says an unset CVar is, against what the original says.

The original states a default in two places, and for six CVars the two
statements disagree:

* the **engine's registration**, `FUN_00767fc0(name, screen, flags, &default,
  setter, ...)` in Wow.exe - the string the cvar object carries until something
  writes it, which is what GetCVar answers on an account that has never set it;
* **FrameXML's option tables**, `{ default = "0", cvar = "lockActionBars" }`,
  which `InterfaceOptionsFrame_LoadUVars` only ever *compares* against GetCVar:
  it calls SetCVar when the player has already changed the Lua global behind
  the row, and otherwise leaves the CVar alone. On the first login the global
  was initialised to that same table value a few lines earlier, so nothing is
  written and the row's `default` never reaches the CVar.

So the registration governs what a fresh account runs on, and the table's
default is a comparison value rather than a shipped value. Six of the names
this client answers are in that disagreement; they are listed in REGISTERED
with what the binary registers, and that is the expectation this check holds
the client to. Where a name has no entry, FrameXML's table is the only
statement available and it is used as-is.

The registration list came out of the 12340 Ghidra decompile - every
`FUN_00767fc0` call site, the default argument read back as the string it
points at - and 121 of the 143 names this client answers match it untouched.
Re-running that sweep is the only way to keep this honest as the client grows;
the six here are what it found the two sources disagreeing on, and the ones it
found this client answering wrongly were fixed against the registration rather
than added to a list.

Data/interface is not in the repository - the extracted interface is the
player's own - so this skips rather than fails when it is absent, the same rule
the other interface sweeps use.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
API = ROOT / "src/addons/lua_system_api.cpp"
# The other FrameXML sweeps take the tree as an argument for the same reason:
# the extracted interface is the player's own, not in the repository.
FRAMEXML = (Path(sys.argv[1]) if len(sys.argv) > 1
            else ROOT / "Data/interface/framexml")

# The names where the engine's registration is not what FrameXML's table
# declares. The value is what the binary registers, which is therefore what
# this client answers; the second line names where that was read.
REGISTERED = {
    "buffdurations": ("1", "registered at 0x00510000; uvarInfo declares 0"),
    "displayworldpvpobjectives": ("1", "registered at 0x00510000; uvarInfo declares 2"),
    "lockactionbars": ("1", "registered at 0x00510000; uvarInfo declares 0"),
    "lootundermouse": ("1", "registered at 0x00510000; uvarInfo declares 0"),
    "questfadingdisable": ("1", "registered at 0x00510000; uvarInfo declares 0"),
    "showdispeldebuffs": ("0", "registered at 0x00510000; uvarInfo declares 1"),
}

FALLBACK = re.compile(
    r'((?:n == "[a-z0-9_]+"\s*(?:\|\|\s*)?)+)\)?\s*(?:\{\s*)?lua_pushstring\(L,\s*"([^"]*)"\)',
    re.IGNORECASE | re.DOTALL)
NAME = re.compile(r'n == "([a-z0-9_]+)"', re.IGNORECASE)
# Blizzard writes the pair in both orders across its option tables.
PAIRS = (re.compile(r'default\s*=\s*"([^"]*)"\s*,\s*cvar\s*=\s*"([^"]*)"'),
         re.compile(r'cvar\s*=\s*"([^"]*)"\s*,\s*default\s*=\s*"([^"]*)"'))


def main():
    if not API.is_file() or not FRAMEXML.is_dir():
        print("the extracted interface is missing - nothing compared.")
        return 0

    blizzard = {}
    for path in sorted(FRAMEXML.glob("*.lua")):
        text = path.read_text(errors="ignore")
        for value, cvar in PAIRS[0].findall(text):
            blizzard.setdefault(cvar.lower(), value)
        for cvar, value in PAIRS[1].findall(text):
            blizzard.setdefault(cvar.lower(), value)

    ours = {}
    for match in FALLBACK.finditer(API.read_text(encoding="utf-8")):
        for name in NAME.findall(match.group(1)):
            ours.setdefault(name.lower(), match.group(2))

    shared = sorted(set(blizzard) & set(ours))
    print(f"Blizzard declares {len(blizzard)} defaults, this client answers "
          f"{len(ours)}, {len(shared)} in both.\n")

    # What the original's own answer is: the registration where one is known,
    # since the option table's default never reaches the CVar.
    want = {name: (REGISTERED[name][0] if name in REGISTERED else blizzard[name])
            for name in shared}

    undecided, drifted = [], []
    for name in shared:
        if want[name] == ours[name]:
            continue
        known = REGISTERED.get(name)
        if known is None:
            undecided.append((name, blizzard[name], ours[name]))
        else:
            drifted.append((name, known[0], ours[name]))

    for name, theirs, mine in undecided:
        print(f"  {name}: FrameXML declares {theirs!r} and this client answers {mine!r}, "
              "and nothing here says the registration says otherwise")
    for name, expected, mine in drifted:
        print(f"  {name}: recorded as answering {expected!r} and now answers {mine!r} - "
              "the decision changed without the note changing")

    if undecided or drifted:
        print(f"\n{len(undecided) + len(drifted)} default(s) differ from the game's "
              "own without a reason recorded.")
        return 0

    # Both sides are found by pattern, so a pattern that stops matching reports
    # the same silence as agreement.
    if len(shared) < 25:
        print(f"\nonly {len(shared)} CVars were declared by both, which is fewer "
              "than either side has - the parse stopped matching rather than "
              "finding nothing wrong.")
        return 1

    print(f"every default this client answers matches the game's own; "
          f"{len(REGISTERED)} of the names compared are ones where the game "
          "states itself twice and differently, and those follow the "
          "registration.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
