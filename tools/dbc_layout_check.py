#!/usr/bin/env python3
"""DBC field indices that name a column the file does not have.

    tools/dbc_layout_check.py ["<the directory holding the .dbc files>"]

The directory named on the command line is the one the client reads its tables
from - for the extracted install, `…/expansions/wotlk/dbfilesclient`. `Data/db`
is the fallback, and it is not in the checkout, so run without an argument on
this machine used to print "no DBCs at …" and return 0, which is what a clean
tree prints: the sweep had not run against any file in a long time.

Every DBC read in this client goes through a named index out of
Data/expansions/<x>/dbc_layouts.json - `(*layout)["IconID"]` and the like. The
name makes it look checked. Nothing checks it: the loader is handed a number,
and a number past the end of a record, or pointed at the wrong column, comes
back as zero or as whatever bytes are there. What that looks like on screen is
an icon that does not load, a stat that reads zero, a beard that is not drawn -
never an error.

WHAT IT LOOKS FOR

An index at or past the file's own field count. That is the unambiguous half of
the problem and needs no judgement: the column does not exist, so the read
cannot be right. It found CharacterFacialHairStyles.Geoset200 = 8 on a file with
eight fields, and the two columns beside it held 0xCCCCCCCC - the facial hair
geosets were being read from padding, so no beard, moustache, sideburns or
draenei tendrils were drawn at all.

THE IN-RANGE-AND-EMPTY HALF, AND THE LOADER'S RULE PORTED INTO IT

An index that is in range and wrong is the larger half, and most of it cannot be
told apart from a genuine zero without knowing what the column means. One class
of it can be: a field *named* Name, Title, Text, SubName or Description - a
field that has to hold an offset into the file's own string block - whose column
holds no such offset in any record.

The first run of that test printed twenty-three rows, and twenty-two of them
were wrong in the same direction. A WotLK client whose locale is not enUS keeps
its strings in the fourth column of each block of sixteen, not the first, and
DBCFile::promoteLocalizedStrings copies each one back to the block's first
column when the table is loaded, before anything reads it. So CharTitles.Title =
2 and Spell.Name = 136 are both correct reads of columns that are zero in the
file, and a sweep that looked only at the file said "answers nothing" about
them twenty-two times. The rule the loader recognises a block by is therefore
ported into this file - the block's first column zero in every record, exactly
one of the rest holding text, and every offset preceded by a terminator - and a
layout naming the first column of a promoted block is not reported.

One row survived, and it is the other way this test is wrong: a name that only
looks like a string. Spell.SpellFamilyName = 208 is, in 3.3.5, the spell's
family *id* - a small integer saying which class's family the spell is in, which
is what the pet and spell-mod matching compares against ChrClasses - and it
became a localised name only in a later expansion. A name's suffix is a hint
about a column, not a proof of one. The guard holds this row at one so that the
second such row gets read rather than waved at.

What the new section did find is not a wrong column, and it is recorded here
because the row that printed it is the only reason it was noticed: CharTitles.dbc
keeps its title text in two columns, 5 and 22, which are the same title twice -
"Pirscher %s" and "Pirscherin %s", the male and the female form. The original
client keeps both and picks per unit by that unit's gender byte; this client
reads one column for everyone, so a female player is named by the male form of
her title - which in German is not a detail, it is a different word. See the
wotlk layout's TitleFemale, and GameHandler::loadTitleNameCache.

WHAT IT CANNOT SEE

An in-range column that holds plausible offsets and is still the wrong one, and
a wrong column whose values happen to be valid offsets. That needs the meaning
of the column, which is what the sibling detector does by comparing rows against
another expansion's file, and what a person does by reading a row they recognise.

It also only reads the DBCs in the directory it was given. A layout for a file
this install does not carry is skipped and counted, not assumed correct - so
nothing here says anything about the TBC layouts' CharTitles entry, because no
TBC copy of that table is on this machine to read a column out of.

ONE EXPANSION AT A TIME

There is one set of DBCs here and four sets of layouts, and a layout only means
anything against the files it was written for - Faction.Name is field 19 in the
Classic file and something else entirely in the WotLK one. Checking all four
against one set of files reported three expansions' worth of noise on the first
run, and the noise looked exactly like findings.

So the layouts are scored against the files and the best-fitting one is the only
one checked. The score is content, not range: a field called Name, Title or
Description has to hold an offset into the file's own string block, and the
layout written for this data is the one whose string fields land there. On the
WotLK files here that separates the four cleanly - the Classic layout puts
Faction.Name at field 19, where the WotLK file keeps something that is not an
offset at all.

Range alone does not separate them. All four layouts have exactly one
out-of-range field, so scoring on that picked whichever sorted first.

The fit does not separate everything either, and the run prints every score so
that is visible rather than implied: TBC and WotLK agree on the string fields
checked here and both reach 100%, while Classic and Turtle fall behind. Where
two tie, the violations reported are the same ones - the layouts differ in the
columns this cannot score, not in the ones it can.
"""
import json
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# The directory of .dbc files on the command line; "Data/db" below is the
# fallback and the string tools/sweep_guard.py looks for - it skips this tool on
# a checkout that has no DBCs, rather than failing it for reporting nothing.
DB = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "Data/db"
EXPANSIONS = ROOT / "Data/expansions"


def field_count(path):
    """The number of columns the file itself declares, or None if not a DBC."""
    with path.open("rb") as fh:
        head = fh.read(20)
    if len(head) < 20 or head[:4] != b"WDBC":
        return None
    _records, fields, _size, _strings = struct.unpack_from("<IIII", head, 4)
    return fields


#: Fields whose value must be an offset into the string block.
#:
#: The Male/Female suffix is part of the name and not of the match: a WotLK
#: CharTitles row carries the same title twice, once per gender, and both
#: columns have to be named for the read to be right. TitleBit deliberately does
#: not match - a bit number is a small integer and would read as a valid offset
#: into the string block, which is how a title id of 1 and a name offset of 1
#: look identical to a test like this one.
STRINGY = re.compile(r"(Name|Title|Description|SubName|Text)(Male|Female)?$")


def records(path):
    """Every record as a tuple of uint32, and the file's own string block."""
    data = path.read_bytes()
    rc, fc, rs, ss = struct.unpack_from("<IIII", data, 4)
    rows = [struct.unpack_from(f"<{fc}I", data, 20 + i * rs) for i in range(rc)]
    start = 20 + rc * rs
    return rows, data[start:start + ss]


def column_stats(rows, block):
    """What the loader decides about every column: all-zero, and text.

    A port of the first half of DBCFile::promoteLocalizedStrings, and the same
    test rather than a similar one: an offset counts only when the byte before
    it is a terminator, which is what keeps a run of ids - 1, 2, 3, every one a
    valid distance into the block, each landing inside somebody else's string -
    from reading as a column of strings. A sweep with its own looser version of
    that test spends its rows explaining the difference.
    """
    width = len(rows[0]) if rows else 0
    all_zero, string_col, has_text = [True] * width, [True] * width, [False] * width
    for row in rows:
        for f, v in enumerate(row):
            if v == 0:
                continue
            all_zero[f] = False
            if not string_col[f]:
                continue
            if v >= len(block) or block[v - 1] != 0:
                string_col[f] = False
            elif block[v] != 0:
                has_text[f] = True
    return all_zero, [s and h for s, h in zip(string_col, has_text)]


def promoted_columns(rows, block):
    """The columns the loader copies a locale's text into before anyone reads them.

    The rest of DBCFile::promoteLocalizedStrings, ported. A WotLK client whose
    locale is not enUS has its strings in the *fourth* column of each block of
    twelve or sixteen, and the loader moves each one back to the block's first
    column at load time. A layout naming that first column is therefore right,
    and a test that only looks at the file would call it dead.
    """
    all_zero, is_text = column_stats(rows, block)
    width = len(all_zero)
    if width < 9:
        return set()

    def block_locale(f, locales):
        if not all_zero[f]:
            return 0
        found = 0
        for l in range(1, locales):
            if all_zero[f + l]:
                continue
            if found or not is_text[f + l]:
                return 0
            found = l
        return found

    def english_block(f, locales):
        if not is_text[f]:
            return False
        return all(all_zero[f + l] for l in range(1, locales))

    for locales, max_locale in ((16, 8), (8, 7)):
        for f in range(0, width - locales + 1):
            if english_block(f, locales):
                return set()               # an English file: nothing is copied
        blocks, votes = [], {}
        f = 0
        while f + locales < width:
            l = block_locale(f, locales)
            for k in range(1, l):
                s = f + k
                if s + locales >= width or not all_zero[s + locales - 1]:
                    break
                if not all_zero[s + locales] and block_locale(s, locales) == l - k:
                    f, l = s, l - k
                    break
            if l and l <= max_locale:
                blocks.append((f, l))
                votes[l] = votes.get(l, 0) + 1
                f += locales + 1
            else:
                f += 1
        if not blocks:
            continue
        locale = 1
        for l in range(2, max_locale + 1):
            if votes.get(l, 0) > votes.get(locale, 0):
                locale = l
        return {start for start, l in blocks if l == locale}
    return set()


def text_at(block, offset):
    """What that offset says - the part a reader checks by eye."""
    end = block.find(b"\0", offset)
    return block[offset:end if end >= 0 else len(block)].decode("latin-1")[:38]


def string_columns(is_text, rows, block):
    """The columns this file keeps text in, with the first record's word.

    Printed beside a finding as an aid to reading it, not as the finding: a
    layout naming one of these is not thereby right, it is only naming a column
    that holds text.
    """
    out = []
    for f, text in enumerate(is_text):
        if not text or not rows:
            continue
        out.append((f, text_at(block, rows[0][f])))
    return out


def dbc_for(name):
    """Data/db is lower case on this install and the layouts are CamelCase."""
    for candidate in (name, name.lower(),
                      re.sub(r"(?<!^)(?=[A-Z])", "_", name).lower()):
        path = DB / f"{candidate}.dbc"
        if path.exists():
            return path
    return None


def main():
    if not DB.is_dir():
        # Every count here comes out of the files. With none there is no field
        # to call out of range and no column to call dead, which is what a set
        # of correct layouts looks like - so stop, and say so, rather than
        # printing zeros and being read as a pass.
        print(f"no DBCs at {DB} - name the directory holding them on the "
              "command line; there is nothing here to compare the layouts to.")
        return 2

    def string_fit(layout_file):
        """How often this layout's string-named fields hold string offsets."""
        good = total = 0
        for dbc_name, fields in json.loads(layout_file.read_text(encoding="utf-8")).items():
            path = dbc_for(dbc_name)
            if not path:
                continue
            count = field_count(path)
            if count is None:
                continue
            named = [i for f, i in fields.items()
                     if isinstance(i, int) and i < count and STRINGY.search(f)]
            if not named:
                continue
            rows, strings = records(path)
            for index in named:
                values = [r[index] for r in rows if r[index] != 0]
                if not values:
                    # A name column holding nothing at all is the finding the
                    # section below reports; here it is left out of the score,
                    # because a column that answers nothing says nothing about
                    # which of four layouts these files belong to.
                    continue
                total += len(values)
                good += sum(1 for v in values if 0 < v < len(strings))
        return (good / total) if total else 0.0

    def dead_strings(layout_file):
        """String-named fields whose column answers no string in any record.

        In range, and wrong: the read cannot fail, it just never says anything.
        """
        found = []
        for dbc_name, fields in sorted(json.loads(
                layout_file.read_text(encoding="utf-8")).items()):
            path = dbc_for(dbc_name)
            if not path:
                continue
            count = field_count(path)
            if count is None:
                continue
            named = [(f, i) for f, i in sorted(fields.items())
                     if isinstance(i, int) and i < count and STRINGY.search(f)]
            if not named:
                continue
            rows, block = records(path)
            if not rows:
                continue
            _zero, is_text = column_stats(rows, block)
            bases = promoted_columns(rows, block)
            for field, index in named:
                if is_text[index]:
                    continue                 # the column does answer text
                if index in bases:
                    # The column is a localised block's enUS slot, and this
                    # file's text sits in one of the columns after it: the
                    # loader copies it here before anything reads the table, so
                    # the layout's read is right and this is not a finding.
                    continue
                why = (f"zero in all {len(rows)} records"
                       if not any(r[index] for r in rows) else
                       f"holds no string offset in any of {len(rows)} records")
                found.append((dbc_name, field, index, why,
                              f"{len(bases)} localised block(s) the loader does "
                              "promote in this file",
                              string_columns(is_text, rows, block)))
        return found

    def violations(layout_file):
        """Out-of-range fields for one expansion's layouts, and how many fit."""
        found, checked, absent = [], 0, 0
        for dbc_name, fields in sorted(json.loads(layout_file.read_text(encoding="utf-8")).items()):
            path = dbc_for(dbc_name)
            if not path:
                absent += 1
                continue
            count = field_count(path)
            if count is None:
                continue
            checked += 1
            for field, index in sorted(fields.items()):
                if isinstance(index, int) and index >= count:
                    found.append((dbc_name, field, index, count))
        return found, checked, absent

    scored = []
    for layout_file in sorted(EXPANSIONS.glob("*/dbc_layouts.json")):
        found, checked, absent = violations(layout_file)
        scored.append((string_fit(layout_file), layout_file.parent.name,
                       found, checked, absent))
    if not scored:
        print("no layouts found")
        return 0

    scored.sort(reverse=True)
    fit, expansion, rows, checked, absent = scored[0]
    layout_file = EXPANSIONS / expansion / "dbc_layouts.json"
    dead = dead_strings(layout_file)
    others = ", ".join(f"{name} {f:.0%}" for f, name, *_ in scored[1:])
    print(f"the DBCs at {DB} best match the {expansion} layouts - "
          f"{fit:.0%} of their string fields land in the string block "
          f"({checked} checked, {absent} with no file here)")
    print(f"the rest fit worse and are not these files': {others}\n")
    print(f"{len(rows)} field(s) naming a column the file does not have:\n")
    for dbc, field, index, count in rows:
        print(f"  {dbc}.{field} = {index}, "
              f"but the file has {count} fields (0-{count - 1})")
    if not rows:
        print("  (none)")

    print(f"\n{len(dead)} field(s) naming a column the file does have, which "
          "answers no string at all:\n")
    for dbc, field, index, why, blocks, candidates in dead:
        print(f"  {dbc}.{field} = {index}: {why}, and it is not a localised "
              f"block's first column ({blocks})")
        # Which columns in that file do hold text, and what the first record
        # says out of each - the read that decides the fix is a row the reader
        # recognises, and this puts one under their eyes.
        print("      holding text here: " +
              (", ".join(f"{i} ({text!r})" for i, text in candidates)
               if candidates else "no column of them"))
    if not dead:
        print("  (none)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
