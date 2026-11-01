#!/usr/bin/env python3
"""Top-level FrameXML windows that nothing has decided about.

Every window the original interface can put on screen has to be exactly one of:

  * **owned** - FrameXML draws it and this client's own render call is gated on
    frameXmlOwns(UiElement::X), or
  * **suppressed** - this client draws it and FrameXML's copy is hidden, or
  * **checked** - named in the takeover file's existence table, which is the
    record of "built or never arrived" that the client prints at start-up. The
    test here is a mention anywhere in that file, which is loose on purpose: a
    name somebody wrote down at all is a name that has been looked at, and the
    C++ says the same about its own table.

Neither means both appear, one on top of the other. That is not hypothetical:
UIErrorsFrame showed every server refusal twice because this client fires
UI_ERROR_MESSAGE and FrameXML's error frame listens for it.

The reason to run this after *any* batch of API work, rather than when something
looks wrong: a window whose functions all return nil stays empty and unnoticed,
and appears the moment they start answering. Finishing GetFactionInfo,
GetTotemInfo and GetTalentPrereqs opened three windows that had been quietly
present all along.

    tools/framexml_window_check.py           # windows nothing has decided about
    tools/framexml_window_check.py --all     # including the ones ignored below
    tools/framexml_window_check.py "<the extracted interface>"

The interface is installed data rather than repository content, so the default
is a path that is often not there. It used to be the only path: with no
interface the run printed "0 top-level windows; 0 accounted for or ignored" and
then listed every suppressed name as one "no XML declares", because there was no
XML - which is a sweep reporting its own blindness as findings, and reporting
coverage as the number zero. It now stops and says so. Naming FrameXML rather
than the folder above it means the same interface; the loader's .toc walk starts
one level up.

Never fails the build: whether an unaccounted window matters depends on whether
this client draws the same thing, which is a judgement about two interfaces
rather than something a regex can settle.

That judgement is now printed with the evidence for it, because a list of 57
bare names is a list nobody reads. Each line says which of four things this
checkout already knows about the window: it is in the load-on-demand list
lua_engine.cpp keeps (that panel answers nil on purpose, so FrameXML's
`if ( not MacroFrame ) then MacroFrame_LoadUI() end` asks for the addon), the
frame name appears in the C++ that feeds it, only its base name's routine
appears (`ChatFrame_` covers ChatFrame7), or nothing anywhere mentions it. Only
the last group is work from scratch.

One correction to make while reading those lines: naming a frame in
framexml_takeover.cpp is what keeps it from being drawn twice. It is *not* what
makes its API calls get answered - that is the missing-API fallback in
lua_engine.cpp, and the load-on-demand list inside it, which is a separate
decision and the reason `__WoweeLoadOnDemandFrames` is checked here at all.
"""

import re
import sys
from pathlib import Path

# The loader is a sibling tool; the interpreter runs with -I, so the tools
# directory has to be named to import it.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from framexml_source import loaded_files  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
TAKEOVER = ROOT / "src" / "ui" / "framexml_takeover.cpp"

_pos = [a for a in sys.argv[1:] if not a.startswith("-")]
# Spelled as one literal, not "Data" / "interface": sweep_guard.py looks for this
# string to decide which tools need an installed interface it does not have here,
# and a path built from two pieces is invisible to it.
INTERFACE = Path(_pos[0]) if _pos else ROOT / "Data/interface"
if INTERFACE.name.lower() == "framexml":
    INTERFACE = INTERFACE.parent
FRAMEXML = INTERFACE / "framexml"


def loaded(suffix):
    """The interface files the client opens, in one kind.

    Every scan here used to glob: FrameXML's own folder for most of them, and
    FrameXML's folder plus `<named path>/../addons/*/*.xml` for two - so naming
    an installed interface still sent the addon half looking in the repository,
    which is not the same interface. The loader walks the .toc chain and reaches
    both halves wherever the tree lives.
    """
    return [p for p in loaded_files(INTERFACE) if p.suffix.lower() == suffix]

# A named, non-virtual frame hung directly off UIParent.
FRAME = re.compile(
    r'<(?:Frame|Button|MessageFrame|ScrollingMessageFrame|ScrollFrame'
    r'|GameTooltip|StatusBar|Slider|EditBox|CheckButton)\b([^>]*)>')
NAME = re.compile(r'name="([^"$]+)"')

# Windows that need no decision, and why. Kept explicit rather than pattern
# matched: "Frame ends in Tooltip" would also swallow something that mattered.
IGNORED = {
    "tooltips this client does not draw": (
        "GameTooltip", "ShoppingTooltip1", "ShoppingTooltip2", "ShoppingTooltip3",
        "ItemRefTooltip", "ItemRefShoppingTooltip1", "ItemRefShoppingTooltip2",
        "ItemRefShoppingTooltip3", "SmallTextTooltip",
    ),
    "dialogs that open only when something asks for them": (
        "StaticPopup1", "StaticPopup2", "StaticPopup3", "StaticPopup4",
        "StackSplitFrame", "CoinPickupFrame", "OpacityFrame",
        "OpacityFrameCloseButton", "PetRenamePopup", "GuildControlPopupFrame",
        "StationeryPopupFrame", "ReadyCheckFrame", "FolderPicker",
        "ChatMenu", "RatingMenuFrame", "DressUpFrame", "AutoCompleteBox",
    ),
    "systems this client has none of": (
        "LFDDungeonReadyPopup", "LFDParentFrame", "LFDRoleCheckPopup",
        "DungeonCompletionAlertFrame1",
        "LFRParentFrame", "PVPParentFrame", "PVPBannerFrame", "ArenaFrame",
        "ArenaRegistrarFrame", "BattlefieldFrame", "VehicleMenuBar",
        "VehicleSeatIndicator", "VoiceChatTalkers", "BNToastFrame",
        "BNetReportFrame", "BNConversationInviteDialog", "FriendsFriendsFrame",
        "MinigameFrame", "MovieProgressFrame", "MacOptionsFrame",
        "MacOptionsCancelFrame", "MacOptionsCompressFrame", "RuneFrame",
        "PetitionFrame", "TabardFrame", "GuildRegistrarFrame", "ChannelPullout",
        "ChannelPulloutTab", "AddFriendFrame", "TradeFrame", "StatsFrame",
    ),
    "drawn only on an event this client never fires": (
        "RaidBossEmoteFrame", "RaidWarningFrame", "AutoFollowStatus",
        "QuestTimerFrame", "TutorialFrame", "TutorialFrameAlertButton",
        "AlertFrame", "WorldStateAlwaysUpFrame", "SubZoneTextFrame",
        "ZoneTextFrame", "ChatConfigFrame", "PartyMemberBackground",
        "ConsolidatedBuffs", "TemporaryEnchantFrame",
    ),
}
IGNORED_NAMES = {n for group in IGNORED.values() for n in group}


INHERITS = re.compile(r'inherits="([^"]+)"')


def uiParentTemplates():
    """Virtual templates that put whatever inherits them on UIParent.

    A frame does not have to say parent="UIParent" itself. GroupLootFrame1
    through 4 say only inherits="GroupLootFrameTemplate", and it is the
    template that names the parent - so looking for the literal attribute
    walked straight past four top-level windows, which then drew beside this
    client's own roll dialog with nothing reporting it.
    """
    templates = {}          # name -> (onUIParent, inheritedNames)
    for path in sorted(loaded(".xml")):
        text = path.read_text(errors="ignore")
        for attrs in FRAME.findall(text):
            if 'virtual="true"' not in attrs:
                continue
            name = NAME.search(attrs)
            if not name:
                continue
            inherited = INHERITS.search(attrs)
            templates[name.group(1)] = (
                'parent="UIParent"' in attrs,
                [n.strip() for n in inherited.group(1).split(",")] if inherited else [],
            )

    # A template may inherit the parent from another template; settle it.
    def resolve(name, seen=()):
        if name not in templates or name in seen:
            return False
        onParent, parents = templates[name]
        return onParent or any(resolve(p, seen + (name,)) for p in parents)

    return {n for n in templates if resolve(n)}


def topLevelWindows():
    onUIParent = uiParentTemplates()
    found = {}
    for path in sorted(loaded(".xml")):
        text = path.read_text(errors="ignore")
        for attrs in FRAME.findall(text):
            if 'virtual="true"' in attrs:
                continue
            if 'parent="UIParent"' not in attrs:
                inherited = INHERITS.search(attrs)
                if not inherited:
                    continue
                if not any(n.strip() in onUIParent
                           for n in inherited.group(1).split(",")):
                    continue
            name = NAME.search(attrs)
            if name:
                found.setdefault(name.group(1), path.name)
    return found


def declaredNames():
    """Every frame or region name the XML declares, and the $parent suffixes."""
    plain, suffixes = set(), set()
    for path in sorted(loaded(".xml")):
        text = path.read_text(errors="ignore")
        for n in re.findall(r'name="([^"]+)"', text):
            if n.startswith("$parent"):
                suffixes.add(n[len("$parent"):])
            else:
                plain.add(n)
    return plain, suffixes


def checkSuppressionNames():
    """Names listed for suppression that no XML declares.

    A name invented here suppresses nothing and reports NOT BUILT forever, which
    looks identical to a frame that simply never opens. Names built from
    $parent are composed at run time, so a listed name counts as real when it is
    some declared frame followed by a declared $parent suffix.
    """
    plain, suffixes = declaredNames()

    # Frames the Lua builds by name at run time: BuffButton1 comes from
    # CreateFrame("Button", "BuffButton"..i, ...) and no XML mentions it.
    builtPrefixes = set()
    for path in loaded(".lua"):
        text = path.read_text(errors="ignore")
        builtPrefixes |= set(re.findall(
            r'CreateFrame\(\s*"[^"]+"\s*,\s*"([A-Za-z]\w*)"\s*\.\.', text))
        # BuffButton1 is reached as _G["BuffButton"..i]; the frame itself was
        # created from a variable, so the prefix only appears at the lookup.
        builtPrefixes |= set(re.findall(r'_G\[\s*"([A-Za-z]\w*)"\s*\.\.', text))

    def composed(name):
        """A $parent name, possibly nested: TargetFrame + TextureFrame + Name."""
        seen = set()
        while name and name not in seen:
            if name in plain:
                return True
            seen.add(name)
            longest = ""
            for suffix in suffixes:
                if suffix and name.endswith(suffix) and len(suffix) > len(longest):
                    longest = suffix
            if not longest:
                return False
            name = name[: -len(longest)]
        return False

    listed = []
    for m in re.finditer(r'\{UiElement::(\w+),\s*((?:"[^"]*"\s*)+)', TAKEOVER.read_text(encoding="utf-8")):
        blob = " ".join(re.findall(r'"([^"]*)"', m.group(2)))
        listed += [(n, m.group(1)) for n in blob.split() if n and n[0].isupper()]

    unknown = []
    for name, element in listed:
        if name in plain:
            continue
        if composed(name):
            continue                      # composed from $parent at run time
        if any(name.startswith(pre) for pre in builtPrefixes):
            continue                      # named by the Lua as it creates it
        unknown.append((name, element))
    return listed, unknown


CPP_ROOTS = (ROOT / "src", ROOT / "include")


def deferredGlobals():
    """Panels this client deliberately answers nil, name -> where it says so.

    lua_engine.cpp keeps `__WoweeLoadOnDemandFrames`, the names FrameXML asks
    for behind an `if ( not MacroFrame ) then MacroFrame_LoadUI() end` guard.
    Every other unknown PascalCase global answers a no-op object, which would
    make that guard read as "already loaded" and leave the panel never asked
    for; these names answer nil instead, which is what makes the addon load and
    the window appear. A name here is therefore a decided window - FrameXML's,
    by design - and it is the strongest of the three pieces of evidence, so it
    gets its own line rather than being counted as an incidental mention.

    This is the hand-written floor only. The list that runs is that plus
    everything the addons on disk declare, added at start-up by
    declareDeferredGlobals, so a name missing from here is not undecided - it
    may be in the derived half, which no Python run can see without the
    interface the client loaded.
    """
    out = {}
    for base in CPP_ROOTS:
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*.cpp")):
            lines = path.read_text(encoding="utf-8",
                                   errors="ignore").splitlines()
            rel = path.relative_to(ROOT).as_posix()
            # Read the block, not `Name = true` anywhere: the table's own names
            # do not all end in Frame - BattlefieldMinimap and StopwatchTicker
            # are in it, and a suffix rule dropped them while crediting
            # unrelated `...Frame = true` fields elsewhere in the tree.
            for start, line in enumerate(lines):
                if "__WoweeLoadOnDemandFrames = {" not in line:
                    continue
                for i in range(start + 1, min(start + 12, len(lines))):
                    for name in re.findall(r'\b([A-Za-z_]\w*)\s*=\s*true', lines[i]):
                        out.setdefault(name, (rel, i + 1))
                    if re.search(r'\}\s*\\?n?"', lines[i]):
                        break
                break
    return out


def clientCodeEvidence(names):
    """What this client's own code says about each undecided window.

    The takeover file is not the only place a decision can show up. A good part
    of these windows this client does not draw but *feeds*: the C++ names the
    frame or the routine behind it - `OpenMail_Update` for OpenMailFrame,
    `GroupLootFrame_OpenNewFrame` for GroupLootFrame1, `MirrorTimer_Hide` for
    MirrorTimer1 - because answering the API call is what makes FrameXML's copy
    work. Whoever decides "does this client draw one" has to find that line to
    answer it at all, and looking for 57 names by hand is what nobody does.

    Three ways a line can be about one of these windows, strongest first:

      the name as a word           - `hides OpenMailFrame when not` (a comment
                                     about this frame, or a lookup of it)
      the name followed by `_`     - the window's own routine:
                                     `ItemSocketingFrame_LoadUI`,
                                     `GroupLootFrame_OpenNewFrame`
      its base name followed by `_` - the shared routine a numbered sibling is
                                     one instance of: `ChatFrame_` for
                                     ChatFrame7, `PartyMemberFrame_` for
                                     PartyMemberFrame3

    The first is anchored at both ends; without the far end, `ArenaEnemyFrames`
    is credited to the `showArenaEnemyFrames` cvar row, which says nothing about
    the frame. Only the last one is weak, and it says so on the line.
    """
    deferred = deferredGlobals()
    stems = {n: re.sub(r"\d+$", "", n) for n in names}

    def alt(items, tail):
        return re.compile(r"\b(" + "|".join(
            sorted({re.escape(i) for i in items}, key=len, reverse=True)) + tail)

    exactRe = alt(names, r")\b")
    ownRe = alt(names, r")_")
    bases = {s for n, s in stems.items() if s != n}
    baseRe = alt(bases, r")_") if bases else None

    exact, own, base = {}, {}, {}
    for rootDir in CPP_ROOTS:
        if not rootDir.is_dir():
            continue
        for path in sorted(rootDir.rglob("*")):
            if path.suffix.lower() not in (".cpp", ".hpp", ".inc"):
                continue
            text = path.read_text(encoding="utf-8", errors="ignore")
            if not (exactRe.search(text) or ownRe.search(text)
                    or (baseRe and baseRe.search(text))):
                continue
            rel = path.relative_to(ROOT).as_posix()
            for i, line in enumerate(text.splitlines(), 1):
                for m in exactRe.finditer(line):
                    name = m.group(1)
                    if deferred.get(name) == (rel, i):
                        continue      # its own load-on-demand row, tier one
                    exact.setdefault(name, []).append((rel, i))
                for m in ownRe.finditer(line):
                    own.setdefault(m.group(1), []).append((rel, i))
                if baseRe:
                    for m in baseRe.finditer(line):
                        for n, s in stems.items():
                            if s == m.group(1) and s != n:
                                base.setdefault(n, (rel, i))
    return exact, own, base


def main():
    showAll = "--all" in sys.argv
    if not INTERFACE.is_dir():
        # Every number here is a count of frames read out of XML. With no
        # interface this function used to print "0 top-level windows; 0
        # accounted for or ignored" and then list all 155 suppressed names as
        # ones "no XML declares" - the same run reading nothing and calling it a
        # finding, which is how a blind sweep keeps its green.
        print(f"no interface at {INTERFACE} - name the extracted interface "
              "directory on the command line. Nothing was read, so nothing "
              "below would mean anything.")
        return 1
    if not TAKEOVER.is_file():
        print(f"no takeover file at {TAKEOVER}")
        return 0

    decided = TAKEOVER.read_text(encoding="utf-8")
    windows = topLevelWindows()

    # A ContainerFrame is a bag, and the bags element covers all thirteen of
    # them however many the client happens to open.
    def accounted(name):
        if name in decided:
            return True
        return name.startswith("ContainerFrame") and "ContainerFrame" in decided

    unknown = {n: f for n, f in windows.items() if not accounted(n)}
    if not showAll:
        unknown = {n: f for n, f in unknown.items() if n not in IGNORED_NAMES}

    print(f"{len(windows)} top-level windows; "
          f"{len(windows) - len(unknown)} accounted for or ignored\n")
    if not unknown:
        listed, bad = checkSuppressionNames()
        if bad:
            print(f"{len(listed)} names listed for suppression; "
                  f"{len(bad)} that no XML declares:")
            for name, element in bad:
                print(f"    {name:<38} {element}")
            print("    A name nothing declares suppresses nothing.")
        else:
            print("Nothing undecided, and every suppressed name is a real frame.")
        return 0

    listed, badNames = checkSuppressionNames()
    if badNames:
        print(f"{len(listed)} frame names listed for suppression; "
              f"{len(badNames)} that no XML declares:")
        for name, element in badNames:
            print(f"    {name:<38} {element}")
        print("    A name nothing declares suppresses nothing.\n")

    # Counted, so the list can be graded by a number rather than by someone
    # reading it: it is long, and "how long" is the only question a check can
    # answer without judgement.
    exact, own, base = clientCodeEvidence(unknown)
    deferred = deferredGlobals()
    print(f"{len(unknown)} neither owned nor suppressed - "
          "check whether this client draws one:")
    tiers = {}
    for name, source in sorted(unknown.items()):
        where = exact.get(name)
        if name in deferred:
            rel, line = deferred[name]
            evidence, tier = f"answers nil so its addon loads - {rel}:{line}", "nil"
        elif where:
            extra = f" (+{len(where) - 1} more)" if len(where) > 1 else ""
            evidence, tier = f"named in {where[0][0]}:{where[0][1]}{extra}", "named"
        elif name in own:
            rel, line = own[name][0]
            evidence, tier = f"named by its own routine, {rel}:{line}", "named"
        elif name in base:
            rel, line = base[name]
            evidence, tier = f"only its base name's routine, {rel}:{line}", "near"
        else:
            evidence, tier = "nothing in this client's code mentions it", "none"
        tiers[tier] = tiers.get(tier, 0) + 1
        print(f"    {name:<32} {source:<30} {evidence}")

    # Counted by strength so "is this list worth reading today" stays a number
    # rather than a judgement: the nil-answered ones are decided by design, the
    # named ones are fed by code that says which window it feeds, and only the
    # last two groups are work.
    print(f"\n{tiers.get('nil', 0)} answered nil so their addon loads, "
          f"{tiers.get('named', 0)} named by the code that feeds them, "
          f"{tiers.get('near', 0)} by a shared routine only, "
          f"{tiers.get('none', 0)} nowhere - the last two groups are the ones "
          "that still need a decision.")
    print("\nIf this client draws the same thing, name the frame in "
          "framexml_takeover.cpp\nand gate the client's render call on "
          "frameXmlOwns. If it does not, add it to\nIGNORED here with the "
          "evidence line as the reason, so the next run stays\nshort. A window "
          "in the 'nowhere' group is one of two things: a feature\nthis client "
          "has no code for at all, or one its code feeds without ever\nnaming "
          "the frame - and which of the two is what the next line of work is.")
    return 0


def checkInherits():
    """Every inherits= naming something the XML never declares.

    A frame that inherits a template which is not there loses every part the
    template would have given it - its art, its regions, its scripts - and
    says nothing. Same for a font string naming a font object that does not
    exist: the text draws in whatever the default is.

    Nothing else here would catch it. The emitter copies the name through, the
    compile check only asks whether the Lua parses, and a frame with no parts
    looks like a frame whose parts failed to draw.

    inherits= names either a virtual frame template *or* a virtual FontString
    template *or* a <Font> object, so all three count as declarations -
    counting only <Font> reported four false positives on the first run.
    """
    FRAME_TAGS = (r'(?:Frame|Button|CheckButton|StatusBar|Slider|EditBox'
                  r'|ScrollFrame|ScrollingMessageFrame|MessageFrame|GameTooltip'
                  r'|Model|PlayerModel|DressUpModel|TabardModel|ColorSelect'
                  r'|SimpleHTML|Cooldown|QuestPOIFrame|Minimap|MovieFrame'
                  r'|FontString)')
    xmls = sorted(loaded(".xml"))
    if not xmls:
        # The window half of this file stops and says so when there is no
        # interface to read. This half went on to print
        # "0 templates and font objects inherited; 0 declared.
        # Every inherits= resolves." - a green verdict from a run that had
        # opened no file, on the same command line that had just been told
        # nothing was read.
        print("\nno XML to read - the inherits check has no verdict.")
        return
    declared, referenced = set(), {}
    for path in xmls:
        text = path.read_text(errors="ignore")
        declared |= set(re.findall(r'<Font\b[^>]*name="([^"]+)"', text))
        for m in re.finditer(r'<' + FRAME_TAGS + r'\b([^>]*)>', text):
            attrs = m.group(1)
            if 'virtual="true"' not in attrs:
                continue
            n = re.search(r'name="([^"]+)"', attrs)
            if n:
                declared.add(n.group(1))
    for path in xmls:
        text = re.sub(r"<!--.*?-->", "", path.read_text(errors="ignore"), flags=re.S)
        for m in re.finditer(r'<' + FRAME_TAGS + r'\b([^>]*)>', text):
            inh = re.search(r'inherits="([^"]+)"', m.group(1))
            if not inh:
                continue
            for name in inh.group(1).split(","):
                referenced.setdefault(name.strip(), []).append(path.name)
    missing = {n: f for n, f in referenced.items() if n not in declared}
    print(f"\n{len(referenced)} templates and font objects inherited; "
          f"{len(declared)} declared.")
    if not missing:
        print("Every inherits= resolves.")
        return
    print("\nInherited but never declared - the frame loses everything the "
          "template would have given it:")
    for name, files in sorted(missing.items(), key=lambda kv: -len(kv[1])):
        print(f"    {name:<38} {len(files)} uses, e.g. {files[0]}")


if __name__ == "__main__":
    code = main()
    checkInherits()
    sys.exit(code)
