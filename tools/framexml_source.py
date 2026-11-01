#!/usr/bin/env python3
"""Reading FrameXML's text without being fooled by it.

Twelve sweeps had their own copy of "strip the comments", written slightly
differently each time, and only one of them had learned to strip string
literals as well. That is this repository's dominant bug shape applied to its
own tools: one rule in twelve places, eleven of them a version behind.

TWO RULES, AND WHICH TO ASK FOR

`without_comments` is for a sweep that reads *names out of strings* - the
event-arity family matches `event == "SOMETHING"`, the CVar label check reads
the literal a branch compares against. Those need the strings intact.

`without_comments_or_strings` is for a sweep that reads *syntax* - anything
looking for `Foo(` and calling it a call. A Lua pattern is a string full of
parentheses, and `strmatch(name, "DropDownList(%d+)")` reads as a call to a
global named DropDownList. That put "every dropdown in the interface raises as
it opens" at the top of a report, which is alarming and wrong.

OFFSETS ARE PRESERVED

Both blank in place rather than deleting, so every line number and index is
where it was. A report that sends you to the wrong line costs more than it
saves, and two of these sweeps did exactly that before they were caught.
"""
import pathlib
import re

_XML_COMMENT = re.compile(r"<!--.*?-->", re.S)
_LUA_COMMENT = re.compile(r"--\[\[.*?\]\]|--[^\n]*", re.S)
_STRING = re.compile(r"'[^'\n]*'|\"[^\"\n]*\"")


def _blank(match):
    """Same length, same newlines, nothing else."""
    return re.sub(r"[^\n]", " ", match.group(0))


def without_comments(text):
    """XML and Lua comments blanked; string literals left alone."""
    return _LUA_COMMENT.sub(_blank, _XML_COMMENT.sub(_blank, text))


def without_comments_or_strings(text):
    """...and the inside of every string literal blanked too.

    The quotes stay, so a token still ends where it did and an unterminated
    string cannot swallow the rest of the file.
    """
    text = without_comments(text)
    return _STRING.sub(lambda m: m.group(0)[0] + " " * (len(m.group(0)) - 2)
                       + m.group(0)[-1], text)


# ── Names the interpreter itself installs ────────────────────────────────────

#: Globals a sweep should never report as unbound, because no binding provides
#: them and no FrameXML file defines them: they come from the Lua 5.1 this
#: project builds (extern/lua-5.1.5) and that lua_engine opens.
#:
#: Read off the interpreter and the engine, not off a recollection of Lua:
#:
#:   * the base table, extern/lua-5.1.5/src/lbaselib.c:448-471, plus ipairs and
#:     pairs and newproxy, which luaopen_base installs by hand a few lines
#:     later (lbaselib.c:633-644) - ipairs and pairs are not in the table above,
#:     and a list written from memory tends to forget that
#:   * only the libraries lua_engine.cpp:6791-6794 opens - base, table, string,
#:     math, and `coroutine' as a side of luaopen_base (lbaselib.c:650). io, os,
#:     debug and package are deliberately not opened, so `os`, `io`, `debug`,
#:     `require` are NOT in this set. A FrameXML call of `os.date` reaches a
#:     binding or the missing-API fallback, and a sweep is right to ask which
#:   * four base names are nil'd back out again, lua_engine.cpp:6803-6805:
#:     dofile, loadfile, load, collectgarbage. They are in the interpreter's
#:     table and not in the running game, so they are not here either - listing
#:     them would hide a call that really does fall through.
#:
#: `loadstring` survives that removal, which is why it is here: the list nils
#: out `load`, not `loadstring`.
#:
#: Two sweeps (framexml_unbound_globals.py, framexml_reachable_globals.py) carry
#: their own wider list of this size, adding FrameXML's own aliases - format,
#: strsub, tinsert, wipe, abs, ceil - which the engine's bootstrap defines rather
#: than the interpreter. Those two count names this one must not excuse, so the
#: lists are deliberately not merged; only the interpreter half is shared.
INTERPRETER_GLOBALS = frozenset({
    "assert", "error", "gcinfo", "getfenv", "getmetatable", "ipairs",
    "loadstring", "newproxy", "next", "pairs", "pcall", "print", "rawequal",
    "rawget", "rawset", "select", "setfenv", "setmetatable", "tonumber",
    "tostring", "type", "unpack", "xpcall",
    "coroutine", "math", "string", "table",
})


# ── Which files the loader actually reaches ──────────────────────────────────

_REFERENCE = re.compile(r'<(?:Script|Include)\s+file="([^"]+)"', re.I)

#: An addon the binding refuses, should one ever be refused again. There is a
#: second way a file can fail to load besides a manifest: `LoadAddOn` can answer
#: one by name and hand back "DISABLED".
#:
#: **Nothing is refused today, and this matches nothing on purpose.** The name
#: this was written against was Blizzard_Calendar, refused while its verbs were
#: unbound so that the minimap's date button did nothing instead of raising. The
#: refusal was lifted once a calendar was behind it - `lua_system_api.cpp:4972`
#: says so, and the note that stood there is kept in that file rather than
#: repeated here - so the empty set this answers now is the truth and not a
#: broken lookup. It stays because a refusal is a thing the loader can still do,
#: and a sweep that scans a refused addon is a sweep reporting faults in files
#: that never run.
#:
#: Three tools reasoned from that refusal and are corrected on the same day as
#: this note: `framexml_unbound_globals.py` excused a family of calendar names
#: by it, `framexml_reachable_globals.py` built its one reported name on it, and
#: `framexml_run.cpp` declined the addon in the harness "as the client does" and
#: so never loaded in the harness what every player loads in the game. They are
#: named here because the next refusal will be reasoned about the same way, and
#: a rule read out of a stale comment is how all three got it wrong.
_REFUSED = re.compile(r'strcmp\(name,\s*"(\w+)"\)\s*==\s*0(?:(?!strcmp).){0,400}?"DISABLED"',
                      re.S)


def _refused_addons(root=None):
    """Lowercased names of addons the client declines to load.

    The loader being modelled lives in *this repository*, so this looks next to
    this file first and only then at the root it was handed. Deriving it from
    the interface alone answered nothing on any machine where the interface is
    the extracted copy - which is all of them, since `Data/interface` has never
    existed here (`Data` holds `expansions` and `opcodes`) and two levels up from
    `…/Data/expansions/wotlk/interface` is `…/Local/WoWee`, which carries no
    source at all. That is a latent rather than a live wrong answer today,
    because nothing is refused: had a name been refused, the lookup would have
    missed it and every sweep would have gone on scanning the refused addon's
    files.
    """
    candidates = [pathlib.Path(__file__).resolve().parent.parent]
    if root is not None:
        candidates.append(pathlib.Path(root))
    for candidate in candidates:
        source = candidate / "src/addons/lua_system_api.cpp"
        if source.is_file():
            return {m.lower()
                    for m in _REFUSED.findall(source.read_text(errors="ignore"))}
    return set()


def loaded_files(interface):
    """Every .lua and .xml the loader reaches, as a set of Paths.

    A file sitting in the folder is not a file that loads. Blizzard's glue
    screens ship in the same directory as FrameXML and are listed in no
    manifest here, because this client has its own login and character select
    and never loads them: accountlogin, characterselect, charactercreate,
    realmlist, realmwizard, glueparent, movieframe. Sixty of the seventy-five
    names framexml_unbound_globals called "these raise as their panel opens"
    came out of those files, which cannot raise because they never run.

    Derived from the manifests and the <Script>/<Include> graph rather than
    from a list written here - a hand-written file list is what crippled every
    per-element sweep before this, and the loader's own answer does not go
    stale when a file is added.

    Case-insensitively, because the loader resolves that way and the interface
    is not consistent: manifests carry Blizzard's capitalisation and the files
    on disk are lowercase.
    """
    interface = pathlib.Path(interface)
    out = set()
    if not interface.is_dir():
        return out

    refused = _refused_addons(interface.parent.parent)

    # Every folder with a manifest, at any depth - the bundled addons sit one
    # level further down, under addons/.
    #
    # Except gluexml, which AddonManager refuses by name: the login and
    # character-select screens are this client's own, and FrameXML's have
    # exactly one way in that this is not. Counting them put sixty names in
    # the unbound-globals report under "these raise as their panel opens",
    # and not one of them can raise, because none of those files ever run.
    directories = sorted({t.parent for t in interface.rglob("*.toc")
                          if t.parent.name.lower() != "gluexml"
                          and t.parent.name.lower() not in refused})
    # An addon may reference a shared template out of FrameXML, so that folder
    # is searched as a fallback exactly as the loader searches it.
    shared = next((d for d in directories if d.name.lower() == "framexml"), None)
    spare = ({p.name.lower(): p for p in shared.iterdir() if p.is_file()}
             if shared else {})

    for directory in directories:
        have = {p.name.lower(): p for p in directory.iterdir() if p.is_file()}

        pending = []
        for toc in directory.glob("*.toc"):
            for line in toc.read_text(errors="ignore").splitlines():
                entry = line.strip()
                if entry and not entry.startswith("#") and \
                        entry.lower().endswith((".xml", ".lua")):
                    pending.append(entry)

        seen = set()
        while pending:
            ref = pending.pop().replace("\\", "/").rsplit("/", 1)[-1].lower()
            if ref in seen:
                continue
            seen.add(ref)
            path = have.get(ref) or spare.get(ref)
            if not path:
                continue
            out.add(path)
            if path.suffix.lower() == ".xml":
                text = without_comments(path.read_text(errors="ignore"))
                pending.extend(_REFERENCE.findall(text))

    return out
