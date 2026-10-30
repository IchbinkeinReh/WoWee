#!/usr/bin/env python3
"""Events the interface waits for that this client never sends.

The companion to framexml_element_readiness.py, and the more productive of the
two. That one asks whether an element's calls are answered - a call that is not
raises, so the gap is loud. This asks whether its events arrive, and the failure
is silent: the frame simply sits there, or shows what it was drawn with, or
never opens at all.

    tools/framexml_event_gap.py "<the extracted interface>"

The interface is installed data rather than repository content, so the default
path below is often not where it is. A missing directory is not the same as an
empty gap list - with nothing registered, "never sent: 0" is the line a fully
covered client prints - so the run stops instead.

Eleven real bugs came out of this after the call side had gone quiet - a mail
frame that hung after sending, bag cooldown swirls that never drew, an
achievements panel showing the empty state it was built with, a master looter
menu that could not open, the whole dungeon finder silently inert.

THE SHAPE TO LOOK FOR
---------------------
Nearly every one was the same: a message **received, parsed, stored, and never
announced**. The data was there and something already read it - often this
client's own ImGui window, which is why the feature worked while FrameXML's
version of it did not. Only the notification between the two was missing.

So the second table below is the one to read first: events with no sender whose
source message the client *already handles*. Those are two working halves with
nothing in between, and they are nearly always real.

THREE THINGS THIS CANNOT DECIDE
-----------------------------
* **A name kept in a variable is invisible here.** The calendar's reader fills
  a list of signal structs and one call raises whatever that list holds, so not
  one of its ten event names sits next to a fire call and all ten read as gaps.
  They get their own table rather than being folded into either side: a name in
  a list is an event only if something walks the list.

* **"Never fired" can mean "something else is fired in its place."**
  SMSG_QUESTGIVER_QUEST_LIST was sending GOSSIP_SHOW where QUEST_GREETING
  belonged, so the gossip frame opened over the quest list. Read the handler.

* **Check whether the server sends the message, by finding where it builds it.**
  The source is a separate checkout - WOWEE_SERVER_SRC says where - so grep for
  `WorldPacket data(SMSG_...)` in src/server/game.

  Do *not* use the STATUS_NEVER field in Opcodes.cpp for this. It reads like a
  statement that the server never sends the opcode and is not one:
  SMSG_ITEM_TEXT_QUERY_RESPONSE and SMSG_GMRESPONSE_RECEIVED are both marked
  STATUS_NEVER and both are built and sent, in ItemHandler.cpp and TicketMgr.cpp.
  Trusting that field produced four wrong conclusions in a row here.

  By the correct test, the three names below are all genuinely sent -
  BattlefieldHandler.cpp and LFGMgr.cpp build them - so they are real gaps
  rather than dead opcodes.

* **An event is only worth firing if the data behind it exists.** The
  battlefield eject pair carries a relocation and a reason this client does not
  parse; a popup saying a player is being moved without being able to say where
  is worse than no popup. Same for the mail refund lock and the inbound GM
  replies. Check what the handler actually holds before wiring it.
"""

import os
import re
import sys

ROOT = os.path.join(os.path.dirname(__file__), "..")
#: Name the extracted interface on the command line; see the docstring.
INTERFACE = (sys.argv[1] if len(sys.argv) > 1
             else os.path.join(ROOT, "Data", "interface"))
SRC = os.path.join(ROOT, "src")

EVENT = r'"([A-Z][A-Z0-9_]{3,})"'


def registered():
    """Events FrameXML asks for, and which files ask."""
    where = {}
    for dirpath, _, filenames in os.walk(INTERFACE):
        # The login screen is a separate interface in its own Lua state.
        if "gluexml" in dirpath.replace("\\", "/").split("/"):
            continue
        for name in filenames:
            if not name.endswith((".lua", ".xml")):
                continue
            src = open(os.path.join(dirpath, name), encoding="utf-8",
                       errors="ignore").read()
            # The closing paren matters: alternatepowerbar.lua registers
            # RegisterEvent("UNIT_"..self.powerName), and without it the
            # literal half of that concatenation was read as an event called
            # "UNIT_" - a name nothing can ever send, reported as a gap on
            # every run. A computed name cannot be checked from here at all.
            for pattern in (r'RegisterEvent\(\s*' + EVENT + r'\s*\)',
                            r'<Event\s+name=' + EVENT):
                for match in re.finditer(pattern, src):
                    where.setdefault(match.group(1), set()).add(name)
    return where


def sent():
    """Events this client can fire, from the calls that fire them."""
    names = set()
    for dirpath, _, filenames in os.walk(SRC):
        for name in filenames:
            if not name.endswith((".cpp", ".hpp")):
                continue
            src = open(os.path.join(dirpath, name), encoding="utf-8",
                       errors="ignore").read()
            # Two call shapes: addonEventCallbackRef()( ... ) through an
            # accessor, and addonEventCallback_( ... ) on the member directly.
            # Requiring both parens missed every one of the second kind.
            names |= set(re.findall(
                r'addonEventCallback[A-Za-z_]*(?:\(\))?\(\s*' + EVENT, src))
            names |= set(re.findall(r'fireAddonEvent\(\s*' + EVENT, src))
            # An event fired through a local alias:
            #     auto fire = owner_.addonEventCallbackRef();
            #     fire("LFG_PROPOSAL_UPDATE", {});
            # Neither pattern above sees that call, so every event sent this
            # way read as never sent - LFG_PROPOSAL_UPDATE was reported as a
            # gap while sitting two lines under its own alias.
            for alias in set(re.findall(
                    r'auto\s+(\w+)\s*=\s*[\w_.>()-]*addonEventCallback\w*\(\)', src)):
                names |= set(re.findall(
                    r'(?<![\w.])' + re.escape(alias) + r'\(\s*' + EVENT, src))
    return names


#: Where a function keeps the event name it was handed: a signal struct's
#: `event` field, or the first field of a struct it appends to a list of them.
STORE_FIELD = r'\.\s*event\s*=\s*(\w+)\s*[;,]'
STORE_INIT = r'(?:push_back|emplace_back)\(\{\s*(\w+)\s*[,}]'
#: A definition at the start of a line: its name, its parameters, and where its
#: body begins. Approximate on purpose - the body only has to reach the next
#: definition, which is how these helpers are written in this tree.
DEFINITION = re.compile(
    r'(?m)^[A-Za-z_][\w:<>,\s*&\[\]]*?\b(\w+)\s*\(([^)]*)\)\s*\{')
#: The same two shapes holding a literal rather than a variable.
HELD_LITERAL = (r'(?:push_back|emplace_back)\(\{\s*' + EVENT,
                r'\.\s*event\s*=\s*' + EVENT)


def held():
    """Events carried to the fire call in a variable, so the name is never next to it.

    `sent()` reads names that sit inside a fire call. The calendar does not
    write like that: its packet reader fills a list of `CalendarSignal`s
    (calendar_data.cpp), and one `fireAddonEvent(signal.event, signal.args)`
    in game_handler_callbacks.cpp raises whatever that list holds. Not one of
    its ten events is next to a fire call, and all ten came back as gaps -
    CALENDAR_UPDATE_EVENT was in the "read these first" table, above the two
    real ones, for months.

    So take the two shapes that hold a name: a struct appended with the name as
    its first field, or a field named `event`; and the helpers that put a
    parameter of their own into one of those, whose call sites are then read
    for literals. What this finds is not counted as sent - it is printed as its
    own table, to be checked by hand, because a name in a list is only raised
    if something walks that list.
    """
    helpers, direct = set(), set()
    texts = []
    for dirpath, _, filenames in os.walk(SRC):
        for name in filenames:
            if not name.endswith((".cpp", ".hpp")):
                continue
            try:
                src = open(os.path.join(dirpath, name),
                           encoding="utf-8", errors="ignore").read()
            except OSError:
                continue
            texts.append(src)
            for pattern in HELD_LITERAL:
                direct |= set(re.findall(pattern, src))
            defs = []
            for match in DEFINITION.finditer(src):
                params = set()
                for part in match.group(2).split(","):
                    part = part.strip().rstrip("=").split("=")[0].strip()
                    if part:
                        params.add(part.replace("*", " ").split()[-1].lstrip("&"))
                defs.append((match.group(1), params, match.start(), match.end()))
            for i, (fname, params, _start, bodyStart) in enumerate(defs):
                body = src[bodyStart:defs[i + 1][2] if i + 1 < len(defs) else len(src)]
                for pattern in (STORE_FIELD, STORE_INIT):
                    for match in re.finditer(pattern, body):
                        if match.group(1) in params:
                            helpers.add(fname)
    for src in texts:
        for helper in helpers:
            # The literal anywhere in the call's argument list - say(ctx, "NAME")
            # hands it over as the second one.
            direct |= set(re.findall(
                r'(?<![\w.])' + re.escape(helper) + r'\s*\([^;{}]{0,120}?' + EVENT, src))
    return direct


def chat_family():
    """The CHAT_MSG_ family, which neither half of the scan above can see.

    The client builds these names by concatenating the chat type onto a prefix,
    and FrameXML registers them by walking its ChatTypeGroup tables - so a
    literal-name match finds nothing on either side and the family is absent
    from the report rather than wrong in it. A zero above says nothing about
    chat.

    Read from the two places that do name them literally: the switch that turns
    a wire chat type into its name, and the tables FrameXML registers from.
    That comparison found ten types the client could not name - loot money,
    experience, honour, reputation, tradeskills, pet info and the rest of the
    run between LOOT and the battleground block - each of which fell to the
    default, was named UNKNOWN, and was dropped by FrameXML's chat.
    """
    names = os.path.join(SRC, "game", "world_packets_social.cpp")
    if not os.path.exists(names):
        return set(), set()
    src = open(names, encoding="utf-8", errors="ignore").read()
    ours = {"CHAT_MSG_" + n for n in re.findall(
        r'case ChatType::\w+:\s*return\s+"([A-Z_0-9]+)"', src)}
    chat = os.path.join(INTERFACE, "framexml", "chatframe.lua")
    if not os.path.exists(chat):
        return ours, set()
    theirs = set(re.findall(
        r'"(CHAT_MSG_\w+)"', open(chat, encoding="utf-8", errors="ignore").read()))
    return ours, theirs


def handled_opcodes():
    """SMSG names this client has a handler for, and which of those only skip.

    A skipped opcode is named in the dispatch table and read to the end without
    being parsed - the deliberate answer for a feature this client does not
    have. Counting one as handled put SMSG_UPDATE_LFG_LIST at the top of the
    "read these first" list, where it stayed: the raid browser is not
    implemented, so an event backed by it is exactly as absent as one with no
    handler at all, and pointing at it every run made the list look answered
    when it was not.
    """
    names, skipped = set(), set()
    for dirpath, _, filenames in os.walk(os.path.join(SRC, "game")):
        for name in filenames:
            if not name.endswith(".cpp"):
                continue
            src = open(os.path.join(dirpath, name), encoding="utf-8",
                       errors="ignore").read()
            names |= set(re.findall(r"Opcode::(SMSG_[A-Z0-9_]+)", src))
            # registerSkipHandler(Opcode::X), and the lambda spelling of the
            # same thing - a body that reads to the end and does nothing else,
            # whether written once or shared by a list of opcodes.
            skipped |= set(re.findall(
                r"registerSkipHandler\(\s*Opcode::(SMSG_[A-Z0-9_]+)", src))
            for block in re.finditer(
                    r"for\s*\(\s*auto\s+\w+\s*:\s*\{([^}]*)\}\s*\)\s*\{"
                    r"\s*table\[\w+\]\s*=\s*\[\]\([^)]*\)\s*\{"
                    r"\s*packet\.skipAll\(\);\s*\};", src):
                skipped |= set(re.findall(r"Opcode::(SMSG_[A-Z0-9_]+)", block.group(1)))
            skipped |= set(re.findall(
                r"table\[Opcode::(SMSG_[A-Z0-9_]+)\]\s*=\s*\[\]\([^)]*\)\s*\{"
                r"\s*packet\.skipAll\(\);\s*\};", src))
            # The third spelling, and the one this client actually uses for its
            # long lists: a loop whose body calls registerSkipHandler on each.
            # Missing it filed sixty-odd deliberately-dropped opcodes as
            # handled, which is what put the voice and calendar events on the
            # "read these first" list - features with no parser at all.
            for block in re.finditer(
                    r"for\s*\(\s*auto\s+(\w+)\s*:\s*\{([^}]*)\}\s*\)\s*\{?"
                    r"\s*registerSkipHandler\(\s*\1\s*\)", src):
                skipped |= set(re.findall(r"Opcode::(SMSG_[A-Z0-9_]+)", block.group(2)))
    return names - skipped, skipped


#: Words that say what happened rather than what it happened to. An event and
#: the message behind it name the same subject and almost never agree on these,
#: so they are dropped before the two names are compared.
FILLER = {"READY", "UPDATE", "UPDATED", "RESULT", "CHANGED", "CHANGE",
          "INFO", "DATA", "RESPOND", "RESPONSE", "MSG", "NOTIFY", "RECEIVED"}


def shares_tokens(event, body):
    """Whether an event and an opcode body name the same thing.

    Squashing underscores and asking for containment only pairs names where one
    reads as a run inside the other, and that missed the case this arm exists
    for: INSPECT_ACHIEVEMENT_READY is backed by SMSG_RESPOND_INSPECT_ACHIEVEMENTS,
    whose extra leading word breaks the run. The report said zero for months
    with that pair - and INSPECT_TALENT_READY, ARENA_TEAM_UPDATE and two more -
    sitting inside it, handled and never announced.

    So: compare word sets instead. Every word of the event that says what it is
    about has to appear in the message, plurals folded, and at least two of them
    must - one word in common is how CORPSE finds CALENDAR.
    """
    def words(name):
        return [w.rstrip("S") for w in name.split("_") if w]

    subject = {w for w in words(event) if w not in {f.rstrip("S") for f in FILLER}}
    if len(subject) < 2:
        return False
    carried = set(words(body))
    if not subject <= carried:
        return False
    # A message that negates a word of the event is about the opposite case:
    # SMSG_CORPSE_NOT_IN_INSTANCE does not back CORPSE_IN_INSTANCE.
    return "NOT" not in carried


def main():
    if not os.path.isdir(INTERFACE):
        # Nothing to register the events, so nothing can be missing from them:
        # the run would print the same lines a fully covered client prints.
        print(f"{INTERFACE} is not a directory - name the extracted interface "
              "directory on the command line.")
        return 2
    where, fired = registered(), sent()
    # `&` binds looser than `-`, so the parentheses are not decoration.
    inHand = (held() & set(where)) - fired
    opcodes, skipped = handled_opcodes()

    # Battle.net has no counterpart on a 3.3.5 server. Held names are out of the
    # gap list too, but they get their own table below: a name stored in a list
    # is only an event if something walks the list, so they are reported rather
    # than believed.
    gap = sorted(e for e in where
                 if e not in fired and e not in inHand and not e.startswith("BN_"))

    def squash(name):
        return name.replace("_", "")

    backed = []
    for event in gap:
        # Shortest first, then by name: several opcodes can match one event by
        # the token rule (SMSG_VOICE_SESSION_LEAVE, _ENABLE and
        # _ROSTER_UPDATE all back VOICE_SESSIONS_UPDATE), and walking a set made
        # the line the check prints change between runs of the same tree.
        for opcode in sorted(opcodes, key=lambda n: (len(n), n)):
            body = opcode[len("SMSG_"):]
            if squash(body) == squash(event) or squash(event) in squash(body) \
                    or shares_tokens(event, body):
                backed.append((event, opcode))
                break

    print(f"registered by FrameXML : {len(where)}")
    print(f"never sent             : {len(gap)}")
    print()
    print("never sent, but the source message IS handled - read these first:")
    for event, opcode in backed:
        files = " ".join(sorted(where[event])[:2])
        print(f"  {event:<38} {opcode:<44} {files}")
    print()
    print(f"{len(backed)} of {len(gap)} have a handler already. The rest are")
    print("features this client does not have, and are correctly silent.")

    # Told apart rather than hidden. An opcode that is only skipped is a
    # decision on record - the packet arrives, is read to the end and nothing
    # is done with it - and an event behind one is as absent as an event behind
    # no handler at all. Worth seeing, and worth not being told to read first.
    onlySkipped = []
    for event in gap:
        for opcode in sorted(skipped, key=lambda n: (len(n), n)):
            body = opcode[len("SMSG_"):]
            if squash(body) == squash(event) or squash(event) in squash(body) \
                    or shares_tokens(event, body):
                onlySkipped.append((event, opcode))
                break
    if onlySkipped:
        print()
        print("Backed only by a skip handler - the packet is read and dropped, "
              "which is\na decision rather than a gap:")
        for event, opcode in onlySkipped:
            print(f"  {event:<38} {opcode}")

    if inHand:
        print()
        print(f"{len(inHand)} more are written as an event name in this client's source, but "
              "never\nnext to a fire call - they reach the interface through a variable. "
              "Neither\na gap nor a proven send; read what walks the list that holds them:")
        for event in sorted(inHand):
            print(f"  {event}")

    ours, theirs = chat_family()
    unnamed = sorted(theirs - ours)
    print()
    print(f"Chat family, read separately because neither half above can see it "
          f"-\nthe client names {len(ours)} wire chat types, FrameXML names "
          f"{len(theirs)}:")
    if unnamed:
        print(f"  {len(unnamed)} FrameXML names that the client cannot, so each "
              f"falls to the\n  default, is called UNKNOWN, and is dropped:")
        for name in unnamed:
            print(f"    {name}")
    else:
        print("  every one of FrameXML's is named here")


if __name__ == "__main__":
    sys.exit(main())
