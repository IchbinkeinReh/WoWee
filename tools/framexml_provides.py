#!/usr/bin/env python3
"""What the client answers for - the one place that decides.

Seven sweeps each worked this out for themselves, and they disagreed. Most read
only the C++ binding tables, which is between a third and a half of the answer:
138 names are provided from bootstrap Lua living inside C++ string literals, and
a tool that cannot see those reports them as gaps.

That is not hypothetical. `framexml_reachable_globals` reported
GetNumStationeries as unbound and reachable from the mail frame. It was answered
by the counting table long before that report, and acting on the report made
things worse - binding it explicitly removed it from the missing-API report,
which is the one thing that report exists to preserve. (It has since left the
counting table for a real binding, once the paper-style popup was built out of
Stationery.dbc; the counting entry was only ever a stand-in for a feature the
client did not have.) Meanwhile
`framexml_element_readiness` had it right the whole time, because its
`registered()` did read the bootstrap.

So: one implementation, imported. A sweep that wants to know whether a name is
answered asks here - and it has to read every place the bootstrap lives, which
is not one file. Two headers under include/addons carry Lua raw strings handed
to bootstrap() like the literals in lua_engine.cpp do, and leaving them out made
`noop_widget_methods` name GetFontString and SetFontString among the no-ops when
button_label_lua.hpp has implemented both since it was written: eleven call sites
dereferencing `:GetFontString()` were reported as raising on a nil.

    from framexml_provides import globals_provided, widget_methods_provided
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ADDONS = ROOT / "src/addons"
ENGINE = ADDONS / "lua_engine.cpp"
SNIPPETS = ROOT / "include/addons"


def _sources():
    return [p.read_text(encoding="utf-8", errors="ignore")
            for p in sorted(ADDONS.glob("*.cpp"))] + _chunks()


def _chunks():
    """Bootstrap Lua that lives in a header rather than in a .cpp.

    Two of them carry raw-string Lua handed to bootstrap() exactly like the
    literals in lua_engine.cpp: button_label_lua.hpp (kButtonLabelLua, which is
    where SetFontString, GetFontString and GetNormalFontObject are defined) and
    addon_lua_snippets.hpp (the options-panel builder, the bit library, the
    missing-cvar fallback). Reading only the .cpp said those methods are answered
    by nothing, so framexml_noop_returns reported eleven call sites dereferencing
    `:GetFontString()` on a nil - on buttons, where the block had been creating a
    font string on demand since it was written.

    Filtered on the raw string rather than taken wholesale: these headers also
    carry C++, and a method-shaped thing in C++ is not a method FrameXML calls.
    """
    out = []
    for path in sorted(SNIPPETS.glob("*.hpp")):
        text = path.read_text(encoding="utf-8", errors="ignore")
        if 'R"LUA(' in text:
            out.append(text)
    return out


def _engine_text():
    """The engine plus the header chunks, as one body of Lua-bearing source."""
    return "\n".join([ENGINE.read_text(encoding="utf-8", errors="ignore")]
                     + _chunks())


def _installed_method_names():
    """Methods a sibling file hangs on whatever table its caller left on the stack.

    `addTabardModelMethods` in lua_tabard_api.cpp walks a `luaL_Reg` array and
    `lua_setfield(L, -2, r.name)`s each name onto the frame metatable the engine
    pushed at lua_engine.cpp:7110. Every name-shaped regex here looks at the
    engine, which mentions that function by call and never by name, so
    `framexml_unbound_widget_methods` kept reporting the tabard dressing room's
    six model methods - CanSaveTabardNow, CycleVariation, InitializeTabardColors,
    Save and the two emblem textures - as answering nil a month after they were
    written.

    Only an array that such a loop walks counts. The obvious version - read every
    `{"Name", lua_X}` pair in every file - takes the *global* binding tables of
    lua_inventory_api.cpp and lua_action_api.cpp along with it, and IsEquippedItem,
    SetBinding and their siblings then read as widget methods, which is a frame
    answering `frame:SetBinding("W")` that the real client refuses.
    """
    out = set()
    for path in sorted(ADDONS.glob("*.cpp")):
        src = path.read_text(encoding="utf-8", errors="ignore")
        for m in re.finditer(r"for\s*\([^;{}]*:\s*(\w+)\s*\)\s*\{[^{}]*"
                             r"lua_setfield\(\s*\w+\s*,\s*-2\s*,\s*\w+\.name",
                             src, re.S):
            decl = re.search(r"\b" + re.escape(m.group(1))
                             + r"\b\s*(?:\[\s*\])?\s*=\s*\{(.*?)\};", src, re.S)
            if decl:
                out |= set(re.findall(r'\{\s*"([A-Za-z_]\w*)"\s*,', decl.group(1)))
        # The same install with the names in the loop header instead of in an
        # array: `for (const char* name : {"CreateAnimationGroup",
        # "StopAnimating"}) { ... lua_setfield(L, -2, name); }` at
        # lua_engine.cpp:4807, which copies the animation bootstrap's two names
        # onto regions off the globals. StopAnimating - glyph UI's
        # `spin:StopAnimating()` - was this sweep's last remaining row and answered
        # nothing on paper, while the copy has run since the line was written.
        for m in re.finditer(r"for\s*\(\s*const char\s*\*\s*(\w+)\s*:\s*\{([^{}]*)\}\s*\)"
                             r"\s*\{[^{}]*lua_setfield\(\s*\w+\s*,\s*-2\s*,\s*\1\s*\)",
                             src, re.S):
            out |= set(re.findall(r'"([A-Za-z_]\w*)"', m.group(2)))
    return out


def globals_provided():
    """Every global name the client answers, however it answers it.

    Four routes, and missing any one of them produces false gaps:

      * a C++ table entry, `{"Name", lua_Name}`
      * an explicit `lua_setglobal(L, "Name")`
      * a function defined in bootstrap Lua, which lives in C++ string
        literals - `function Name(` and `function Name:`
      * a name in a quoted list inside the bootstrap, which is how the counting
        table provides a zero for thirty-five names so that a nil never reaches
        a `for` limit
    """
    names = set()
    for src in _sources():
        names |= set(re.findall(r'\{\s*(?:\.\w+\s*=\s*)?"([A-Za-z_]\w*)"\s*,', src))
        names |= set(re.findall(r'lua_setglobal\(\s*\w+\s*,\s*(?:\.\w+\s*=\s*)?"([A-Za-z_]\w*)"', src))
        names |= set(re.findall(r"function\s+([A-Za-z_]\w*)\s*[:(]", src))
        names |= set(re.findall(r"'([A-Za-z_]\w*)'", src))
    return names


def widget_methods_provided():
    """Method names a frame answers: the method table, Lua shims, the allowlist.

    Separate from globals because they resolve through the widget metatable
    rather than _G, and a name can be one without being the other.
    """
    src = _engine_text()
    table = set(re.findall(r'\{(?:\.\w+\s*=\s*)?"([A-Za-z_]\w*)",\s*(?:\.\w+\s*=\s*)?lua_', src))
    # Every method the bootstrap defines on anything, not only on the frame
    # metatable. Matching `mt:` alone missed animMeta and groupMeta, so
    # IsPlaying, SetDuration and SetOffset were reported as answering nil when
    # the animation system has provided all three since it was written.
    shims = set(re.findall(r'function\s+\w+\s*:\s*(\w+)\s*\(', src))
    # The region method table is built with a `set("Name", fn)` lambda rather
    # than a braced table, and fifty-seven names go on that way. Five of them
    # were answered nowhere else and so read as unprovided - GetTextColor and
    # SetTextHeight among them, both of which had just been implemented. A
    # source of truth that cannot see one of the registration forms sends every
    # sweep that trusts it looking for gaps that are not there.
    region = set(re.findall(r'\bset\("([A-Za-z_]\w*)"', src))
    # Several names per string literal - "SetMovable=1,SetNormalTexture=1,\n" -
    # so anchoring on the opening quote finds only the first of each and
    # under-counts the allowlist by four to one.
    allowlist = set(re.findall(r"\b([A-Za-z]\w*)=1", src))
    # Assigned onto the metatable directly, which is how a method backed by a C
    # function pushed as a global reaches the widget table:
    # `__WoweeFrameMT.SetUnitAura = __WoweeTooltipSetUnitAura`.  Matching only
    # the `function mt:name(` form of the line above missed every one of these,
    # and tooltip_setter_check reported SetUnitAura as a name that raises on a
    # buff icon hover - the loudest answer that sweep has - while the aura
    # tooltip had been built by the C binding all along.
    assigned = set(re.findall(r"\b\w*MT\.([A-Za-z_]\w*)\s*=[^=]", src))
    return table | shims | region | allowlist | assigned | _installed_method_names()


def counting_table():
    """Names given a zero so a nil never reaches a numeric `for` limit.

    Worth having apart: these are deliberately *not* real implementations, and
    they stay in the missing-API report under a "count:" prefix. Binding one
    for real takes it out of that report, which is a loss rather than progress.
    """
    src = _engine_text()
    m = re.search(r"local counting = \{(.*?)\}", src, re.S)
    return set(re.findall(r"'(\w+)'", m.group(1))) if m else set()


def noop_widget_methods():
    """Method names answered only by the no-op - which returns nothing.

    The distinction the other sweeps do not draw. `widget_methods_provided`
    folds the allowlist in with the real ones, because for "does this raise
    outright" they are the same. They are not the same for a caller that reads
    the return: a no-op answers nil, and nil in a comparison raises just as an
    absent method does. GetFieldSize sat in the allowlist and so counted as
    answered, while its one caller compared a byte count against it and the
    guild event log came out blank.
    """
    src = _engine_text()
    real = set(re.findall(r'\{(?:\.\w+\s*=\s*)?"([A-Za-z_]\w*)",\s*(?:\.\w+\s*=\s*)?lua_', src))
    real |= set(re.findall(r'set\("([A-Za-z_]\w*)"', src))
    real |= set(re.findall(r'function\s+\w+\s*:\s*(\w+)\s*\(', src))
    real |= _loop_built(src)
    # A name installed by a sibling's adder is implemented, so it cannot also be
    # the no-op the engine's allowlist claims. None are today; leaving this out
    # would make the next adder report as one.
    real |= _installed_method_names()
    allowlist = set(re.findall(r"\b([A-Za-z]\w*)=1", src))
    return allowlist - real


def _loop_built(src):
    """Method names the bootstrap assembles rather than writes out.

    The button art block is the reason: it loops a list of slot names and
    assigns `mt['Set' .. slot]` and `mt['Get' .. slot]`, so twelve real methods
    appear nowhere as a literal. Reading only literals put GetNormalTexture,
    GetCheckedTexture and their neighbours in the no-op set, which is the
    opposite of true - they are singled out for a real implementation *because*
    the no-op was wrong for them, and the comment above the loop says so.

    Cross-product rather than exact: every affix used with a loop variable
    against every name in every list. Wider than the truth, and wide is the
    safe direction here - a name wrongly called provided drops a row from a
    report, a name wrongly called missing sends someone to implement what
    already works.
    """
    items = set()
    for lst in re.findall(r"ipairs\(\{(.*?)\}\)", src, re.S):
        items |= set(re.findall(r"'([A-Za-z_]\w*)'", lst))
    affixes = set(re.findall(r"\w+\[\s*'([A-Za-z_]\w*)'\s*\.\.", src))
    affixes |= set(re.findall(r"\.\.\s*'([A-Za-z_]\w*)'\s*\]", src))
    return {a + i for a in affixes for i in items} | \
           {i + a for a in affixes for i in items}


if __name__ == "__main__":
    g, w, c = globals_provided(), widget_methods_provided(), counting_table()
    print(f"{len(g)} globals provided")
    print(f"{len(w)} widget methods provided")
    print(f"{len(c)} of the globals are counting-table zeros, which belong in "
          f"the missing-API report and should not be bound away")
