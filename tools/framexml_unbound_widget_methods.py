#!/usr/bin/env python3
"""Widget methods FrameXML calls that answer nil, and so raise.

The global surface has its own sweep. This is the other half: `frame:Method()`
rather than `Method()`, resolved through the widget metatable instead of _G.

Three things can happen to `frame:Foo()`:

  * Foo is in the method table (`{"Foo", lua_...}`) or has a Lua shim
    (`__WoweeFrameMT:Foo` / `mt:Foo`) - it works.
  * Foo is in the no-op allowlist (`"Foo=1"`) - it answers a harmless no-op.
    Silent, and the subject of its own audit; see framexml_missing_api_list.
  * Foo is none of those - the fallback records it and **returns nil**, so the
    call raises.

This finds the third kind without needing a run. The runtime does record them
(as `widget:<name>`), but only for methods a session actually reached, and only
into a report that has to be read.

False positives to expect, and why they cannot be filtered by shape:
  * Methods FrameXML defines on its own tables and objects - a table is not a
    widget, and `:Method()` looks identical either way. The script subtracts
    every `function X:Method`, `X.Method = function` and `X.Method = name` it
    can see, which catches most of them. What it cannot see is an assignment
    whose right side is a call - `context.GetTableName = Pick_Cache_Function(
    ...)` - because there is no way to know what a call answers. Check how a
    name is defined before treating it as a gap.
  * Methods on Lua's own types (`("x"):format(...)`) - lowercase, so the
    uppercase-initial filter drops them.

A FIELD ASSIGNMENT IS A DEFINITION, AND THE ENGINE READS IT TOO

`X.Method = someFunction` installs the method on that object: a field beats
the metatable on the way out, and it is the same field the call reads on the
way in. So blizzard_achievementui's `self.Collapse = AchievementButton_
Collapse` in AchievementButton_OnLoad answers Collapse and Expand, and the
options panels' `self.SetDisplayValue = self.SetValue` answers
SetDisplayValue - which is not a client method at all, not in the exe and not
in `__WoweeWidgetMethods`. It is FrameXML saving the prototype before
shadowing it:

    self.SetDisplayValue = self.SetValue;      -- the engine's, read off mt
    self.SetValue = function (self, value)     -- this panel's, in the field
        self:SetDisplayValue(value);           -- the one it just saved
        ...

The read on the first line works here for the same reason it works in the
reference client: `mt.__index = mt`, so `self.SetValue` finds the engine's
binding as a value. The panels that never run that OnLoad are guarded at the
call - `if (slider.SetDisplayValue) then` in optionspaneltemplates.lua - and
the fallback answers nil for the read, so the branch is simply not taken.
SetDisplayValue is therefore answered twice over and must not be added to the
engine as if it were API: the string does not occur in Wow.exe once.

WHAT IT REPORTS TODAY, AND WHY NONE OF IT IS REAL

Four, all of them on dump.lua's writer object, which nothing in the tree ever
constructs - `/dump` is not reachable here. Three are assigned from a call
(rule above), and `Result` is never assigned but is only ever called behind
`if (context.Result) then`, which the nil fallback fails harmlessly.

The nine that were real-looking before were GlueXML, and are gone now that
this reads only what the loader opens.
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from framexml_source import loaded_files

ROOT = Path(__file__).resolve().parent.parent
# The extracted interface is installed data rather than repository content:
# name its directory, or nothing here is read and the sweep reports nothing.
XML = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "Data/interface"
if not XML.is_dir():
    print(f"no interface at {XML} - name one on the command line")
    raise SystemExit(2)
ENGINE = ROOT / "src/addons/lua_engine.cpp"


def strip(text, is_xml):
    """Both rules, from the one place that has them. A method call is syntax,
    so the insides of strings go too."""
    del is_xml   # both rules apply to both kinds of file
    return without_comments_or_strings(text)


# What the engine answers for - asked of the one place that decides rather
# than worked out again here. This file had its own copy of that regex, and it
# matched only `mt:`, so every method the bootstrap defines on animMeta or
# groupMeta read as unanswered: IsPlaying, SetDuration and SetOffset were all
# reported against the glyph frame when the animation system has provided them
# since it was written. One fact in two places, drifting.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from framexml_provides import widget_methods_provided  # noqa: E402
from framexml_source import without_comments_or_strings  # noqa: E402, loaded_files

answered_by_engine = widget_methods_provided()

# Methods FrameXML defines itself, on frames or on its own tables.
defined = set()
called = {}
# Only files the loader opens. GlueXML - login, character select, realm
# list - sits in the same tree and is refused by name, so a name it calls
# or a value it unpacks says nothing about this client.
for p in sorted(loaded_files(XML)):
    # utf-8 named: these files are utf-8 and the default on a Windows console
    # is not, so the German interface decodes as cp1252 and `errors="ignore"`
    # silently drops the five bytes cp1252 leaves undefined - inside a name or
    # a comment delimiter, that moves what the patterns below match.
    text = strip(p.read_text(encoding="utf-8", errors="ignore"),
                 p.suffix == ".xml")
    defined |= set(re.findall(r"\bfunction\s+[\w.]+[.:](\w+)\s*\(", text))
    defined |= set(re.findall(r"[\w.]+\.(\w+)\s*=\s*function", text))
    # ...and assigned straight from another name, which is the same install by
    # a shorter route. `X.Method = someName` puts a callable in a field, the
    # field beats the metatable, and the call answers. framexml_api_gap.py has
    # had this rule for globals all along and used it to clear
    # AchievementFrameTab_OnClick, which is assigned rather than defined; the
    # widget half of the surface needed it for Collapse, Expand and
    # SetDisplayValue, three names this reported as raising.
    #
    # The right side has to be a bare name (or `self.SetValue`, a field read)
    # and the line has to end there: that keeps `x.y == z` out, keeps out
    # `= nil`, which installs the opposite of a method, and keeps out
    # `context.GetTableName = Pick_Cache_Function(...)` - a call's value is
    # not knowable here, and pretending otherwise would hide the four names
    # this still reports.
    defined |= set(re.findall(
        r"[\w.]+\.(\w+)\s*=(?!=)\s*(?!nil\b|false\b)[A-Za-z_][\w.]*\s*;?\s*$",
        text, re.M))
    for m in re.finditer(r":(\w+)\s*\(", text):
        called.setdefault(m.group(1), set()).add(p.name)

# Uppercase-initial only: WoW widget methods are PascalCase, and the fallback
# itself only records those. On* are script handler names read as fields.
answered = answered_by_engine | defined
missing = {
    name: files
    for name, files in called.items()
    if name[:1].isupper() and not name.startswith("On") and name not in answered
}

print(f"{len(answered_by_engine)} answered by the engine, "
      f"{len(defined)} defined by FrameXML\n")
print(f"{len(missing)} widget methods called that answer nil:\n")
for name, files in sorted(missing.items(), key=lambda kv: (-len(kv[1]), kv[0])):
    where = " ".join(sorted(files)[:3])
    more = f" +{len(files) - 3}" if len(files) > 3 else ""
    print(f"  {name:34} {len(files):3} file(s)  {where}{more}")

sys.exit(0)
