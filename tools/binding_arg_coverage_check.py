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
  * "counts luaL_*(L, N)" means **readers** of the argument, not every call that
    names L beside a number. It used to be the latter, which credited a binding
    with `lua_pushboolean(L, 1)` (a value, not an index) and `lua_pop(L, 2)` (a
    slot count), so `GetBinding(index[, mode])` looked like it read the mode
    because the helper it ends with pops two, and a binding whose whole job is
    pushing values hid the binding that called it. Requiring a reader rather than
    a lua_* call is the direction that fails safe: a reader this pattern does not
    know prints a row to go and read.
  * a helper need not be spelled `static` or `inline` to count as one.
    `uint64_t usedOnUnit(lua_State* L, int index)` is the argument reader the
    three item-use bindings share, and none of them calls a lua_* function with
    the target themselves, so the requiring storage class made all three look
    like they ignored it.

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

It is not in the list below the count either, and that is a measurement worth
keeping. Its body reads no argument at all, which is not "one short of the call"
- the alias answers the whole call from the player, whatever it is handed. Said
apart, it sits with the other bindings of that shape, and the number they come
to is printed as well: the fault is real in each and a comma count cannot grade
it, so it is reported rather than counted.

Nor a binding whose extra argument is genuinely optional. Some of what is left
is that: the show-realm flag on GetUnitName, whose answer is a realm this client
never has, the exactMatch flag on the three party-assignment names, which every
caller passes as the one value that flag asks for. They are listed rather than
filtered because "harmless" is a judgement about each one, and the ceiling is
there so the next addition is looked at.

WHAT THE SECOND ARGUMENT SPLITS

    tools/binding_arg_coverage_check.py "<the extracted interface>" "<strings.tsv>"

The installed interface is a post-Cataclysm FrameXML wearing a 30300 tag, so a
name here could be one that binary never had. Pass the decompile's `strings.tsv`
and the rows split on whether the client's own strings carry the name, the same
test framexml_live_stubs.py applies. Run that on 2026-11-01 and it filters one
row of the thirty-seven: GetUnitName, which unitframe.lua:468 defines for
itself, so what the interface calls by that name is its own function and the
native binding is shadowed before anything can reach it. That the other
thirty-six come back is the useful half: GetActiveTalentGroup,
GetNumTalentGroups, AddPreviewTalentPoints and the rest of the dual-spec talent
names read like a feature from a decade later and are in the 12340 client after
all. Guessing which names are new is worth as much here as anywhere else, so the
split runs rather than a rule of thumb.

The split runs over the counted rows only. The second list describes a shape
rather than ratcheting a count, and the test would sort nothing out of it
anyway: every one of the fourteen names spot-checked there on 2026-11-01 - the
three KBSetup_*, the three voice-session names, GetMultiCastTotemSpells,
UnitClassBase, GetGuildInfo, GetMacroInfo, GetLFDChoiceInfo,
GetAttackPowerForStat, UnitIsSameServer and IsSelectedSpell - is a name the 12340
client carries.
Those are bindings asked about a feature that binary had, answering from
themselves instead of from what they were given.

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
    """The highest argument index a body reads with its own hands.

    Readers only. The pattern used to be "any lua_* call with a number after L",
    which also caught the pushers and the stack shufflers: `lua_pushboolean(L, 1)`
    hands the interpreter a *value* and `lua_pop(L, 2)` counts slots, and each of
    those made a binding look like it had taken an argument. A helper whose whole
    job is pushing two values scored two, and the binding that called it was
    dropped from the list - the exact shape of fault this sweep exists to find.
    Excluded that way round is the safe direction too: a missed reader shows up
    as a row to go and read, not as a clean sweep.
    """
    readers = r"(?:luaL_(?:check|opt)\w+|lua_(?:to|is)\w+)\(\s*L\s*,\s*(\d+)"
    idx = [int(n) for n in re.findall(readers, body)]
    return max(idx) if idx else 0


def helper_reach():
    """helper name -> the highest Lua argument index it reads itself."""
    src = HELPERS.read_text(errors="ignore")
    src += "".join(p.read_text(errors="ignore") for p in ADDONS.glob("*.cpp"))
    out = {}
    # Braces, for the reason given where the bodies are read: a one-line helper
    # has no closing brace at the start of a line.
    #
    # And a helper need not be spelled static or inline at all: `uint64_t
    # usedOnUnit(lua_State* L, int index)` is the argument reader the three
    # item-use bindings share, none of which call a lua_* function with the
    # target themselves. Requiring the storage class made those three look like
    # they ignored the target their caller passes, right after the helper that
    # hands it to the wire was written. A definition at the start of a line
    # counts with or without it - the `lua_State* L` inside the parentheses is
    # what says definition rather than call.
    for m in re.finditer(r"^[ \t{]*(?:static\s+|inline\s+)?[\w:*&<> ]+?\s(\w+)\(lua_State\* L[^)]*\)\s*\{(?:\.\w+\s*=\s*)?",
                         src, re.M):
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
    blind = []
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
        # A body that reads nothing is not "one argument short of the call", it
        # answers the whole call from elsewhere - GetAttackPowerForStat(stat,
        # value) aliased to GetAttackPower, which hands back the player's total
        # melee power whatever it is handed. Counting that as "reads 1, passes 2"
        # made it one more row of the list; asking for neither is a different
        # width of fault, so it is listed apart and kept out of the count.
        if reads == 0 and widest[name] > reads:
            # Split again by whether the body answers at all, because the two
            # halves are two different faults and only one of them is new here.
            # `(void)L; return 0;` is a binding that has not been written - the
            # other sweep (framexml_live_stubs.py) reports every one of those by
            # name already, and repeating 150 of them buries what the count is
            # for. A body that *pushes values* while ignoring everything it was
            # handed is the fault this list exists to catch: the call answers,
            # confidently, about the wrong thing.
            answers = re.search(r"lua_push\w+|luaReturn\w+|return\s+[1-9]", body)
            if answers:
                blind.append((widest[name], name))
        elif reads and widest[name] > reads:
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

    # Printed apart, and deliberately outside the count above: a binding that
    # names no argument at all has not "missed the last one", it answers the
    # whole call from something else.
    if blind:
        print()
        print(f"{len(blind)} of them read no argument at all, whatever the interface")
        print("passes - each answers the call from elsewhere, which a comma count")
        print("cannot grade:")
        for passes, name in sorted(blind, reverse=True):
            print(f"  passes {passes}, reads none   {name}")

    if STRINGS and STRINGS.is_file():
        known = set()
        with STRINGS.open(encoding="utf-8", errors="ignore") as fh:
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
