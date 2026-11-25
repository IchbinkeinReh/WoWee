#!/usr/bin/env python3
"""A Lua binding that asks for an argument after it has pushed a new table.

    tools/lua_arg_after_push_check.py

WHY

CreateFrame("Frame", "Name") is called with two arguments. The binding pushed
the frame's table, which made it the third value on the stack, and asked then
whether the third argument, the parent, is a table: it was - the frame itself.
GetParent() of every such frame answered the frame, and whoever walked up its
parents did so for good.

WHAT IT DOES

In each lua_CFunction of src/addons, looks for an argument from the second on
that is asked for after a table was made, where the function did not fix the
number of its arguments with lua_settop first.

WHAT IT CANNOT SEE

A value pushed by something else than lua_newtable, lua_createtable or
lua_newuserdata, and an argument read through a helper that is not listed here.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BINDING = re.compile(r"^\s*(?:static )?int (\w+)\(lua_State\* ?L\w*\) ?\{", re.M)
MADE = re.compile(r"lua_(?:newtable|createtable|newuserdata)\(")
ASKED = re.compile(r"(?:lua_is(?:none|noneornil|table|nil|string|number|function|boolean)|lua_toboolean|lua_tonumber|"
                   r"lua_tointeger|lua_tostring|lua_type|luaL_opt\w+|widgetOf|widgetIdOf|luaClientSwitchArg|"
                   r"luaClientBool)\(L, ?([2-9])\b")


def body(text, start):
    depth, at = 1, start
    while depth and at < len(text):
        depth += {"{": 1, "}": -1}.get(text[at], 0)
        at += 1
    return text[start:at]


def main():
    looked, found = 0, []
    for path in sorted((ROOT / "src" / "addons").glob("*.cpp")):
        text = re.sub(r"//[^\n]*", "", path.read_text(encoding="utf-8", errors="ignore"))
        for m in BINDING.finditer(text):
            inner = body(text, m.end())
            made = MADE.search(inner)
            if not made:
                continue
            looked += 1
            if "lua_settop(" in inner[:made.start()]:
                continue
            asked = ASKED.search(inner, made.start())
            if asked:
                found.append((path.name, text.count("\n", 0, m.start()) + 1, m.group(1), asked.group(1)))
    print(f"{looked} binding(s) that make a table\n")
    print(f"{len(found)} ask for an argument after it:")
    for name, line, function, index in found:
        print(f"  src/addons/{name}:{line}  {function}  (argument {index})")
    if not found:
        print("  (none)")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
