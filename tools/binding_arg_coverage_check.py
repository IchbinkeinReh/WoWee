#!/usr/bin/env python3
"""Bindings that read fewer arguments than the interface passes.

    tools/binding_arg_coverage_check.py "<the extracted interface>"

The interface is installed data rather than repository content, so the default
below is often not where it is.  With nothing parsed the widest call the
interface makes is nothing at all, and the run stops rather than printing the
"nothing under-read" lines a fully answered client prints.

WHY THIS FINDS WHAT THE OTHER SWEEPS CANNOT

Every sweep here asks whether a *name* is answered. This asks whether the
answer used everything it was told. The name is bound either way, so nothing
else can see it - and an ignored argument does not raise, it answers confidently
about the wrong thing.

Seven faults came out of it on 2026-08-05, and the split is the point. Three
showed wrong data:

  * bookType - the pet spellbook named the player's spells
  * powerType - a druid's mana bar in a form showed its energy
  * pet (talents) - the pet tab drew the player's trees

Four *acted* on the wrong thing, which is the half that matters:

  * PickupSpell    dragging a pet spell put a player spell on the bar
  * LearnTalent    clicking a pet talent spent a point in the player's tree
  * CancelUnitBuff a name where an index was expected raised instead of
                   cancelling - the possess bar's way out of a vehicle
  * CastSpellByID  click-casting on a unit frame cast on the current target,
                   and CastSpellByName two lines away was wrong the same way

None raised. In every one of them the icons were right, because the tables
behind the icons had been fixed separately, and only the thing underneath was
wrong.

WHAT IT COMPARES

The most arguments the interface ever passes to a name, against the highest
argument index the binding reads. Both sides are approximations and the
docstring is honest about which way each errs:

  * the interface side takes the widest call it can see on one line, so a call
    split across lines is skipped rather than undercounted;
  * the binding side counts luaL_*(L, N) written in the body **and** arguments
    read through a helper - `wantsPetTalents(L, 4)` names its index at the call
    site, and `spellIdForCall(L, gh)` reads one and two inside itself. Missing
    that made four talent bindings read as unfixed immediately after they were
    fixed, which is the same shape as framexml_provides not seeing the
    set("Name", ...) registration form.

WHAT IT COULD NOT SEE UNTIL 2026-08-06

More than half the bindings. A binding registered as an inline lambda in the
table - {"Name", [](lua_State* L) -> int { ... }} - is the same binding to Lua
as one written as a named function above it, but only the named form was
matched here, so 738 of 1420 were never asked the question. Three faults came
straight out of the other half, and all three are the acting kind:

  * DoTradeSkill    both of the trade skill frame's buttons pass a count, and
                    Create All made one item. The craft queue it needed already
                    existed; the client's own crafting window was using it.
  * CastPetAction   the unit a click-cast binding passes was dropped, so the pet
                    went at the current target instead of what was clicked -
                    exactly the fault CastSpellByID had, two files over.
  * ToggleSpellAutocast  the spellbook passes a book slot and its book; only the
                    name form was handled, so right-clicking a pet spell looked
                    up a spell called "12" and toggled nothing.

WHAT IT CANNOT SEE

An argument that is read and then misused - UnitIsFriend read its first
argument and answered about the wrong unit of the two, so it never appeared
here. That one came from reading the call sites, and this sweep is a way of
choosing which call sites to read rather than a replacement for reading them.

THE ONE ROW THAT IS WRONG AND IS LEFT

GetAttackPowerForStat(statIndex, amount) is aliased to GetAttackPower, which
answers the player's *total* melee attack power - so the character sheet's
"increases attack power by %d" prints that total as the contribution of one
stat. Getting it right needs the per-class coefficient for each stat, which is
a table this client does not carry and which a guess would get wrong for half
the classes. Wrong is wrong either way; a made-up formula would merely be
harder to notice.

Nor a binding whose extra argument is genuinely optional. Some of what is left
is that: the show-realm flag on GetUnitName, whose answer is a realm this client
never has, the exactMatch flag on the three party-assignment names, which every
caller passes as the one value that flag asks for. They are listed rather than
filtered because "harmless" is a judgement about each one, and the ceiling is
there so the next addition is looked at.

WHAT THE SECOND ARGUMENT SPLITS

    tools/binding_arg_coverage_check.py "<the extracted interface>" "<strings.tsv>"

WHAT THE SECOND ARGUMENT SPLITS

    tools/binding_arg_coverage_check.py "<the extracted interface>" "<strings.tsv>"

The installed interface is a post-Cataclysm FrameXML wearing a 30300 tag, so a
name here could be one that binary never had. Pass the decompile's `strings.tsv`
and the rows split on whether the client's own strings carry the name, the same
test framexml_live_stubs.py applies. Run that on 2026-11-01 and it filters one
row of 47: GetUnitName, which unitframe.lua:468 defines for itself, so what the
interface calls by that name is its own function and the native binding is
shadowed before anything can reach it. That the other 46 come back is the useful
half: GetActiveTalentGroup, GetNumTalentGroups, AddPreviewTalentPoints and the
rest of the dual-spec talent names read like a feature from a decade later and
are in the 12340 client after all. Guessing which names are new is worth as much
here as anywhere else, so the split runs rather than a rule of thumb.

The count the ceiling is held to stays the unsplit one: the split says which
rows to read, not how many faults there are.

A wide call is not the same as a call that can vary. The binding panel passes
`KeyBindingFrame.mode` to GetBinding, GetBindingKey, GetBindingAction and
SetBinding; the two sites that open the panel set that field to 1 and nothing
else writes it, so the mode argument is a constant - and the client's own Usage
line ("GetBinding(index[, mode])", 0x0055e8d0) takes 1..4 and defaults to the
first. Four rows out, one value in. Only reading the call site says so: a
constant is still an argument as far as a comma count is concerned, and the
sweep cannot tell a constant from a control.
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from framexml_source import loaded_files, without_comments  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
INTERFACE = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "Data/interface"
ADDONS = ROOT / "src/addons"
HELPERS = ROOT / "include/addons/lua_api_helpers.hpp"
# The decompile's string table, for the split described in the note above.
STRINGS = Path(sys.argv[2]) if len(sys.argv) > 2 else None


def _top_level_commas(text):
    depth = 0
    count = 0
    for ch in text:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif ch == "," and depth == 0:
            count += 1
    return count + 1 if text.strip() else 0


def passed_by_interface():
    """name -> the most arguments any single-line call passes."""
    widest = {}
    for path in loaded_files(INTERFACE):
        text = without_comments(path.read_text(errors="ignore"))
        for m in re.finditer(r"(?<![\w.:])([A-Z][A-Za-z0-9_]{2,})\(", text):
            name = m.group(1)
            i = m.end()
            depth, j = 1, i
            while j < len(text) and depth > 0 and j - i < 200:
                if text[j] in "([{":
                    depth += 1
                elif text[j] in ")]}":
                    depth -= 1
                j += 1
            args = text[i:j - 1]
            if "\n" in args:
                continue          # a wrapped call, rather than a short one
            widest[name] = max(widest.get(name, 0), _top_level_commas(args))
    return widest


def _max_index(body):
    idx = [int(n) for n in re.findall(r"lua(?:L)?_\w+\(\s*L\s*,\s*(\d+)", body)]
    return max(idx) if idx else 0


def helper_reach():
    """helper name -> the highest Lua argument index it reads itself."""
    src = HELPERS.read_text(errors="ignore")
    src += "".join(p.read_text(errors="ignore") for p in ADDONS.glob("*.cpp"))
    out = {}
    # Braces, for the reason given where the bodies are read: a one-line helper
    # has no closing brace at the start of a line.
    for m in re.finditer(r"(?:inline|static)\s+[\w:*&<> ]+?\s(\w+)\(lua_State\* L[^)]*\)\s*\{(?:\.\w+\s*=\s*)?", src):
        depth, i = 1, m.end()
        while i < len(src) and depth:
            if src[i] == "{":
                depth += 1
            elif src[i] == "}":
                depth -= 1
            i += 1
        out[m.group(1)] = _max_index(src[m.end():i - 1])
    return out


def main():
    if not INTERFACE.is_dir():
        print(f"{INTERFACE} is not a directory - name the extracted interface "
              "directory on the command line.")
        return 2
    widest = passed_by_interface()
    src = "".join(p.read_text(errors="ignore") for p in sorted(ADDONS.glob("*.cpp")))
    # Braces matched, not "to the first \n}". A named function written on one
    # line - `static int lua_X(lua_State* L) { (void)L; return 0; }` - has no
    # closing brace at the start of a line, so the non-greedy form ran on and
    # took the next function's body with it. Which bindings got reported then
    # depended on how the ones above them happened to be formatted: adding two
    # functions beside a one-liner moved GetContainerItemPurchaseInfo in and out
    # of the list without touching it. Same fault the inline form had below.
    bodies = {}
    for m in re.finditer(r"static int (lua_\w+)\(lua_State\* L\)\s*\{(?:\.\w+\s*=\s*)?", src):
        depth, i = 1, m.end()
        while i < len(src) and depth:
            if src[i] == "{":
                depth += 1
            elif src[i] == "}":
                depth -= 1
            i += 1
        bodies[m.group(1)] = src[m.end():i - 1]
    registered = {m.group(1): m.group(2) for m in
                  re.finditer(r'\{(?:\.\w+\s*=\s*)?"([A-Za-z_]\w*)",\s*(?:\.\w+\s*=\s*)?(lua_\w+)\}', src)}
    # ...and the inline form, which is more than half of them. Registering a
    # binding as a lambda in the table rather than as a named function above it
    # is the same binding to Lua, but it was invisible here: `registered` only
    # matched {"Name", lua_Name} and `bodies` only matched a named definition,
    # so 738 of the 1420 bindings were never asked whether they read what they
    # were passed. The body is taken by matching braces, because the closing
    # "}}" sits at whatever indent the file happens to use.
    for m in re.finditer(r'\{(?:\.\w+\s*=\s*)?"([A-Za-z_]\w*)",\s*(?:\.\w+\s*=\s*)?\[\]\(lua_State\* L\) -> int \{', src):
        depth, i = 1, m.end()
        while i < len(src) and depth:
            if src[i] == "{":
                depth += 1
            elif src[i] == "}":
                depth -= 1
            i += 1
        name = m.group(1)
        bodies[name] = src[m.end():i - 1]
        registered.setdefault(name, name)
    helpers = helper_reach()

    rows = []
    for name, impl in sorted(registered.items()):
        if name not in widest:
            continue
        body = bodies.get(impl)
        if not body:
            continue
        reads = _max_index(body)
        # ...and through a helper, either by the index named at the call site
        # or by what the helper reads on its own account.
        for call in re.finditer(r"(\w+)\(\s*L\s*(?:,\s*(\d+))?", body):
            fn, literal = call.group(1), call.group(2)
            if fn in helpers:
                reads = max(reads, helpers[fn])
            if literal and fn in helpers:
                reads = max(reads, int(literal))
        if reads > 0 and widest[name] > reads:
            rows.append((widest[name] - reads, name, widest[name], reads))

    rows.sort(reverse=True)
    print(f"{len(widest)} names called by the interface, {len(registered)} bindings read")
    if "UnitName" not in widest:
        print("  CANARY: UnitName never seen called - the interface is not parsing.")
    print()
    print(f"{len(rows)} binding(s) read fewer arguments than the interface passes:\n")
    for _, name, passes, reads in rows:
        print(f"  passes {passes}, reads {reads}   {name}")
    if not rows:
        print("  (none)")

    if STRINGS and STRINGS.is_file():
        known = set()
        with STRINGS.open(errors="ignore") as fh:
            for line in fh:
                parts = line.rstrip("\n").split("\t", 1)
                if len(parts) == 2:
                    known.add(parts[1])
        absent = sorted(name for _, name, _, _ in rows if name not in known)
        print()
        print(f"{len(absent)} of them the 12340 client carries no name for, so the")
        print("interface is asking that binary about a feature it never had:")
        for name in absent:
            print(f"  {name}")
        print()
        print(f"{len(rows) - len(absent)} are names the client does carry - those are the")
        print("ones with an answer to check.")
    elif STRINGS:
        print(f"\nno strings file at {STRINGS} - the list is unsplit")
    else:
        print("\nnames were not checked against the client binary; pass its decompile's")
        print("strings.tsv as the second argument to split the list")
    return 0


if __name__ == "__main__":
    sys.exit(main())
