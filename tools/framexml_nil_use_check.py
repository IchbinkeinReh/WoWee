#!/usr/bin/env python3
"""Calls to functions that do not exist, used where nil raises immediately.

The previous sweep only caught `local v = Missing()` followed by a use of v.
A call used straight in the expression is the same fault with no variable in
between, and concatenation is a third raising context that neither covered:
"text"..nil is an error in Lua, not the empty string.

    tools/framexml_nil_use_check.py ["<the extracted interface>"]

**XML is scanned as well as Lua, and that is where its one finding lives.**
minimap.xml's minimap tooltip OnUpdate reads

    GameTooltip:SetText(MOVIE_RECORDING_RECORDING..MovieRecording_GetTime());

and MovieRecording_GetTime is a name nothing answers. It is reported and not
fixed: the frame that runs that body is `MiniMapRecordingButton`, declared
`hidden="true"`, and the only Show() anywhere for it is behind
`MovieRecording_IsRecording()`, which this client answers false, so the
indicator never appears, its OnEnter never sets MINIMAP_RECORDING_INDICATOR_ON,
and the body never runs. It is also one member of a family the executable does
not contain at all - the byte stem "MovieRecording" does not occur in Wow.exe -
which is the same adjudication sweep_guard records for
MovieRecording_IsCodecSupported and MovieRecording_GetProgress. A stub would
have made the count go to zero without changing what any player sees, which is
how a sweep gets talked out of having eyes.

Reading only `.lua` is what the header used to do, and this is the second sweep
in a row whose blindness was half the tree: a FrameXML `.xml` is not markup to
the client, it is Lua - every `<OnLoad>`, `<OnEvent>`, `<OnUpdate>` and
`<OnClick>` body is a chunk the loader installs and runs.
"""
import re, pathlib, collections, sys

# Paths are resolved against the repository, not the working directory:
# run from build/ these globs matched nothing and the report came out
# clean from an empty scan, which is the exact shape of a check that
# cannot fail.
REPO = pathlib.Path(__file__).resolve().parent.parent

# One literal, so sweep_guard can see this sweep needs the interface and
# skip it where there is none. A path in separate components is invisible
# to that check, and the sweep then runs on CI, finds nothing, and is
# failed for reporting nothing.
root = (pathlib.Path(sys.argv[1]) if len(sys.argv) > 1
        else REPO / "Data/interface")
if not root.is_dir():
    # The files scanned are the interface's own, so with no interface there is
    # nothing to scan and the report comes out clean - the same shape the
    # comment above this path was written to warn about.
    print(f"No interface at {root} - name the unpacked interface on the "
          "command line; there are no bodies to scan here.")
    sys.exit(2)
# Either spelling of the path means the same interface to a reader; loaded_files
# walks the .toc from the folder above FrameXML, so the step has to happen.
if root.name.lower() == "framexml":
    root = root.parent
# Which files the client loads, from the one place that knows, and both kinds.
#
# This used to glob `framexml/*.lua` and `addons/*/*.lua` under the named
# directory. For `.lua` that was nearly right - it found every loaded Lua file -
# but it read five files the client never opens (focusframe.lua and its
# friends, on disk and reached by no Include, whose faults get reported as work)
# and it read not one `.xml`. The XML is not markup to this client: the loader
# installs each script body as a Lua chunk, so the bodies are exactly as live
# as the `.lua` files, and 162 of the 343 files the loader opens are XML.
#
# Measured across the change: 187 files read before, 343 after; 6984 known names
# before, 6955 after (the phantom files were contributing definitions nothing
# calls); 0 findings before, 1 after - the one described in the header, which is
# unreachable and pinned in sweep_guard with the chain that makes it so.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from framexml_source import loaded_files  # noqa: E402

files = sorted(p for p in loaded_files(root)
               if p.suffix.lower() in (".lua", ".xml"))
texts = {f: f.read_text(encoding="utf-8", errors="ignore") for f in files}

csrc = "\n".join(p.read_text(errors="ignore")
                 for p in (REPO / "src" / "addons").glob("*.cpp"))
c_bindings = set(re.findall(r'\{(?:\.\w+\s*=\s*)?"(\w+)",', csrc)) | \
             set(re.findall(r'static int lua_(\w+)', csrc))
lua_defined = set()
for t in texts.values():
    lua_defined |= set(re.findall(r'\bfunction\s+([A-Za-z_]\w*)\s*\(', t))
    lua_defined |= set(re.findall(r'\b([A-Za-z_]\w*)\s*=\s*function\s*\(', t))
# Names the bootstrap defines as counting stubs answer 0, not nil.
counting = set(re.findall(r"'([A-Za-z_]\w*)'",
    re.search(r'local counting = \{(.*?)\n\s*"\}', (REPO / 'src' / 'addons' / 'lua_engine.cpp').read_text(encoding="utf-8"), re.S).group(1)))
known = c_bindings | lua_defined | counting

CALL = r'(?<![:.\w])([A-Z][A-Za-z0-9_]{3,})\s*\([^()]*\)'
patterns = [
    ("compared as a number", re.compile(CALL + r'\s*(?:[<>]=?)')),
    ("in arithmetic",        re.compile(CALL + r'\s*[-+*/]\s*[\w("\']')),
    # The call after a `..` needs its own pattern. CALL starts with a
    # (?<![:.\w]) meant to skip obj.Method(), and after a concatenation the
    # character before the name IS a dot - so the operand of every `.."x"..`
    # was rejected by the very pattern written to find it. That is how
    # _G["FACIAL_HAIR_"..GetFacialHairCustomization()] went unreported while
    # raising in BarberShop_OnLoad.
    ("concatenated", re.compile(r'\.\.\s*([A-Z][A-Za-z0-9_]{3,})\s*\([^()]*\)'
                                r'|' + CALL + r'\s*\.\.')),
]

# The same fault with a variable in between: `local x = Missing()` and then x
# used somewhere nil raises, a few lines down. Concatenation belongs here as
# much as arithmetic - the barber shop reads
#     local hairCustomization = GetHairCustomization();
#     ... _G["HAIR_"..hairCustomization.."_STYLE"]
# and it was the concatenation, not any arithmetic, that took the addon down.
ASSIGN = re.compile(r'\blocal\s+([A-Za-z_]\w*)\s*=\s*'
                    r'(?<![:.])([A-Z][A-Za-z0-9_]{3,})\s*\(')

hits = collections.defaultdict(list)
for f, t in texts.items():
    lines = t.splitlines()
    for i, line in enumerate(lines, 1):
        if line.lstrip().startswith("--"):
            continue
        for label, pat in patterns:
            for m in pat.finditer(line):
                fn = next(g for g in m.groups() if g)
                if fn in known:
                    continue
                hits[fn].append((label, f"{f.name}:{i}: {line.strip()[:88]}"))

        m = ASSIGN.search(line)
        if not m or m.group(2) in known:
            continue
        var, fn = m.group(1), m.group(2)
        # Both sides of every operator. Only the left was checked, so
        # `if ( GetMoney() >= cost )` - the variable on the right of a
        # comparison - went unseen, which is how the bank frame's nil slot
        # cost stayed hidden while the sweep reported clean.
        use = re.compile(r'\b' + re.escape(var) + r'\s*(?:[<>]=?|[-+*/]|\.\.)'
                         r'|(?:[<>]=?|[-+*/]|\.\.)\s*\b' + re.escape(var) + r'\b')
        # A guard between the two makes the use safe, and this is the whole
        # reason the shape was left out of the contract checker before: most
        # candidates are `local x = Foo(); if ( x ) then ... x .. "y" ...`,
        # where nil never reaches anything. Without noticing the guard the
        # report is 20-odd false positives and gets ignored.
        guard = re.compile(r'\bif\s*\(?\s*(?:not\s+)?' + re.escape(var) + r'\b')
        for j, later in enumerate(lines[i:i + 20], start=i + 1):
            if later.lstrip().startswith("--"):
                continue
            if guard.search(later):
                break
            if use.search(later):
                hits[fn].append(("assigned then used",
                                 f"{f.name}:{j}: {later.strip()[:88]}"))
                break

for fn in sorted(hits):
    labels = {l for l, _ in hits[fn]}
    print(f"\n### {fn}  [{', '.join(sorted(labels))}]")
    for _, h in hits[fn][:3]:
        print("   ", h)
# What it looked at, before what it found. Pinned at zero and reporting
# only findings, this reads the same whether nothing is wrong or the
# list of known names stopped being built.
if not known:
    print("Found no known bindings at all, which cannot be right - the "
          "registration parse broke rather than every binding vanishing.")
    raise SystemExit(1)
# What it read, in the same report as what it found: "0 findings" from a scan of
# 343 files and "0 findings" from a scan of none look the same on the terminal.
print(f"scanned {len(files)} file(s) the loader opens - "
      f"{sum(1 for f in files if f.suffix.lower() == '.lua')} .lua, "
      f"{sum(1 for f in files if f.suffix.lower() == '.xml')} .xml")
print(f"\n{len(known)} name(s) the interface may call, checked against "
      f"what this client answers")
print(f"\n{len(hits)} missing functions used where nil raises")
