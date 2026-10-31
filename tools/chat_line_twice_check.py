#!/usr/bin/env python3
"""Chat lines this client writes that the interface also writes.

    tools/chat_line_twice_check.py ["<the extracted interface>"]

WHY

Handing the chat window to FrameXML did not stop this client writing to it.
Both halves still run: a packet arrives, the handler adds a line of its own and
fires the addon event, and chatframe.lua's own OnEvent branch formats the same
fact from the event's arguments and adds it too. The player reads it twice.

Nothing raises, no test fails and the line is correct both times - it is only
there twice. It was found by a player watching Booty Bay come under sustained
attack and counting fifteen messages in the window, three per packet: one from
this client, one from the interface, and one on the error line the real client
never uses for that event at all.

Seven events in chatframe.lua write their own line. Three of them were
duplicated this way: ZONE_UNDER_ATTACK, PLAYER_LEVEL_UP and GUILD_MOTD.

The guard is what separates a doubled line from a guarded one, and the test
usually sits in the *header* of the block, not in the block: `if
(!ui::frameXmlOwns(ui::UiElement::Chat)) { addSystemChatMessage(...); }`. Walk
out only the braces and the guard is outside what was read, which reported
ZONE_UNDER_ATTACK - the one handler written with a comment explaining that it
stopped doubling - the first time this sweep had an interface to read.

WHAT IT DOES

Reads chatframe.lua's ChatFrame_OnEvent chain for the branches that call
AddMessage, then finds where this client fires each of those events and asks
whether the same function also writes a chat line - without first asking
whether FrameXML owns the chat window.

WHAT IT CANNOT SEE

Whether the two lines say the same thing. A handler that fires an event and
writes an unrelated line about something else is reported here and is fine; the
guard is what the report is really about, and adding one is cheap. It also only
knows the events chatframe.lua handles by name, so a line duplicated through a
CHAT_MSG_* path is invisible to it - that one is [[framexml_duplicate_windows]]
territory, not this.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
# The interface root on the command line like every other interface sweep; the
# chat window comes off it. Data/interface is the fallback and is not tracked.
INTERFACE = (pathlib.Path(sys.argv[1]) if len(sys.argv) > 1
             else ROOT / "Data/interface")
CHATFRAME = INTERFACE / "framexml/chatframe.lua"
SOURCES = sorted((ROOT / "src/game").glob("*.cpp"))

# The calls that put a line in front of the player.
WRITERS = ("addSystemChatMessage", "addLocalChatMessage", "addLocalChatLine",
           "addUIError", "raiseUiError")

# The question a duplicated writer failed to ask.
GUARD = "frameXmlOwns"

# Settled: the line is written for a case the interface's branch does not cover.
EXPECTED = {}


def events_the_interface_writes(text):
    """Events whose ChatFrame_OnEvent branch calls AddMessage."""
    found = []
    for chunk in re.split(r"\n\telseif \( event == ", text)[1:]:
        name = re.match(r'"([A-Z_0-9]+)"', chunk)
        if not name:
            continue
        body = chunk.split("\n\telseif")[0]
        if "AddMessage" in body:
            found.append(name.group(1))
    return found


def enclosing_block(text, at):
    """The braced block the offset sits in, walked out from it, with the header
    that opens it prepended.

    The header is the part that carries the guard. The fix for the doubled zone
    warning is spelled

        if (!ui::frameXmlOwns(ui::UiElement::Chat)) {
            addSystemChatMessage(...);
        }

    and the innermost block - the one this walks out - holds the call and not
    the test. Reading the body alone reported the one handler whose comment
    says out loud that it stopped doubling, which made this sweep's single row
    about the sweep itself.
    """
    depth, start = 0, None
    for i in range(at, -1, -1):
        if text[i] == "}":
            depth += 1
        elif text[i] == "{":
            if depth == 0:
                start = i
                break
            depth -= 1
    if start is None:
        return ""
    # Back over the control header, which ends at whatever statement or block
    # came before it - a `;`, or the brace of the block before or around it.
    # `:` is deliberately not one of them: `if (!ui::frameXmlOwns(...)) {` stops
    # at its own first `::` if it is, and the guard is then outside what came
    # back, which is the miss this whole walk exists to close. Capped because a
    # header with no boundary in front of it means the walk has run past
    # something this cannot tell where to stop.
    head = max(0, start - 500)
    for i in range(start - 1, head - 1, -1):
        if text[i] in ";}{" :
            head = i + 1
            break
    depth, end = 0, len(text)
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                end = i
                break
    return text[head:end]


def main():
    if not CHATFRAME.exists():
        print(f"No chatframe.lua at {CHATFRAME}. Nothing can be compared - do "
              "not believe a zero. Name the unpacked interface on the command "
              "line.")
        return 1

    events = events_the_interface_writes(CHATFRAME.read_text(encoding="utf-8"))
    if not events:
        print("Read no AddMessage branches out of chatframe.lua. The zero below "
              "means the parse broke, not that the interface writes nothing.")
        return 1

    doubled = []
    for path in SOURCES:
        text = path.read_text(encoding="utf-8")
        for event in events:
            for m in re.finditer(r'"%s"' % event, text):
                block = enclosing_block(text, m.start())
                if not block or GUARD in block:
                    continue
                written = [w for w in WRITERS if w in block]
                if not written:
                    continue
                line = text.count("\n", 0, m.start()) + 1
                key = f"{path.name}:{event}"
                if key in EXPECTED:
                    continue
                doubled.append((path.name, line, event, written[0]))

    print(f"{len(events)} events the interface writes its own chat line for:")
    for e in events:
        print(f"  {e}")
    print()
    print(f"{len(doubled)} of them written a second time by this client:")
    for name, line, event, writer in doubled:
        print(f"  {name}:{line}  {event}  also calls {writer}()")
    if not doubled:
        print("  (none)")
    return 1 if doubled else 0


if __name__ == "__main__":
    sys.exit(main())
