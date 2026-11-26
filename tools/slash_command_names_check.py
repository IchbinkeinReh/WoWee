#!/usr/bin/env python3
"""Every slash command this client answers is one 12340 knows, or a developer tool.

    tools/slash_command_names_check.py

T369, the user's answer (2026-11-26): the client's own developer and
diagnostic commands stay (answering in German); every other name the German
12340 client does not know goes, so it gets the original's unknown-command line
(FrameXML's ChatEdit_ParseText -> HELP_TEXT_SIMPLE) instead of an answer of
this client's own. Shortcuts like /rl /gkick /wts /wtb /nr /lootmethod /mark
were such names.

KNOWN_12340 is the part of the registry's names that the deDE interface itself
registers - every SLASH_*n and EMOTE*_CMD*n string of
Interface\\FrameXML\\GlobalStrings.lua of the 12340 deDE client, intersected with
the registry when this check was written (the client data is not in the
checkout, so the set is kept here). DEVELOPER is what the user kept; the console
command table's names (0x00407870 / 0x00402910: cvarlist, set, ver, worldport
...) are among them.

A name in the registry that is in neither set fails: either it is a real 12340
name (add it to KNOWN_12340 after checking GlobalStrings.lua) or it is one of
this client's own, which needs the user's word before it is added.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
COMMANDS = ROOT / "src/ui/chat/commands"

KNOWN_12340 = {
    "?", "afk", "assist", "away", "battleground", "bg", "busy", "camp", "cancelaura",
    "cancelform", "cast", "castsequence", "chathelp", "clear", "clearfocus",
    "clearmainassist", "clearmaintank", "cleartarget", "combatlog", "dismount", "dnd",
    "duel", "dump", "equip", "equipset", "exit", "f", "focus", "follow", "forfeit",
    "friend", "g", "gdemote", "gdisband", "ginfo", "ginvite", "gleader", "gmotd",
    "gpromote", "gquit", "groster", "guild", "guilddemote", "guilddisband", "guildinfo",
    "guildinvite", "guildleader", "guildmotd", "guildpromote", "guildquit",
    "guildroster", "help", "i", "ignore", "inspect", "invite", "join", "kick", "kneel",
    "leave", "logout", "ma", "macrohelp", "mainassist", "maintank", "mt", "o", "officer",
    "osay", "p", "party", "petaggressive", "petattack", "petdefensive", "petfollow",
    "petpassive", "petstay", "played", "pvp", "quit", "r", "ra", "raid", "raidinfo",
    "random", "ready", "readycheck", "reload", "remfriend", "removefriend", "reply",
    "rnd", "rsay", "run", "rw", "s", "say", "script", "shout", "sit", "stand",
    "startattack", "stopattack", "stopcasting", "stopmacro", "surrender", "t", "target",
    "targetenemy", "targetfriend", "targetlastenemy", "targetlastfriend",
    "targetlasttarget", "tell", "threat", "time", "trade", "unignore", "uninvite",
    "use", "w", "whisper", "who", "y", "yell", "yield",
}

DEVELOPER = {
    # this client's diagnostics
    "gps", "loc", "coords", "whereami", "zone", "cam", "daytime", "mem", "profile", "perf",
    "record", "fxcheck", "transportinfo", "transport", "unstuck", "unstuckgy",
    "unstuckhearth", "maxout",
    # the original's console command table, typed bare
    "cvarlist", "cvar_reset", "cvar_default", "set", "ver", "pitchlimit",
    "changeplayerdifficulty", "togglehelm", "togglecloak", "memusage", "heapusage2",
    "setmap", "worldport", "charmport", "setrawpos", "showdetaildoodads", "updatepoi",
}

# The shortcuts that went with T369; named so a revert is caught by name.
REMOVED = {
    "raidwarning", "instance", "leavechannel", "wts", "wtb", "cancellogout",
    "cancelshapeshift", "pethalt", "petdismiss", "gmhelp", "gmcommands", "rc", "notready",
    "nr", "leaveparty", "raidconvert", "lootmethod", "grouploot", "setloot",
    "lootthreshold", "mark", "marktarget", "raidtarget", "roll", "leaveguild", "gkick",
    "guildkick", "gcreate", "guildcreate", "screenshot", "ss", "ticket", "gmticket", "gm",
    "score", "cl", "helm", "helmet", "showhelm", "cloak", "showcloak", "stopfollow",
    "addfriend", "delfriend", "whois", "online", "players", "print", "reloadui", "rl",
    "difficulty", "targetlast",
}

ALIASES = re.compile(r"aliases\(\)\s*const\s*override\s*\{\s*return\s*\{([^}]*)\}", re.S)


def registry_names():
    names = set()
    for path in sorted(COMMANDS.glob("*.cpp")):
        text = path.read_text(encoding="utf-8")
        for m in ALIASES.finditer(text):
            names.update(n.lower() for n in re.findall(r'"([^"]*)"', m.group(1)))
    return names


def main():
    names = registry_names()
    print(f"{len(names)} registry names")
    if len(names) < 100:
        print("FAIL: the alias parse found too few names; the pattern no longer matches")
        return 1
    bad = sorted(n for n in names if n not in KNOWN_12340 and n not in DEVELOPER)
    back = sorted(n for n in names if n in REMOVED)
    for n in back:
        print(f"FAIL: /{n} is back; 12340 does not know it (T369)")
    for n in bad:
        if n not in REMOVED:
            print(f"FAIL: /{n} is neither a 12340 name nor a kept developer command")
    return 1 if bad or back else 0


if __name__ == "__main__":
    sys.exit(main())
