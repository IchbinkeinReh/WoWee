#!/usr/bin/env python3
"""Where a binding and the interface disagree about a call's shape.

A missing function is mostly harmless here: an undefined global answers nil, and
nil reads as false in a condition. What actually breaks the interface is a
function that *exists* and is the wrong shape, because Lua raises rather than
shrugging:

  * **too few arguments** - luaL_checknumber(L, 1) *raises* when the argument is
    absent. AbandonQuest() is called with none, so abandoning a quest from the
    quest log failed outright every time.
  * **too few return values** - the extra names are nil, which is fine until one
    is used. GetTalentInfo returned eight of ten, and the frame compares the
    ninth against maxRank: an error the moment a talent point is staged.
  * **too few event arguments** - same shape from the other direction.
    CHARACTER_POINTS_CHANGED is read as `arg2 > 0`, and was fired with nothing.

All three found real crashes on frames that are owned by default, and none of
them showed up in any count of missing names. This keeps them from coming back.

    tools/framexml_contract_check.py "<the extracted interface>"

Reports candidates, not faults. An extra nil only matters if it is used, and
plenty are guarded by `if (not x)` on the next line - UnitPowerType and
GetPetHappiness both are. Read the consumer before changing anything.
"""

import re
import sys
from pathlib import Path

import sys as _sys
_sys.path.insert(0, str(Path(__file__).resolve().parent))
from framexml_source import without_comments

ROOT = Path(__file__).resolve().parent.parent
# The default keeps the one-literal spelling sweep_guard looks for in this
# source.  That guard decides whether to skip a sweep by finding its input path
# here, and this sweep ran on CI where there is no interface to read, reported
# nothing, and was failed for reporting nothing.  The interface is installed data
# rather than repository content, so the directory to read is named on the
# command line; without one, or with a directory that is not there, nothing is
# parsed and the run stops rather than printing what a fully conforming client
# prints.
INTERFACE = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "Data/interface"
if INTERFACE.name.lower() == "framexml":
    # Naming the FrameXML folder means the same interface to a reader, and the
    # loader's answer starts from the folder above it.
    INTERFACE = INTERFACE.parent
FRAMEXML = INTERFACE / "framexml"
BINDINGS = ROOT / "src" / "addons"
GAME = ROOT / "src" / "game"

#: Return-count candidates that are correct as they stand, named rather than
#: counted so the printed count means what it says.
#:
#: Every Battle.net name is answered by a stub - there is no Battle.net to answer
#: it with - and the frames that unpack fourteen names from BNGetFriendInfo are
#: the friends and whisper panels sitting on that stub.  A missing return is a
#: fault only when the value is used, and these cannot be used: the friend list
#: never fills, so nothing walks what was unpacked from it.  Named individually
#: rather than by prefix, so a new BN name still reports.
EXPECTED_RETURNS = {
    "BNGetConversationMemberInfo", "BNGetFriendInfo", "BNGetFriendInfoByID",
    "BNGetFriendInviteInfo", "BNGetToonInfo",
}

CHECKS_ARG1 = re.compile(r"luaL_check(?:number|string|integer)\s*\(\s*L\s*,\s*1\s*\)")
NAMED_FN = re.compile(r"static int lua_(\w+)\s*\(lua_State\* L\)\s*\{(?:\.\w+\s*=\s*)?")
LAMBDA_FN = re.compile(r'\{(?:\.\w+\s*=\s*)?"(\w+)",\s*(?:\.\w+\s*=\s*)?\[\]\(lua_State\* L\)\s*->\s*int\s*\{')


def interfaceSources():
    """Every interface file the client loads, in both kinds.

    Three rules used to build their own list with two globs - `framexml/*.lua`
    and `addons/*/*.lua` - and read 187 files out of the 343 the loader opens.
    For the arity and return-count rules that cost six phantom files' calls -
    five FrameXML files no .toc Include reaches and one addon file beside them -
    and nothing else. For the event
    rule it cost the addons outright: that loop globbed `framexml/*.lua` only,
    so a handler in a load-on-demand panel could not be seen at all. And no
    rule read a single `.xml`, which is 162 of the 343 files: the loader
    installs every script body in one as a Lua chunk, and those bodies call
    bindings with too few arguments and unpack events exactly like the Lua does.

    Measured across the change: 187 files before, 343 after; 0 arity candidates
    either way, 2 return-count either way, 1 event candidate where the old scan
    had none. That one turned out to be manufactured - see checkEventArgs - so
    the count is back to zero, but the file set is why it appeared.
    """
    from framexml_source import loaded_files
    return sorted(p for p in loaded_files(INTERFACE)
                  if p.suffix.lower() in (".lua", ".xml"))


def stripComments(text):
    text = re.sub(r"--\[(=*)\[.*?\]\1\]", "", text, flags=re.S)
    return without_comments(text)


def bodyFrom(text, start):
    """The function body beginning at an opening brace, by matching braces.

    This used to read a fixed 1200 characters and cut at whatever marker
    turned up first. 102 of the bindings are longer than that and never
    reached their marker, so the checker was reading part of a function and
    drawing conclusions about the whole: a luaL_check past the cutoff was
    invisible, which under-reports the one fault it calls always real, and a
    lua_push past it made a binding look like it returned fewer values than
    it does, which is most of the return-count noise.

    Braces inside string and character literals are skipped, since a body
    that formats a message with one would otherwise end early.
    """
    depth, i, n = 0, start, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        # Comments first, and this is not fussiness: an apostrophe in prose -
        # "// Blizzard's own thresholds" - reads as an opening character
        # literal, and the scan then runs to the next apostrophe anywhere in
        # the file, swallowing every brace between. That put the end of one
        # binding eleven hundred lines further down and reported a function
        # that takes no arguments as requiring one.
        if two == "//":
            i = text.find("\n", i)
            if i == -1:
                break
        elif two == "/*":
            i = text.find("*/", i)
            if i == -1:
                break
            i += 1
        elif c == '"' or c == "'":
            quote = c
            i += 1
            while i < n and text[i] != quote:
                i += 2 if text[i] == "\\" else 1
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[start:i]
        i += 1
    return text[start:]


def bindingBodies():
    """Each binding's name and its body, however it was registered."""
    for cpp in sorted(BINDINGS.glob("*.cpp")):
        s = cpp.read_text(encoding="utf-8")
        for pattern in (NAMED_FN, LAMBDA_FN):
            for m in pattern.finditer(s):
                # end() sits just past the opening brace the pattern matched.
                yield m.group(1), bodyFrom(s, m.end() - 1), cpp.name


def checkArity():
    """Bindings that require argument one where the interface passes none."""
    bare = set()
    for p in interfaceSources():
        bare |= set(re.findall(r"(?<![:.\w])([A-Z][A-Za-z0-9_]{2,})\s*\(\s*\)",
                               stripComments(p.read_text(encoding="utf-8", errors="ignore"))))
    hits = []
    for name, body, where in bindingBodies():
        if name in bare and CHECKS_ARG1.search(body):
            # A guarded check is fine: lua_isnoneornil before the check means
            # the bare call takes the other path.
            if "lua_isnoneornil" in body:
                continue
            hits.append((name, where))
    return sorted(set(hits))


def checkReturns():
    """Bindings returning fewer values than the interface unpacks."""
    want = {}
    for p in interfaceSources():
        t = stripComments(p.read_text(encoding="utf-8", errors="ignore"))
        # `local a, b = Foo()` and the plain `a, b = Foo()` beside it. The
        # second form is 234 lines in FrameXML alone - the achievement summary
        # unpacks its counts that way - and looking only for `local` walked
        # straight past all of them.
        for m in re.finditer(
                r"(?:local\s+)?\b([A-Za-z_][\w]*(?:\s*,\s*[A-Za-z_]\w*)+)\s*=\s*"
                r"([A-Z]\w{2,})\s*\(", t):
            names = [x for x in m.group(1).split(",") if x.strip()]
            if len(names) <= 1:
                continue
            # More than one call on the right-hand side means the names are
            # shared between them, and counting them all against the first is
            # nonsense. Two of these were reported as faults:
            #     local screenWidth, screenHeight = GetScreenWidth(), GetScreenHeight()
            #     local a, b = GetCVarBool("x"), GetCVarBool("y")
            rhs = t[m.end(2):]
            rhs = rhs[:rhs.find("\n") if "\n" in rhs else len(rhs)]
            if re.search(r"\)\s*,\s*[A-Za-z_]\w*\s*\(", rhs):
                continue
            if len(names) > want.get(m.group(2), (0, ""))[0]:
                want[m.group(2)] = (len(names), p.name)

    give = {}
    for name, body, _ in bindingBodies():
        if re.search(r"return\s+[a-z_]\w*\s*;", body):
            give[name] = None          # counted at runtime; nothing to compare
            continue
        rets = [int(x) for x in re.findall(r"return\s+(\d+)\s*;", body)]
        if rets and give.get(name, 0) is not None:
            give[name] = max(rets + [give.get(name) or 0])

    return sorted((n, give[n], c, w) for n, (c, w) in want.items()
                  if give.get(n) is not None and n in give and c > give[n]
                  and n not in EXPECTED_RETURNS)


def checkEventArgs():
    """Events whose handler unpacks more than is ever fired."""
    fired = {}
    for cpp in sorted(GAME.glob("*.cpp")) + sorted(BINDINGS.glob("*.cpp")):
        s = cpp.read_text(encoding="utf-8")
        for m in re.finditer(
                r'(?:fireAddonEvent|addonEventCallbackRef\(\))\(\s*"(\w+)"\s*,\s*\{([^{}]*)\}', s):
            args = m.group(2).strip()
            n = 0 if not args else args.count(",") + 1
            fired[m.group(1)] = max(fired.get(m.group(1), 0), n)

    hits = {}
    for p in interfaceSources():
        t = stripComments(p.read_text(encoding="utf-8", errors="ignore"))
        # Stop at the next branch: a fixed window crossed into the following
        # elseif once and blamed the wrong event for its argument.
        for m in re.finditer(r'event\s*==\s*"(\w+)"\s*\)?\s*then(.*?)(?:\n\s*(?:elseif|end)\b)',
                             t, re.S):
            name, body = m.group(1), m.group(2)
            u = re.search(r"local\s+([A-Za-z_][\w\s,]*?)\s*=\s*\.\.\.", body)
            if not u:
                continue
            # A branch head like
            #
            #     elseif ( event == "GUILD_ROSTER_UPDATE" or event == "PLAYER_GUILD_UPDATE" ) then
            #         if ( event == "GUILD_ROSTER_UPDATE" ) then
            #             local arg1 = ...;
            #
            # matches on the second name — the first fails the pattern's `then`
            # and the scanner moves on — and the unpack is charged to it. The
            # re-test directly above the unpack says whose argument it actually
            # is, and PLAYER_GUILD_UPDATE was reported for one it never reads.
            guard = re.findall(r'if\s*\(?\s*event\s*==\s*"(\w+)"', body[:u.start()])
            if guard:
                name = guard[-1]
            if name not in fired or name in hits:
                continue
            want = len([x for x in u.group(1).split(",") if x.strip()])
            if want > fired[name]:
                hits[name] = (fired[name], want, p.name)
    return sorted((k,) + v for k, v in hits.items())


def main():
    if not FRAMEXML.is_dir():
        print(f"{FRAMEXML} is not a directory - name the extracted interface "
              "directory on the command line.")
        return 2
    files = interfaceSources()
    # What was read, printed before what was found: "none" from 343 files and
    # "none" from no files at all are the same three characters on a terminal.
    print(f"scanned {len(files)} file(s) the loader opens - "
          f"{sum(1 for f in files if f.suffix.lower() == '.lua')} .lua, "
          f"{sum(1 for f in files if f.suffix.lower() == '.xml')} .xml\n")
    arity = checkArity()
    returns = checkReturns()
    events = checkEventArgs()

    print("Bindings that require argument one, which the interface calls bare:")
    for name, where in arity:
        print(f"    {name:<30} {where}")
    print("    (none)" if not arity else "")

    print("\nBindings returning fewer values than the interface unpacks:")
    for name, got, want, where in returns:
        print(f"    {name:<30} returns {got}, unpacks {want}   ({where})")
    print("    (none)" if not returns else "")

    print("\nEvents whose handler unpacks more than is fired:")
    for name, got, want, where in events:
        print(f"    {name:<30} fired {got}, unpacks {want}   ({where})")
    print("    (none)" if not events else "")

    print(f"\n{len(arity)} arity, {len(returns)} return-count, {len(events)} event "
          f"candidates. Only the first is always a fault; the other two matter "
          f"when\nthe missing value is used rather than merely unpacked.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
