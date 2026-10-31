#!/usr/bin/env python3
"""Who announces a CVar change, as what, and whether this client can hear it.

    tools/framexml_cvar_label_check.py ["<the extracted interface>"]

WHAT THE EVENT ACTUALLY CARRIES

CVAR_UPDATE's first argument is not the CVar's name and not a spelling of it. It
is SetCVar's *third argument*, handed over by the binding and nothing else: the
one site in the binary that raises the event (FUN_00514c10, the fire at
0x00514cff) tests that argument for being a string or a number - FUN_0084df60
compares the argument slot's type against 3 and 4 - and fires only when it is,
with that argument itself. A SetCVar called with two arguments changes the CVar
and tells nobody.

So the mapping from CVar to announcement is not in the binary at all - the
assumption this sweep was written to check, and which made its first run report
one orphan where there is a pile. It is in the interface, and the panels that
make a change carry it themselves:

    ["REMOVE_CHAT_DELAY"] = { default = "0", cvar = "removeChatDelay",
                              event = "REMOVE_CHAT_DELAY_TEXT" }
    self.cvar = "playerStatusText";  self.cvarLabel = "STATUS_TEXT_PLAYER";
    SetCVar("chatStyle", chatStyle, "chatStyle")

and the writer passes the field through (`SetCVar(self.cvar, value, self.event)`).
A label that appears in a comparison but in no declaration is a branch the
original client cannot take either, which is worth knowing but is not a gap.

WHAT IS LEFT, AND WHY

This client has a settings window of its own, which is not one of FrameXML's
panels and so has no `event` field to pass. For a change made there the payload
is this client's to choose, and it chooses from kNamed in lua_system_api.cpp
plus a transform for the rest - which is where the real finding is: the
transform answers BUFF_DURATIONS for buffDurations, and every listener compares
SHOW_BUFF_DURATION_TEXT. Every row of the first report below is a setting of this
window's that reached the CVar and never the frames waiting for the announcement.

WHAT IT CANNOT SEE

Whether a payload is passed by a writer this sweep cannot parse - a call built
through a local alias of the settings table, which the glue panels do. And a
comparison of arg1 against a *computed* string, such as
`event = "ENVIRONMENTAL_DAMAGE_"..type`, which no literal list can match.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
import sys as _s; _s.path.insert(0, str(ROOT / "tools"))
from framexml_source import loaded_files

# The interface root on the command line, the way every other interface sweep
# takes it; Data/interface is the fallback and is not tracked in the checkout.
XML = (Path(sys.argv[1]) if len(sys.argv) > 1
       else ROOT / "Data/interface")
API = ROOT / "src/addons/lua_system_api.cpp"
SCHEMA = ROOT / "src/ui/settings_schema.cpp"


def declared_pairs(files):
    """(cvar, payload) -> where it is declared, in every form the interface uses."""
    pairs = {}
    for path in files:
        text = path.read_text(errors="ignore")
        # A settings row names both halves, in either order, with anything else
        # in the entry around them. Not across a brace: two neighbouring entries
        # naming a cvar and an event are not a pair.
        for m in re.finditer(r'\{[^{}]*?cvar\s*=\s*"(\w+)"[^{}]*?'
                             r'event\s*=\s*"([A-Za-z]\w*)"[^{}]*?\}', text):
            pairs.setdefault((m.group(1), m.group(2)), set()).add(f"{path.name}:row")
        for m in re.finditer(r'\{[^{}]*?event\s*=\s*"([A-Za-z]\w*)"[^{}]*?'
                             r'cvar\s*=\s*"(\w+)"[^{}]*?\}', text):
            pairs.setdefault((m.group(2), m.group(1)), set()).add(f"{path.name}:row")
        # A widget that reads the announcement itself names it next to the CVar
        # it watches - usually the next line, with a comment sometimes between.
        for m in re.finditer(r'self\.cvar\s*=\s*"(\w+)"\s*;?\s*(?:--[^\n]*)?\n'
                             r'\s*self\.cvarLabel\s*=\s*"([A-Za-z]\w*)"', text):
            pairs.setdefault((m.group(1), m.group(2)),
                             set()).add(f"{path.name}:cvarLabel")
        for m in re.finditer(r'self\.cvarLabel\s*=\s*"([A-Za-z]\w*)"\s*;?\s*(?:--[^\n]*)?\n'
                             r'\s*self\.cvar\s*=\s*"(\w+)"', text):
            pairs.setdefault((m.group(2), m.group(1)),
                             set()).add(f"{path.name}:cvarLabel")
        # And a writer that has no table says the payload at the call itself.
        inner = r"(?:[^()]|\([^()]*\))*"
        for m in re.finditer(r'SetCVar\w*\(\s*"(\w+)"\s*,' + inner + r',\s*"([A-Za-z]\w*)"',
                             text):
            pairs.setdefault((m.group(1), m.group(2)), set()).add(f"{path.name}:call")
    return pairs


def compared_labels(files):
    """Literals a CVAR_UPDATE branch compares its first argument against."""
    wanted = {}
    for path in files:
        text = path.read_text(errors="ignore")
        # The branch is the stretch of text from the test that selects this
        # event to the next test that selects a different one. Stopping at the
        # first `end` or bare `else` instead - which is what this did first -
        # cuts the stretch at the first *inner* block, and the comparison in
        # TextStatusBar_OnEvent is inside two of those: the percent bar's label
        # never showed up as compared, and the kNamed entry beside it read as
        # supported by nothing.
        tests = [(m.start(), m.group(1), m.end())
                 for m in re.finditer(r'event\s*==\s*"([A-Z][A-Z0-9_]+)"', text)]
        for i, (start, name, end) in enumerate(tests):
            if name != "CVAR_UPDATE":
                continue
            stop = tests[i + 1][0] if i + 1 < len(tests) else len(text)
            nxt = re.search(r'\nfunction\b', text[end:stop])
            body = text[end:end + nxt.start()] if nxt else text[end:stop]
            # arg1, a local unpacked from the event, or the widget's own
            # self.cvarLabel - all three are comparisons of the first argument.
            for m in re.finditer(r'(?:arg1|cvar|self\.cvarLabel|variableName)'
                                 r'\s*==\s*"([A-Za-z]\w*)"', body):
                wanted.setdefault(m.group(1), set()).add(path.name)
    return wanted


def announced():
    """This client's own payload rule, read out of the binding that applies it."""
    src = API.read_text(errors="ignore")
    block = re.search(r"kNamed\[\]\s*=\s*\{(.*?)\};", src, re.S)
    named = {} if not block else dict(
        (k.lower(), v) for k, v in
        re.findall(r'\{\s*"(\w+)",\s*(?:/\*.*?\*/\s*)?"(\w+)"\s*\}', block.group(1)))

    def payload(name):
        key = name.lower()
        if key in named:
            return named[key]
        out = []
        for i, c in enumerate(name):
            if i and c.isupper() and not name[i - 1].isupper():
                out.append("_")
            out.append(c.upper())
        return "".join(out)

    return payload, named


def main():
    if not XML.is_dir():
        # loaded_files() answers with an empty set for a directory that is not
        # there, so no pair and no comparison is collected, both report columns
        # read empty, and that is what a clean interface looks like too.
        print(f"No interface at {XML} - name the unpacked interface on the "
              "command line; there are no CVars mentioned here.")
        return 2
    files = loaded_files(XML)
    pairs = declared_pairs(files)
    wanted = compared_labels(files)
    payload, named = announced()
    driven_names = []
    if SCHEMA.is_file():
        driven_names = re.findall(r'"cvar:(\w+)"', SCHEMA.read_text(errors="ignore"))
    driven = {d.lower() for d in driven_names}
    if not pairs or not wanted:
        print("  CANARY: no pair and no comparison parsed - the report below "
              "means nothing.\n")

    # Keyed case-blind, because that is how a CVar name is looked up, but the
    # transform has to see the spelling the interface wrote - combatTextFloatMode
    # announced as combattextfloatmode is COMBATTEXTFLOATMODE, which matches
    # nothing.
    declared, spelled = {}, {}
    for cvar, pay in pairs:
        declared.setdefault(cvar.lower(), set()).add(pay)
        spelled.setdefault(cvar.lower(), cvar)

    print(f"{len(pairs)} declared (cvar, payload) pair(s), "
          f"{len(wanted)} label(s) compared against the first argument, "
          f"{len(named)} kNamed entries\n")

    wrong = []
    for cvar, pays in sorted(declared.items()):
        got = payload(spelled[cvar])
        if got not in pays:
            where = "/".join(sorted({w for (c, _), ws in pairs.items()
                                     if c.lower() == cvar for w in ws}))
            wrong.append((cvar, sorted(pays), got, cvar in driven, where))
    print(f"{len(wrong)} pair(s) this client would announce as something else - "
          "a change made here never reaches the frames listening:\n")
    for cvar, pays, got, is_driven, where in wrong:
        print(f"  {'WINDOW' if is_driven else '       '} {cvar:30} "
              f"announced {'/'.join(pays):40} as {got:28} [{where}]")
    if not wrong:
        print("  (none)")

    passed = {p for (_, p) in pairs}
    dead = {l: f for l, f in wanted.items() if l not in passed}
    # What this client can announce anyway: its window is the writer for those,
    # and for a setting nobody in the interface changes, the comparison is the
    # only evidence of what the frame is waiting for. Reaching it is the point
    # of the fallback transform, so these rows are read as "the original client
    # left this branch dead; here the setting does something".
    here = set(named.values()) | {payload(d) for d in driven_names}
    unreachable = [l for l in dead if l not in here]
    print(f"\n{len(dead)} label(s) compared that no writer in the interface passes "
          "- a branch the original client cannot take either, "
          f"{len(unreachable)} of them unreachable here as well:\n")
    for lbl, files_ in sorted(dead.items()):
        print(f"  {lbl:34} {' '.join(sorted(files_)):34} "
              f"{'announced by this window' if lbl in here else 'unreachable here too'}")
    if not dead:
        print("  (none)")

    stranded = [(c, p) for c, p in sorted(named.items())
                if p not in declared.get(c, ()) and p not in wanted]
    print(f"\n{len(stranded)} kNamed entry/entries no declaration or comparison "
          "supports:\n")
    for c, p in stranded:
        others = "/".join(sorted(declared.get(c, ()))) or "-"
        print(f"  {c:34} {p}   (declared: {others})")
    if not stranded:
        print("  (none)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
