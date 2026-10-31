#!/usr/bin/env python3
"""What an unset CVar reads as, said in two places, checked against itself.

A CVar the player has never touched has to answer the same to both sides of this
client or the options panel and the game disagree about the same setting.

The two sides are:

  * `GetCVar` in `src/addons/lua_system_api.cpp`, which is what the interface
    asks. Its fallbacks are what a checkbox ticks itself from.
  * `storedCVarValue(name, default)` and its float twin `storedCVarFloat`, which
    are what the client reads where the setting is actually used - in the combat
    text, the nameplates, the chat filters, the shadow quality, and the camera's
    own blend rates and time window.

Neither is wrong to exist. The interface needs an answer before anything is
stored, and the client needs one at the point of use. What they must not do is
differ: a `fctHealing` that answers 0 to the panel and 1 to the client draws
healing numbers over an unticked box, and the player's only way to fix a setting
that is already doing what they want is to tick it twice.

Nothing else here would notice. Both values are individually reasonable, both
sides work, and the disagreement only shows up on screen - which is the same
shape as the login screen's parallax scale and the preset table that had drifted
in four of ten columns.

The two sides spell a number differently, so they are compared as numbers when
both read as numbers: `storedCVarFloat("cameraSmoothTimeMin", "0.1")` beside a
`GetCVar` answer of `"0.1"` is the same window, and `"0.10"` would be too.

Reads the source rather than running anything, so it costs nothing and holds for
files that start using either side after this was written.

The `||` chains matter. A branch may test several names before pushing one
value - `n == "fctdamage" || n == "fcthealing" || n == "fctdodgeparrymiss"` -
and a pattern that only takes the first name reports the other two as having no
answer at all. That is what the first version of this did, and it named two
CVars as unanswered that are answered on the same line.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
API = ROOT / "src/addons/lua_system_api.cpp"

# Every name in a branch, not just the first, then the value that branch pushes.
FALLBACK = re.compile(
    r'((?:n == "[a-z0-9_]+"\s*(?:\|\|\s*)?)+)\)?\s*(?:\{\s*)?lua_pushstring\(L,\s*"([^"]*)"\)',
    re.IGNORECASE | re.DOTALL)
NAME = re.compile(r'n == "([a-z0-9_]+)"', re.IGNORECASE)
DIRECT = re.compile(
    r'storedCVar(Value|Float)\(\s*"([A-Za-z0-9_]+)"\s*,\s*(?:\.\w+\s*=\s*)?"([^"]*)"')
# The same call with its two arguments in a table: MoveView{In,Out,Left,Right,
# Up,Down}Start keeps its three rates as `{"cameraYawMoveSpeed", "180"}` rows and
# reads one of them by index, because which one it wants is only known at
# runtime. Nothing beside the call names the CVar or its fallback, so the table
# has to be read - and until this was, the pair was a fallback no check saw. It
# carried 180 for a CVar GetCVar answered 0.2 for, and the view turned at the one
# while the interface printed the other.
#
# A pair whose second field is not a number is a (CVar, setting) binding or a
# (setting, field) pair, not a fallback, and is left alone.
TABLE_READ = re.compile(r'storedCVar(Value|Float)\(\s*\w+\s*\[')
PAIR = re.compile(r'\{\s*"([A-Za-z0-9_]+)"\s*,\s*"(-?[\d.]+)"\s*\}')


SCHEMA = ROOT / "src/ui/settings_schema.cpp"


def schema_rows(text):
    """The `kSchema[]` rows as lists of fields.

    Positional reading is the only way: a tooltip is several string literals
    glued together across lines and carries `\\"` escapes, so a row cannot be
    matched with a pattern over the whole line. `defaultValue` is the eleventh
    field and the store - `"cvar:NAME"` for the rows that write a CVar of the
    same name - comes later, so the store is found by what it looks like.
    """
    start = text.index("kSchema[] = {")
    end = text.index("};", start)
    rows, i = [], start
    while True:
        i = text.find('{"', i + 1, end)
        if i < 0:
            return rows
        rows.append(fields_of(text, i))
    raise AssertionError("unreachable")


def fields_of(s, i):
    """One brace-init's fields, from s[i] == '{'."""
    i += 1
    out = []
    while i < len(s):
        c = s[i]
        if c in " \t\r\n":
            i += 1
            continue
        if c == "}":
            break
        if c == '"':
            buf = []
            while True:
                j = i + 1
                while j < len(s):
                    if s[j] == "\\":
                        j += 2
                        continue
                    if s[j] == '"':
                        break
                    j += 1
                buf.append(s[i + 1:j])
                i = j + 1
                k = i
                while k < len(s) and s[k] in " \t\r\n":
                    k += 1
                if k < len(s) and s[k] == '"':      # a concatenated literal
                    i = k
                    continue
                break
            out.append('"' + "".join(buf) + '"')
            continue                                # the comma is the next scan
        j = i
        depth = 0
        while j < len(s):
            ch = s[j]
            if ch == '"':
                j = s.index('"', j + 1) + 1
                continue
            if ch in "([{":
                depth += 1
            elif ch in ")]}":
                if depth == 0:
                    break
                depth -= 1
            elif ch == "," and depth == 0:
                break
            j += 1
        out.append(s[i:j].strip())
        i = j
        if i < len(s) and s[i] == ",":
            i += 1
    # a string field leaves its comma behind for the generic branch, which reads
    # it as an empty field - drop those so the indices are the struct's own
    return [t for t in out if t != ""]


def agree(answer, read, as_number):
    """The same value, however each side spells it.

    Only a `storedCVarFloat` read may spell its fallback differently from the
    string GetCVar pushes, because that is the one that converts. A
    `storedCVarValue` fallback is compared as text by the client itself, so
    "01" against "1" is a real disagreement here even though both parse to one.
    """
    if answer == read:
        return True
    if not as_number:
        return False
    try:
        return float(answer) == float(read)
    except ValueError:
        return False


def main():
    if not API.is_file():
        print("lua_system_api.cpp is missing - nothing compared.")
        return 1

    source = API.read_text(encoding="utf-8")
    answers = {}
    for match in FALLBACK.finditer(source):
        for name in NAME.findall(match.group(1)):
            answers.setdefault(name.lower(), match.group(2))

    reads = {}
    for path in sorted(list(ROOT.glob("src/**/*.cpp")) + list(ROOT.glob("include/**/*.hpp"))):
        text = path.read_text(encoding="utf-8")
        for kind, name, value in DIRECT.findall(text):
            reads.setdefault(name, (value, path.relative_to(ROOT), kind))
        # Only where a read is actually taken from a table: a file full of
        # name/string pairs that are not fallbacks at all is not a file to be
        # mining defaults out of.
        if TABLE_READ.search(text):
            for name, value in PAIR.findall(text):
                reads.setdefault(name, (value, path.relative_to(ROOT), "Float"))

    # The panel's own starting values. A row whose store is `cvar:NAME` is a
    # control over that CVar, so its defaultValue is the box the player sees on
    # the first login and has to mean the same thing GetCVar answers.
    on_rows = []
    if SCHEMA.is_file():
        for fields in schema_rows(SCHEMA.read_text(encoding="utf-8")):
            if len(fields) < 11:
                continue
            at = next((k for k, f in enumerate(fields) if f.startswith('"cvar:')), None)
            if at is None or at < 11:
                continue
            default = fields[10].strip()
            # A dropdown keeps an index and names what each index is stored as,
            # so the row's own number is not the CVar's value: "Show it: Always"
            # is targetOfTargetMode "5". The `values` list follows the store.
            kept = fields[at + 1] if at + 1 < len(fields) else ""
            if (kept.startswith('"') and "|" in kept
                    # a `values` list is words, numbers and the odd strftime
                    # code; the `after` script that follows it is Lua
                    and re.fullmatch(r"[A-Za-z0-9_%:.\- |]*", kept[1:-1])):
                choices = kept[1:-1].split("|")
                try:
                    default = choices[int(float(default))]
                except (ValueError, IndexError):
                    default = f"<no choice {default} of {len(choices)}>"
            on_rows.append((fields[at][6:-1], default, fields[0]))

    print(f"GetCVar answers for {len(answers)} CVars; the client reads {len(reads)} "
          f"of them where they are used; {len(on_rows)} panel rows sit on a CVar.\n")

    unanswered, disagree, unstated = [], [], []
    for name, (value, where, kind) in sorted(reads.items()):
        answer = answers.get(name.lower())
        if answer is None:
            unanswered.append((name, value, where))
        elif not agree(answer, value, kind == "Float"):
            disagree.append((name, answer, value, where))

    for cvar, default, key in sorted(on_rows):
        answer = answers.get(cvar.lower())
        if answer is None:
            # "0" is what GetCVar gives back for a name nobody answered, so a
            # row starting anywhere else is the disagreement.
            if not agree("0", default, True):
                unstated.append((cvar, default, key))
        elif not agree(answer, default, True):
            disagree.append((cvar, answer, default, f"the {key} row of settings_schema.cpp"))

    for name, value, where in unanswered:
        print(f"  {name}: the client reads {value!r} at {where}, and GetCVar has no "
              f"answer - so the panel reads it as unset while the client acts on {value!r}")
    for name, answer, value, where in disagree:
        print(f"  {name}: GetCVar answers {answer!r} and the client reads {value!r} "
              f"at {where}")
    for cvar, default, key in unstated:
        print(f"  {cvar}: the {key} row starts at {default} and GetCVar has no "
              "answer, so the box is drawn on and the interface reads off")

    if unanswered or disagree or unstated:
        print(f"\n{len(unanswered) + len(disagree) + len(unstated)} CVar(s) mean "
              "different things to the panel and to the client.")
        return 0

    # Both sides are found by pattern, so a pattern that stops matching reports
    # the same silence as agreement.
    if len(answers) < 100 or len(reads) < 20:
        print(f"\nonly {len(answers)} answers and {len(reads)} reads were found, "
              "which is fewer than either side has - the parse stopped matching "
              "rather than finding nothing wrong.")
        return 1

    print("every CVar the client reads for itself answers the interface the same way.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
