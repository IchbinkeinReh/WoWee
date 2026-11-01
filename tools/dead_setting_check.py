#!/usr/bin/env python3
"""Options panel controls whose CVar nothing ever reads.

Every checkbox, slider and dropdown in the options panels names a CVar in
self.cvar. The control will save that CVar and read it back happily whether or
not anything acts on it, so a setting with no reader looks exactly like a
setting that works: it remembers what you chose and changes nothing.

A CVar counts as read if any of these mention it, outside the panel definition
that declares it:

  * FrameXML asking for it directly - GetCVar/GetCVarBool/SetCVar
  * a uvarInfo entry mapping it to a global, where that global is read
  * the client itself - storedCVarValue, or the name as a string literal

Names are matched case-insensitively, because the client lowercases them.

A control built in Lua rather than XML has no name this can resolve, so
greying it does not take it off the list. That under-credits by two today (the
two voice device dropdowns) and errs towards reporting a setting as dead, which
is the safe direction for a ratchet.

Run with --canary to check the sweep can still see: it plants a control naming
a CVar nothing reads and fails if that is not reported. A matcher that has gone
blind reads exactly like a clean tree.

What that canary proves is narrow, and it is worth being plain about: it shows
the sweep can still report a name that appears nowhere at all. It cannot show
the reader test is calibrated, because a test that counts too much still
reports a name it never sees. Widening what counts as a reader is checked by
the finding count, not by the canary.

The extracted interface is installed data, not repository content, and it has
sat in more than one place: name its directory on the command line. With a
missing directory the check fails rather than reporting zero dead settings,
because "0 of 0 declared" and "nothing is dead" are the same line otherwise -
which is how this one read nothing at all for a while. Naming the interface
folder rather than its framexml subfolder used to be the other way of reading
nothing: the controls were collected with `PANELS.glob("*.xml")`, so the wrong
spelling declared no controls and the run exited saying so. Both spellings mean
the same interface now.

Which files the client opens is asked of the loader rather than decided from a
folder name, on both halves of the sweep, and the two halves were wrong in
opposite directions. The panel scan looked only inside FrameXML and so could not
see an options panel the newer interface keeps in an addon - blizzard_arenaui.xml
is one. The mention scan walked the whole interface tree, 73 files more than the
client opens (gluexml's 57, the twelve FrameXML files no .toc Include reaches,
lcdxml's two and one addon frame's two files), which credits a CVar as *read* on
the strength of a file no interpreter here runs - the direction that hides
findings rather than the direction that invents them. Measured on this install,
the two lists give the same answer today: the same 198 declared CVars, the same
0 findings. The arena panel's CVar is also declared on a FrameXML panel, and
focusframe.xml - which nothing loads - declares names that are declared elsewhere
too. Same verdict, and neither half of the comparison is decided by a folder name
any more.
"""

import argparse
import re
import sys
from pathlib import Path

# The loader is a sibling tool, and the interpreter runs with -I, so the tools
# directory has to be named to import it.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from framexml_source import loaded_files  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
PANELS = ROOT / "Data/interface/framexml"
CPP_ROOTS = [ROOT / "src", ROOT / "include"]


def interface_root():
    """The folder the .toc chain starts in, whichever spelling was named."""
    return PANELS.parent if PANELS.name.lower() == "framexml" else PANELS


def loaded(suffixes):
    """Interface files the client opens, from the .toc walk rather than a walk
    of the folder. Both spellings of the named directory work: FRAMEXML names
    the folder the panels live in, its parent names the interface, and the .toc
    sits in the parent."""
    return [p for p in loaded_files(interface_root())
            if p.suffix.lower() in suffixes]

# The files that declare controls. A mention inside one of these does not make
# a setting live, and both ways of being clever about that were tried:
#
#   * Counting every mention took this sweep from 29 findings to 3, because
#     those files carry a table keyed by CVar name for tooltip text, so nearly
#     every setting appears in them. The canary still passed - a planted name
#     that appears nowhere cannot detect a reader test that has gone slack.
#   * Counting only the by-name asks - GetCVar("x") - left two settings reading
#     as live whose only reader greys a neighbouring control.
#
# The second is the honest measure of the wrong thing. A panel consulting
# itself to grey a sibling changes the panel, not the game, and this sweep is
# looking for controls that change nothing in the game. cameraSmoothStyle was
# the case that prompted the question and it proves the point: it is asked for
# by name in interfaceoptionspanels.lua, and until it was wired up it still did
# not move the camera by one degree. (It was later unwired again, with the
# camera position smoothing it drove, which the original client never had.)
DECL_FILES = {"interfaceoptionspanels.xml", "interfaceoptionspanels.lua",
              "videooptionspanels.xml", "videooptionspanels.lua",
              "audiooptionspanels.xml", "audiooptionspanels.lua"}

CVAR_DECL = re.compile(r'self\.cvar\s*=\s*"([A-Za-z0-9_]+)"')
FRAME_DECL = re.compile(r'<Frame\s+name="([A-Za-z0-9_]+)"')
CONTROL_DECL = re.compile(r'<(?:CheckButton|Slider|Button|Frame)\s+name="(\$parent[A-Za-z0-9_]*|[A-Za-z0-9_]+)"')
#: A name in kRemovedControlsLua - one plain string per line.
REMOVED = re.compile(r'^\s*"([A-Za-z0-9_]+)",\s*(?:\.\w+\s*=\s*)?$', re.M)
#: `function SomeFrameName_OnLoad (self)` - the frame is the part before _On.
LUA_HANDLER = re.compile(r'function\s+([A-Za-z0-9_]+?)_On[A-Za-z]+\s*\(')
UVAR_DECL = re.compile(r'self\.uvar\s*=\s*"([A-Za-z0-9_]+)"')
UVAR_ENTRY = re.compile(r'\["([A-Z0-9_]+)"\]\s*=\s*\{[^}]*cvar\s*=\s*"([A-Za-z0-9_]+)"')


def read(p):
    try:
        return p.read_text(errors="ignore")
    except OSError:
        return ""


#: A start or end tag. Attribute values may hold ">" so they are consumed as
#: quoted runs rather than scanned for the closing bracket.
TAG = re.compile(r'<(/?)([A-Za-z][\w.]*)((?:[^>"\']|"[^"]*"|\'[^\']*\')*?)(/?)>', re.S)
NAME_ATTR = re.compile(r'\bname\s*=\s*"([^"]*)"')


def _controls_in_xml(text):
    """(cvar, control name, offset) for every self.cvar in one panel file.

    A depth stack over the tags, so a cvar is attributed to the element it is
    written inside. $parent is that element, which is not the nearest preceding
    named frame: a named sibling declared just above wins that race and gives a
    name no frame answers to. The four voice sliders are the example -
    $parentSpeakerVolume inside AudioOptionsVoicePanel sits after a named
    BindingOutput sibling, so a textual scan reads
    AudioOptionsVoicePanelBindingOutputSpeakerVolume and the frame is
    AudioOptionsVoicePanelSpeakerVolume.

    Hand-rolled rather than xml.etree, which the security scan blocks and which
    this does not need: nothing here parses anything but the repository's own
    markup, and only names and nesting are wanted. Checked against the parser
    it replaced across all 140 panel files, name for name.
    """
    text = re.sub(r"<!--.*?-->", "", text, flags=re.S)
    events = []
    for m in TAG.finditer(text):
        events.append((m.start(), "tag", m))
    for m in CVAR_DECL.finditer(text):
        events.append((m.start(), "cvar", m))
    events.sort(key=lambda e: e[0])

    stack, out = [], []
    for pos, kind, m in events:
        if kind == "cvar":
            owner = next((f for f in reversed(stack) if f), None)
            out.append((m.group(1).lower(), owner, pos))
            continue
        closing, _tag, attrs, selfclose = m.groups()
        if closing:
            if stack:
                stack.pop()
            continue
        nm = NAME_ATTR.search(attrs)
        resolved = None
        if nm:
            raw = nm.group(1)
            parent = next((f for f in reversed(stack) if f), None)
            resolved = (parent + raw[len("$parent"):]) if raw.startswith("$parent") and parent \
                       else (None if raw.startswith("$parent") else raw)
        if not selfclose:
            stack.append(resolved)
    return out


def declared_controls():
    """CVar -> (file:line, control frame name or None)."""
    out = {}
    # The options panels first. A CVar can be declared on a unit frame as well
    # as on the control that sets it - targetStatusText is declared five times
    # across focusframe, targetframe and the panel - and the control the player
    # sees is the one this sweep is about. Document order alone picks whichever
    # file sorts first, which was focusframe.
    files = sorted(loaded({".xml"}),
                   key=lambda q: (q.name not in DECL_FILES, q.name))
    for q in files:
        text = read(q)
        for cvar, ctrl, pos in _controls_in_xml(text):
            if ctrl is None:
                continue
            out.setdefault(cvar, (f"{q.name}:{text.count(chr(10), 0, pos) + 1}", ctrl))

    # Controls built in Lua rather than XML. Their frame name is in the handler
    # they are declared inside - AudioOptionsSoundPanelHardwareDropDown sets its
    # cvar in AudioOptionsSoundPanelHardwareDropDown_OnLoad - so the enclosing
    # function names the control the same way $parent does in the markup.
    # Without this a device dropdown could be greyed and still read as
    # unhandled, because nothing here knew what it was called.
    for p in sorted(loaded({".lua"})):
        text = read(p)
        for m in CVAR_DECL.finditer(text):
            line = text.count("\n", 0, m.start()) + 1
            fns = list(LUA_HANDLER.finditer(text[:m.start()]))
            ctrl = fns[-1].group(1) if fns else None
            out.setdefault(m.group(1).lower(), (f"{p.name}:{line}", ctrl))
    return out


def removed_controls():
    """Frame names the client takes off its panels.

    A control on a page the client drops whole counts as removed too - the
    player cannot reach it either way, and listing its thirteen controls
    individually as well would be the same fact written twice.
    """
    text = read(ROOT / "include/addons/addon_lua_snippets.hpp")
    start = text.find("kRemovedControlsLua")
    if start == -1:
        return set(), set()
    end = text.find(")LUA", start)
    body = text[start:end]
    cut = body.find("kRemovedCategories")
    names = set(REMOVED.findall(body[:cut] if cut != -1 else body))
    pages = set(REMOVED.findall(body[cut:])) if cut != -1 else set()
    return names, pages


def uvar_map():
    """cvar (lower) -> uvar global, from uvarInfo entries.

    The file is looked for among the ones the client opens rather than by
    joining the named directory to a remembered file name, which is what made
    the interface-root spelling answer nothing at all: the uvar map was empty,
    so every setting that is read through a uvar global looked unread.
    """
    hits = [p for p in loaded({".lua"})
            if p.name.lower() == "interfaceoptionsframe.lua"]
    text = read(hits[0]) if hits else read(PANELS / "interfaceoptionsframe.lua")
    return {c.lower(): u for u, c in UVAR_ENTRY.findall(text)}


def gather(roots, suffixes):
    for root in roots:
        if not root.exists():
            continue
        for p in root.rglob("*"):
            if p.is_file() and p.suffix in suffixes:
                yield p


def readers(extra_snippets):
    """Lowercased names mentioned anywhere that is not a declaration site.

    The interface half is the files the client opens, which is narrower than the
    folder this used to walk - 343 rather than 416 - and narrower in the
    direction that matters for a ratchet. A mention in gluexml, in lcdxml, or in
    one of the twelve FrameXML files no .toc Include reaches makes a CVar look
    read by a program that never loads that file, and a sweep that believes a
    setting is alive reports nothing at all about it.
    """
    seen = set()
    globals_read = set()
    for p in loaded({".lua", ".xml"}):
        if p.name in DECL_FILES:
            continue
        text = read(p).lower()
        seen.add((p, text))
        globals_read.add((p, text))
    for p in gather(CPP_ROOTS, {".cpp", ".hpp", ".h"}):
        seen.add((p, read(p).lower()))
    for name, text in extra_snippets:
        seen.add((name, text.lower()))
    return seen


def main():
    global PANELS
    ap = argparse.ArgumentParser()
    ap.add_argument("--canary", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("interface", nargs="?", default=str(PANELS),
                    help="the directory of the extracted interface, or its framexml "
                         "subdirectory - it is installed data rather than repository "
                         "content, so the default may not be where it is")
    args = ap.parse_args()

    PANELS = Path(args.interface)
    if not PANELS.is_dir():
        # The controls are what everything here hangs on, and with the directory
        # away there are none - which prints exactly like a clean tree.
        print(f"{PANELS} is not a directory, so no control was declared and none "
              "could be checked. Name the extracted interface directory.")
        return 1

    controls = declared_controls()
    uvars = uvar_map()
    removed, removedPages = removed_controls()

    extra = []
    if args.canary:
        controls["woweecanarysettingnoreader"] = ("canary:0", None)

    corpus = readers(extra)

    dead = []
    handled = []
    for cvar, (where, ctrl) in sorted(controls.items()):
        needle = cvar
        found = False
        for _, text in corpus:
            if needle in text:
                found = True
                break
        if not found and cvar in uvars:
            g = uvars[cvar].lower()
            for _, text in corpus:
                if g in text:
                    found = True
                    break
        if not found:
            if ctrl and (ctrl in removed or any(ctrl.startswith(p) for p in removedPages)):
                handled.append((cvar, where))
            else:
                dead.append((cvar, where))

    total = len(controls)
    print(f"settings with no reader and still on a panel: {len(dead)} of {total} declared "
          f"({len(handled)} more are dead and taken off the panels)")
    for cvar, where in dead:
        print(f"  {cvar:38s} {where}")

    # "0 of 0" is the same silence as "nothing is dead", and it came out of a
    # default interface path that no longer exists once already.
    if total == 0:
        print("no control was declared - the parse stopped matching rather than "
              "finding nothing dead.")
        return 1

    if args.canary:
        if not any(c == "woweecanarysettingnoreader" for c, _ in dead):
            print("CANARY FAILED: planted dead setting was not reported")
            return 1
        print("canary ok")
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
