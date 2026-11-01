#!/usr/bin/env python3
"""Missing globals used as a numeric for-limit, where nil is an error.

Most missing globals are survivable. The fallback makes any unknown name
callable and it answers nil, and FrameXML checks nil constantly - so a gap
usually shows up as a panel that stays empty.

`for i = 1, GetNumSomething()` is the exception. A nil limit is not an empty
loop; it raises "'for' limit must be a number" and takes down the handler
around it. GetNumTrackingTypes was unbound, so the minimap's tracking menu was
not empty but dead, and the only visible trace was a button that did nothing.

That is what this looks for: a call whose result reaches the limit position of
a numeric `for`, where the name is neither defined by the interface nor bound
by the client.

**Two forms, and the first version only found one of them.** The direct
`for i = 1, Foo()` is easy. But the bug that prompted this is written the other
way - `local count = GetNumTrackingTypes()` and then `for id = 1, count` on a
later line - so a version matching only the direct form reported six real
faults and stayed silent about the one it was written for. Both are checked
now, the indirect one within the same function.

**`or 0` is a guard too, and the tersest one.** channelframe.lua writes
`local numMembers = GetNumVoiceSessionMembersBySessionID(...) or 0;` - a
missing function answers nil, `or` replaces it, and the loop runs zero times.
Reading only the call and not the rest of the line reported it as a crash.

**A guarded variable is not a fault.** worldmapframe.lua writes
`local n = GetNumQuestItemDrops(...)` and then `if (n and n > 0) then` before
looping - nil is handled and nothing raises. Reporting it anyway was the
indirect check's own false positive, so a variable tested in an `if` between
its assignment and its loop is dropped.

Two things it deliberately does not report:

  * names the bootstrap aliases rather than binds - `getn` is table.getn, and
    the first version named it four times
  * systems that genuinely do not exist here (LFD, Battle.net). Those frames
    are unreachable, so the raise is unreachable too. They are still listed,
    because "unreachable" is a judgement about the rest of the client and this
    script is not the place to make it - read the call site before acting.

**The argument decided half of what got read, and either way it read it.** The
first version globbed `*.lua` and `addons/*/*.lua` below whatever was named, so
naming FrameXML read the core and never opened an addon, and naming the
interface folder read the addons and never opened the core. Each run printed a
file count that looked like a scan - 135 files one way, 52 the other, against
the 181 the loader opens - and the second of those reported `select` eighteen
times as an unprovided for-limit and `HybridScrollFrame_GetOffset` once, all of
them answered by the half of the tree that had been left closed: `select` is
Lua's, and HybridScrollFrame is FrameXML's own scroll library. It now asks the
loader which files it opens, and accepts either spelling of the path.

Usage:  tools/framexml_for_limit_check.py [path to Interface]
                                          (Interface/FrameXML is accepted too,
                                           and stepped up to the folder the
                                           loader's answer starts from)
"""
import re
import sys
import pathlib

# Paths resolve against the repository, not the working directory.
REPO = pathlib.Path(__file__).resolve().parent.parent

import collections

# Spelled as one string, not `Data` / `interface`: sweep_guard's missing_input
# reads this source for the input names it knows, and a join splits the name
# across quotes so the skip is never taken and the sweep fails instead.
root = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "Data/interface"
if not root.is_dir():
    sys.exit(f"no such directory: {root}")
# Named FrameXML rather than the interface? Its parent is what the loader's own
# answer walks, so step up one.
if root.name.lower() == "framexml":
    root = root.parent

# Which files the loader opens, from the one place that knows - not from globs
# written here.
#
# This tool had its own pair of globs, `*.lua` plus `addons/*/*.lua` relative to
# whatever was named, and they only ever covered half the tree whichever way the
# argument went: named at the interface root they read the addon folders and the
# strays and never opened FrameXML itself, named at FrameXML they read it and
# never opened the addons. Both runs printed a file count that looked like a
# scan - 52 with 135 core files unopened, 135 with the addons unopened - and the
# first of those reported `select` eighteen times as an unprovided for-limit and
# `HybridScrollFrame_GetOffset` once, both of which the other half of the tree
# answers: `select` is Lua, and HybridScrollFrame is FrameXML's own scroll
# library. The loader's answer takes in both halves, refuses the GlueXML screens
# this client never opens, and does not go stale when a file is added.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from framexml_source import (INTERPRETER_GLOBALS, loaded_files,  # noqa: E402
                             without_comments_or_strings)

allsrc = [p for p in loaded_files(root) if p.suffix.lower() in (".lua", ".xml")]
lua = [p for p in allsrc if p.suffix.lower() == ".lua"]

# What the interface defines for itself.
defined = set()
for f in allsrc:
    # utf-8 named, as every other FrameXML sweep names it: the default on a
    # Windows console is the codepage, which drops the five bytes that spelling
    # leaves undefined. Measured on this install, four interface files carry
    # high bytes at all and all four decode as utf-8, so the loss is small -
    # small, not zero, and the patterns below run through comments.
    t = without_comments_or_strings(f.read_text(encoding="utf-8", errors="ignore"))
    defined |= set(re.findall(r'^\s*(?:local\s+)?function\s+([A-Za-z_][\w.]*)\s*\(', t, re.M))
    # The second form is deliberately sloppy, and measured rather than assumed.
    # It accepts every *indented* `name =` line too, which adds 1846 names to
    # this set. Five sampled: two are rows of a settings table
    # (`BaseMip = { text = "TEXTURE_DETAIL", minValue = 0, …`), two are keys of
    # an icon or colour table (`ArrowDown = {layer = "ARTWORK", …}`,
    # `AudioOptionsVoicePanelAmbienceFadeLabel = NORMAL_FONT_COLOR,`), one is an
    # ordinary assignment sitting inside a function
    # (`BGindex = PVPBattlegroundFrame.selectedBG;`). That last class is why the
    # form stays: a global assigned inside a function body is still a global, and
    # anchoring the pattern to column 0 would report those as undefined.
    #
    # The cost is the first two classes - a table key excuses its name - and it
    # is a cost this sweep pays today: `restrictedenvironment.lua:104` reads
    #
    #     select = select;
    #
    # inside RESTRICTED_FUNCTIONS_SCOPE, which puts `select` in this set for a
    # reason that has nothing to do with the loop it excuses. Forty-nine loaded
    # sites are `for i = 1, select(" ", ...) do`. That is why the interpreter's
    # own names come from the named set below rather than from whatever the
    # interface happens to mention: an accidental excuse disappears the moment
    # that file stops being loaded, and a fault would come back with no note
    # saying where it had been hiding.
    defined |= set(re.findall(r'^\s*([A-Za-z_]\w*)\s*=\s*', t, re.M))
    defined |= set(re.findall(r'\b([A-Za-z_]\w*)\s*=\s*function', t))

# What the client binds, however it is spelled - including the bootstrap's
# aliases, which are why `getn` is not a hit.
src = "\n".join(p.read_text(encoding="utf-8", errors="ignore")
                for p in (REPO / "src" / "addons").glob("*.cpp"))
provided = set(re.findall(r'\{(?:\.\w+\s*=\s*)?"(\w+)"', src))
provided |= set(re.findall(r'"(\w+)"\s*,\s*lua_', src))
provided |= set(re.findall(r'"function\s+(\w+)\s*\(', src))
provided |= set(re.findall(r'^\s*([A-Za-z_]\w*)\s*=', src, re.M))
provided |= set(re.findall(r'\{(?:\.\w+\s*=\s*)?"\w+",\s*(?:\.\w+\s*=\s*)?"(\w+)",\s*(?:\.\w+\s*=\s*)?"\w+"\}', src))
provided |= set(re.findall(r'\{(?:\.\w+\s*=\s*)?"\w+",\s*(?:\.\w+\s*=\s*)?"\w+",\s*(?:\.\w+\s*=\s*)?"(\w+)"\}', src))
# The counting table in lua_engine.cpp bootstraps a zero for every name in it,
# precisely so a nil never reaches a `for` limit. Missing it here reported
# GetLFDLockPlayerCount, GetNumRandomDungeons and GetNumQuestLogRewardFactions
# as raises when all three answer zero - the exact bug this sweep exists to
# find, reported against its own fix.
_counting = re.search(r"local counting = \{(.*?)\}", src, re.S)
if _counting:
    provided |= set(re.findall(r"'(\w+)'", _counting.group(1)))
# And what the interpreter itself installs, which no binding and no FrameXML
# file provides: `select` was reported eighteen times as an unprovided for-limit
# once the scan reached the addon half of the tree, at sites written as
# `for i = 1, select("#", ...) do` - the idiom is everywhere in FrameXML, and
# the name belongs to Lua. A limit built on it raises only if the *result* is
# nil, which select("#", ...) cannot answer.
provided |= INTERPRETER_GLOBALS

LIMIT_DIRECT = re.compile(r'\bfor\s+\w+\s*=\s*[^,]+,\s*([A-Za-z_]\w*)\s*\(')
ASSIGN = re.compile(r'\blocal\s+([A-Za-z_]\w*)\s*=\s*([A-Za-z_]\w*)\s*\(')
LIMIT_VAR = re.compile(r'\bfor\s+\w+\s*=\s*[^,]+,\s*([A-Za-z_]\w*)\s*(?:do\b|$)')

hits = collections.defaultdict(list)
for f in lua:
    # Comments and the insides of strings blanked first, from the one place that
    # has both rules. This sweep reads syntax, which is the case that module
    # says needs the strings gone too: a Lua pattern is a string full of
    # parentheses, and `gsub(s, "for i = 1, DoThing(%d)", "")` reads as a loop
    # over a call to a global nothing defines. Offsets are preserved, so every
    # line number this prints is still the line a reader finds.
    lines = without_comments_or_strings(
        f.read_text(encoding="utf-8", errors="ignore")).splitlines()
    # local name -> (function it came from, line). Cleared at each function
    # boundary so a variable cannot be matched against a call in another scope.
    pending = {}
    for i, line in enumerate(lines, 1):
        if re.match(r'\s*(?:local\s+)?function\b', line):
            pending.clear()
        for fn in LIMIT_DIRECT.findall(line):
            if re.search(r'\)\s*or\s+\S', line):
                continue
            if fn not in defined and fn not in provided:
                hits[fn].append(f"{f.name}:{i}")
        for var, fn in ASSIGN.findall(line):
            # `= Foo() or 0` has already handled the nil.
            if re.search(r'\)\s*or\s+\S', line):
                pending.pop(var, None)
                continue
            if fn not in defined and fn not in provided:
                pending[var] = (fn, i)
            else:
                pending.pop(var, None)   # reassigned from something that answers
        # An `if` mentioning the variable is the author checking it. That is
        # the whole difference between a crash and an empty list, so it clears
        # the variable rather than merely noting it.
        if re.search(r'\bif\b', line):
            for var in list(pending):
                if re.search(r'\b' + re.escape(var) + r'\b', line):
                    pending.pop(var, None)
        for var in LIMIT_VAR.findall(line):
            if var in pending:
                fn, at = pending[var]
                hits[fn].append(f"{f.name}:{at} (via {var}, looped at line {i})")

print(f"scanned {len(lua)} Lua file(s) the loader opens, "
      f"{len(allsrc)} file(s) in all")
# The count is printed whether it is zero or not. A clean branch worded only as
# prose - "no unprovided global is used as a for-limit" - cannot be graded
# against a ceiling, and a sweep the guard cannot grade is a sweep nobody
# watches: the first version of this reported six faults and stayed silent about
# the one it was written for, and nothing in the tree would have noticed.
if hits:
    print(f"\n{len(hits)} unprovided global(s) in a for-limit "
          "- each raises when reached:\n")
    for fn, where in sorted(hits.items(), key=lambda kv: -len(kv[1])):
        print(f"  {fn:32} {len(where)} site(s)")
        for w in where[:3]:
            print(f"      {w}")
    print("\nBinding one to 0 turns the raise into an empty list. Check first")
    print("whether the client has data to count - zero should be the truth,")
    print("not a way to silence the script.")
else:
    print("\n0 unprovided global(s) in a for-limit")
