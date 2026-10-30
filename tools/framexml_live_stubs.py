#!/usr/bin/env python3
r"""Stubbed bindings that a handed-over element actually calls.

The readiness report counts a name as answered once it is bound, and a stub is
bound - so `HasPetSpells -> lua_ReturnNil` read as clean while it was hiding
the entire pet spell book. This asks the narrower question: which of the stubs
are on a code path that is on screen right now.

Being listed is not being wrong. Plenty of stubs are correct: the feature is
genuinely absent (vehicles, voice chat) or FrameXML tolerates the empty answer.
It is a reading list, ordered by how central the file is.

WHAT IT COULD NOT SEE UNTIL 2026-08-06

A stub written out in place. Only the named shared ones were recognised -
lua_ReturnNil and its family - so a lambda answering a literal from inside the
registration table counted as an implementation, which is exactly what a stub
is not. The count went from thirty-nine to a hundred and ninety-five.

The test is now that stripping the pushes and the return leaves nothing that
calls anything. Asking it the other way round - which sources does this body
read - kept failing in the same direction: GetActionBarPage keeps its page in a
Lua global, GetSelectedFaction and GetSelectedSkill keep theirs in C++ statics
behind an accessor, and all three are implementations that simply do not go
through the game handler.

Two of the newly visible are worth naming: GetQuestLogCompletionText answers ""
and GetQuestLogRequiredMoney answers 0, both to the quest log and the tracker.
Answering them needs SMSG_QUEST_QUERY_RESPONSE parsed further than it is - the
quest log entry carries neither field, and the packet that does carry them is
the turn-in one, which arrives only once the player is at the NPC.

THE FORTY THAT WERE CHECKED. The hundred and fifty-six that joined them have
not been.

THE FORTY IT REPORTS TODAY, ALL CHECKED

None is a defect, but three are worth knowing about because they are visible
rather than merely absent. Read once so the count can be watched rather than
re-triaged.

VISIBLE, AND DELIBERATE

  * GetBattlefieldInstanceRunTime - the scoreboard prints "Time Elapsed: 0
    seconds" rather than nothing, because worldstateframe.lua formats whatever
    it is given. The real answer is milliseconds since the *instance* started,
    which this client cannot know: it can measure since the player joined, and
    labelling that "Time Elapsed" would be confidently wrong for anyone who
    arrived late. A zero that reads as broken is better than a number that
    reads as true.
  * ShowContainerSellCursor, ShowBuybackSellCursor - the cursor does not change
    to the sell icon over a bag item at a vendor. This client's cursor has no
    such icon to change to.

ONE THAT IS ABSENT BY CHOICE RATHER THAN BY NECESSITY

The GM survey. Its questions come from four DBCs this install carries, not from
any packet - GMSurveyCurrentSurvey maps language to survey, GMSurveySurveys
lists the question ids, GMSurveyQuestions and GMSurveyAnswers hold the text -
and the trigger is the getSurvey byte in SMSG_GMRESPONSE_STATUS_UPDATE.
Submitting is what makes it work rather than merely appear, and that means
accumulating ten answers with per-question comments for CMSG_GMSURVEY_SUBMIT.
The panel is only reached after a game master closes a ticket.

Recorded because this file previously said the questions were unparseable,
which was read off the event's name rather than off the files sitting beside
it.

TWO THAT WERE CHECKED AND ARE GENUINELY ABSENT

Written down because three neighbouring claims of the same kind turned out to
be wrong - the refund window, the GM survey and vehicle state were all called
absent and all three were reachable. These two are not.

  * Voice chat. AzerothCore's handlers read the request and throw it away:
    HandleVoiceSessionEnableOpcode is two read_skips, HandleSetActiveVoiceChannel
    another two, HandleChannelVoiceOnOpcode an empty body with a comment. No
    SMSG_VOICE_* is ever sent, so there is no session to report on and nothing
    a binding could answer from.
  * Movie recording. The renderer can capture one frame - Renderer::captureScreenshot
    writes a PNG - and there is no encoder behind it. MovieRecording_* is video.

ABSENT FEATURES, WHICH IS WHAT THE STUB SAYS (28)

The world map's debug objects, zone map, battlefield flag and vehicle
positions, dungeon map floors and Wintergrasp timer; Battle.net and its friend
list; voice chat and mutes; movie recording; mail stationery; achievement
comparison; arena opponents; the multi-cast bar's offset; addon memory usage.
Each is a feature this client does not have, and the stub is the honest shape
of that.

CORRECT ANSWERS THAT LOOK LIKE STUBS (9)

IsMacClient is false because it is not one. GetAdjustedSkillPoints is zero
because WotLK has no skill points - skillframe.lua gates every purchase verb
on it, which is why BuySkillTier and AddSkillUp are unreachable rather than
unimplemented. GetCurrentMapDungeonLevel is zero because a map with no floors
is on floor zero.

THREE THAT WERE STUBS AND ARE NOT (2026-10-30)

GetUnitHealthModifier, GetUnitPowerModifier and GetUnitMaxHealthModifier all
answered a bare 1.0 and all three are answered from the unit now. The client
gates them identically - the unit is the local player, or it has a
UNIT_FIELD_PETNUMBER, and anything else pushes *zero* - and then: max health is
the unit's own UNIT_FIELD_MAXHEALTHMODIFIER float plus one (0x00612870 at field
blob +0x22c), and the other two answer one only for a hunter's pet. The client's
non-hunter leg is not a feature this one lacks: it reads the two floats at
+0x34 and +0x38 of the per-entry creature record a query response fills (lookup
0x0067b6a0, memory-pool label "\AUDBCACHECALLBACK\", keyed by the entry id, and
its first test is `key == 0` - which is why a player answers zero). A 3.3.5
creature query carries no float field at all, so those two reads reinterpret an
id, and a small integer read as IEEE 754 is a denormal: zero, on both sides.

The multiplier field is a wire field after all; an earlier reading here had
written it off as client-internal. The client's *unit* descriptor space runs
contiguous from blob dword 0 up to 141 with no gaps, and its last three records
name 139/140/141 MAXHEALTHMODIFIER, HOVERHEIGHT and PADDING - the three tail
unit fields against UNIT_END at 148 - while the record below them,
POWER_COST_MULTIPLIER at blob 132 covering seven dwords, is this repository's
own "138". Two spaces are the trap: the *player* space has its own record at blob
dword 139, and it is PLAYER_VISIBLE_ITEM_3_ENTRYID.

Visible on the pet paperdoll's stat tooltip, whose only use these are: a
warlock pet or a death knight's ghoul now says its stamina changes nothing,
which is what the original client says, and a hunter's pet keeps its one.

Recorded so the three are not re-triaged, and so that record is looked for by
its real name next time - it is the answer to a creature query, which is what
this repository already keeps in its creature info cache, and not a row of a
table. The pool label that made it look like a DBC row is the reason that was
wrong for as long as it was.

A COMMENT IS NOT A CALL (2026-10-30)

The file text was grepped raw, so a name mentioned only inside a commented-out
block counted as reached from a live element. Two rows went, and both are things
3.3.5 itself deleted rather than anything here: petframe.lua's pet-expiry flash
block (GetPetTimeRemaining, mentioned twice inside `--`), and the old honour
progress bar in inspecthonorframe.lua (GetInspectPVPRankProgress, once). Lua
comments are
blanked before matching now, long-bracket ones consumed whole and strings left
alone, and `<!-- -->` in the markup files the same way. 173 stubs, 74 reached.

ONE THAT IS A QUERY AND NOT A VERB

ForceGossip reads like an instruction and is a question. FUN_0058ab50 casts the
current gossip target's guid pair to a unit (the same checked cast type 8 that
the hover-height watcher uses), and answers bit 27 of the flag dword at +0xc of
the record hung off that unit at +0x964 - the same per-entry creature record the
pet stat modifiers read at +0x34 and +0x38, and the same accessor family: bits
2, 4 (inverted), 19, 22, 25, 26 and 27 are each one one-line getter.

gossipframe.lua:12 uses it as `not ForceGossip()` inside the test that clicks a
lone gossip option for you when there are no quests and one option. So a definite
no - which is what this client answers - auto-selects, and the original
auto-selects too for every NPC whose query answer does not set bit 27. That is
the error direction the stub had backwards, and it is closed: the record is the
SMSG_CREATURE_QUERY_RESPONSE answer kept per entry, the +0xc flag dword is the
typeFlags this repository already parses, and bit 27 is ForceGossip. What settled
that it is a query answer and not a row: fingerprinting all 246 tables for the
record's shape - a bit field at column 3 and two small floats at columns 13 and
14 - left nine near misses, none of them keyed by a creature entry: they are the
sound and spell-chain tables, a vehicle seat, a map overlay, a destructible
model, a durability level and a scaling-stat distribution. A table this install
were short of would have shown up there under a creature-template name.

FIVE OF THE REACHED ROWS ARE NOT ASKS FROM THE REFERENCE CLIENT (2026-10-30)

The interface tree named on the command line is not the 12340 client's own UI.
It is the post-Cataclysm layout under a 30300 tag: 115 of its FrameXML XML files
hold <Frame> definitions, they point at ..\FrameXML\UI.xsd, and a 3.3.5 FrameXML
carries no frame XML at all. So "reached from a live element" is a superset of
what the reference client ever reached, and the binary is what separates the two
halves: take the second column of the decompile's strings.tsv, sort it, and
`comm -23` it against these rows. Five of the 74 come out with no match:
AchievementMicroButton_Update (called at blizzard_achievementui.lua:671),
MutedList_Update (friendsframe.lua:1341), CannotBeResurrected,
InspectAchievements and MovieRecording_IsRecording. Nothing in the tree defines
the first three, so that interface is asking the client for functions this client
never had. The fourth is the row the other way round: uiparent.lua:346 defines
InspectAchievements itself, so its own Lua function replaces the native binding
at load and no stub of ours is ever what runs - the binding still matters for a
tree that does not define it, which is why it is bound rather than absent. The
other
69 are names the 12340 executable does carry, and those are the rows worth
reading the decompile over; the rest are a newer UI asking for something the
reference client never had, and chasing them against this binary is a dead end
by construction.
"""
import re
import sys
from pathlib import Path

# The repository this file sits in, and the interface to read.
#
# ROOT was one contributor's absolute home directory, so these eight sweeps ran
# on exactly one machine and silently read nothing anywhere else - loaded_files
# on a directory that is not there returns an empty set, and a sweep with no
# input reports a clean tree.
#
# The interface directory can be named on the command line, because the one
# under Data/ is whichever expansion was extracted last and the question these
# answer is usually about a particular one:
#
#     python3 tools/framexml_live_stubs.py ~/wow-1.12/interface
ROOT = Path(__file__).resolve().parent.parent
XML = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "Data/interface"
if not XML.is_dir():
    print(f"no interface at {XML} - name one on the command line")
    raise SystemExit(2)

STUBS = {"lua_ReturnNil", "lua_ReturnZero", "lua_ReturnFalse", "lua_ReturnNothing",
         "lua_ReturnTrue", "lua_ReturnEmptyString", "lua_ContainerNoOp",
         "lua_ContainerFalse", "lua_NoOp", "lua_Noop"}

# The FrameXML files that belong to a handed-over element, which is all of them.
# Derived from the takeover file rather than written out, and mapped to files
# through the readiness tool's table, plus the shared files every panel goes
# through.
#
# It was a hand-made list and twice that was the bug - it named
# paperdollframe.lua as "the character sheet" and missed the other four
# subframes, and it covered only the defaults while the candidates tier was
# what was actually on screen. Then it read a defaults list and a candidates
# tier out of the source, and both of those went with WOWEE_FRAMEXML_UI - which
# nothing noticed, because ROOT was one machine's home directory and this sweep
# had not run anywhere else in a long time.
def _live_files():
    import re as _re
    tk = (ROOT / "src/ui/framexml_takeover.cpp").read_text(encoding="utf-8")
    if "return true;" not in tk:
        raise SystemExit("frameXmlOwns no longer answers unconditionally - this "
                         "sweep assumes every element in the table is handed over")
    elements = set(_re.findall(r'\{UiElement::\w+,\s*"([a-z]+)"', tk))
    if not elements:
        raise SystemExit("no elements found in framexml_takeover.cpp - this reads "
                         "the element table")
    rd = (ROOT / "tools/framexml_element_readiness.py").read_text(encoding="utf-8")
    # Parsed as data, not run as code. These are two plain dict literals in a
    # sibling sweep, and exec() on a slice of another file is both more than
    # this needs and a finding in its own right: it hands whatever that regex
    # happened to match to the interpreter. literal_eval accepts a literal and
    # nothing else, so a stray call or import in there is an error rather than
    # something that runs.
    import ast as _ast
    ns = {}
    for _name in ("ELEMENTS", "ADDON_ELEMENTS"):
        _m = _re.search(r"^%s = (\{.*?^\})" % _name, rd, _re.S | _re.M)
        if not _m:
            raise SystemExit(f"{_name} is no longer a dict literal in "
                             "framexml_element_readiness.py - this sweep reads it as one")
        ns[_name] = _ast.literal_eval(_m.group(1))
    out = {}
    for el in elements:
        for f in ns["ELEMENTS"].get(el, []):
            out[f] = el
        addon = ns["ADDON_ELEMENTS"].get(el)
        if addon:
            d = XML / "addons" / addon
            if d.exists():
                for p in d.rglob("*"):
                    if p.suffix in (".lua", ".xml"):
                        out[p.name] = el
    for f, el in {
            "uiparent.lua": "dialogs", "staticpopup.lua": "dialogs",
            "unitpopup.lua": "unitframes", "unitframe.lua": "unitframes",
            "targetframe.lua": "targetframe", "playerframe.lua": "playerframe",
            "actionbutton.lua": "mainmenubar", "bonusactionbarframe.lua": "mainmenubar",
            "mainmenubar.lua": "mainmenubar", "mainmenubarbagbuttons.lua": "bagbar",
            "containerframe.lua": "bags", "paperdollframe.lua": "characterframe",
            "skillframe.lua": "characterframe/skills",
            "reputationframe.lua": "characterframe/rep",
            "tokenframe.lua": "characterframe/currency",
            "petpaperdollframe.lua": "characterframe/pet",
            "characterframe.lua": "characterframe", "spellbookframe.lua": "spellbook",
            "petframe.lua": "petframe", "petactionbarframe.lua": "petframe",
            "buffframe.lua": "buffs", "castingbarframe.lua": "castbar",
            "durabilityframe.lua": "durability", "zonetext.lua": "zonetext",
            "minimap.lua": "minimap", "mirrortimer.lua": "playerframe",
            "focusframe.lua": "focusframe", "gametooltip.lua": "tooltips",
            "itembuttontemplate.lua": "shared"}.items():
        out.setdefault(f, el)
    return out


OWNED = _live_files()

_LONG_OPEN = re.compile(r"\[(=*)\[")


def _strip_lua_comments(text):
    """Blank out Lua comments, leaving strings alone.

    A commented-out call is not a call: petframe.lua mentions
    GetPetTimeRemaining twice and both mentions are inside a `--` block left
    over from the expansion that removed the pet expiry bar. Long-bracket
    comments (`--[[ ]]`, `--[==[ ]==]`) have to be consumed whole or a `]]` in
    one ends the comment early, and the string cases have to be skipped or a
    `"a -- b"` hides every call after it on that line.
    """
    out, i, n = [], 0, len(text)
    while i < n:
        ch = text[i]
        if text.startswith("--", i):
            m = _LONG_OPEN.match(text, i + 2)
            if m:
                close = "]" + m.group(1) + "]"
                k = text.find(close, m.end())
                i = n if k < 0 else k + len(close)
            else:
                k = text.find("\n", i + 2)
                i = n if k < 0 else k
            out.append(" ")
            continue
        if ch in "\"'":
            j = i + 1
            while j < n and text[j] != ch and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
            continue
        if ch == "[":
            m = _LONG_OPEN.match(text, i)
            if m:
                close = "]" + m.group(1) + "]"
                k = text.find(close, m.end())
                j = n if k < 0 else k + len(close)
                out.append(text[i:j])
                i = j
                continue
        out.append(ch)
        i += 1
    return "".join(out)


def _strip_xml_comments(text):
    """Blank out `<!-- -->`, which is the same mistake in the markup files."""
    out, i = [], 0
    while True:
        k = text.find("<!--", i)
        if k < 0:
            out.append(text[i:])
            return "".join(out)
        e = text.find("-->", k + 4)
        out.append(text[i:k])
        i = len(text) if e < 0 else e + 3


# A comment is not a call, and both file kinds have one.
STRIP = {".lua": _strip_lua_comments, ".xml": _strip_xml_comments}

bound = {}
inline_stub = {}
for f in (ROOT / "src/addons").glob("*.cpp"):
    s = f.read_text(errors="ignore")
    for m in re.finditer(r'\{(?:\.\w+\s*=\s*)?"([A-Za-z0-9_]+)",\s*(?:\.\w+\s*=\s*)?(?:&)?\s*(lua_[A-Za-z0-9_]+)\}', s):
        bound[m.group(1)] = m.group(2)
    # The inline form. A binding registered as a lambda in the table is the
    # same binding to Lua, but only the named shared stubs - lua_ReturnNil and
    # its family - were recognised here, so a stub written out in place was
    # counted as an implementation. It is a stub by the same test: it never
    # reaches the game and never reads what it was passed, so it answers the
    # same thing every time it is called.
    for m in re.finditer(r'\{(?:\.\w+\s*=\s*)?"([A-Za-z0-9_]+)",\s*(?:\.\w+\s*=\s*)?\[\]\(lua_State\*\s*L?\s*\)\s*->\s*int\s*\{', s):
        depth, i = 1, m.end()
        while i < len(s) and depth:
            if s[i] == "{":
                depth += 1
            elif s[i] == "}":
                depth -= 1
            i += 1
        body = s[m.end():i - 1]
        name = m.group(1)
        bound.setdefault(name, name)
        # A stub answers with literals and consults nothing to do it. Asking
        # instead which *sources* a body reads kept getting this wrong in the
        # same direction: GetActionBarPage keeps its page in a Lua global,
        # GetSelectedFaction and GetSelectedSkill in C++ statics behind an
        # accessor, and all three are implementations that simply do not go
        # through the game handler. So the test is the other way round - strip
        # the pushes and the return, and a stub has nothing left that calls
        # anything.
        # Only the call *token* is removed, not what it was passed - otherwise
        # lua_pushnumber(L, selectedFaction()) loses the accessor inside it and
        # reads as a literal. luaReturnNil goes with them: it is how a binding
        # says nil, not something it consults.
        rest = re.sub(r"lua_push\w*\s*\(|luaReturnNil\s*\(|\breturn\b|\(void\)\s*L\s*;",
                      "", body)
        if not re.search(r"\w+\s*\(", rest):
            inline_stub[name] = "(inline)"

stubbed = {n for n, impl in bound.items() if impl in STUBS} | set(inline_stub)

hits = {}
for fname, element in OWNED.items():
    matches = list(XML.rglob(fname))
    if not matches:
        continue
    text = matches[0].read_text(errors="ignore")
    strip = STRIP.get(matches[0].suffix)
    if strip:
        text = strip(text)
    for name in stubbed:
        if re.search(rf"\b{re.escape(name)}\s*\(", text):
            hits.setdefault(name, []).append(f"{element}:{fname}")

print(f"{len(bound)} bindings, {len(stubbed)} of them stubs, "
      f"{len(hits)} reached from a handed-over element\n")
for name in sorted(hits, key=lambda n: (-len(hits[n]), n)):
    print(f"  {name:<34} {bound[name]:<20} {', '.join(sorted(set(hits[name])))}")
