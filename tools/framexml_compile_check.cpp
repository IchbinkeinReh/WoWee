// Does the Lua we generate from FrameXML actually compile - and build?
//
// The unit tests check the shape of the output against cases someone thought
// of. This asks Lua itself about every real file, which is a different and
// harsher question - it found an empty function attribute emitting
// SetScript("X", ), and temporaries running into Lua's limit of 200 locals per
// function. Neither degrades: the whole chunk refuses to compile.
//
// Compiling is not building. T378: the emitter wrote SetNonSpaceWrap on
// ChatFrameTemplate's ScrollingMessageFrame. That compiles, but the type has
// no such method (its table 0x00b2cee0; widget_types.hpp), the call raised and
// ChatFrame1..7 were never built. So the second half runs the emitted Lua -
// every file in load order, then every template on an object of its own type -
// against stand-in objects whose methods are exactly their type's as the
// engine gives them (kWidgetTypesLua: a method some type has and this one
// lacks answers nil). A call of such a method is a failure. Other errors of
// the run (a FrameXML function the stand-ins do not provide) are counted and
// shown, not failed: they are the stand-in's limits, not the emitter's.
//
//   framexml_compile_check <folder of extracted XML>
//   framexml_compile_check --mpq <client Data folder> interface/framexml/
//   framexml_compile_check --mpq <client Data folder> interface/addons/
// WOWEE_FRAMEXML_EMIT_DIR=<dir> also writes each file's emitted Lua there.
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
#include "addons/widget_types.hpp"
#include "pipeline/mpq_archive_set.hpp"
#include "ui/framexml_emitter.hpp"
#include "ui/xml_parser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

// Paths as the archives write them: lower case, backslashes.
std::string normalized(std::string p) {
    for (char& c : p) {
        c = (c == '/') ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return p;
}

std::string dirOf(const std::string& p) {
    size_t at = p.find_last_of('\\');
    return at == std::string::npos ? std::string() : p.substr(0, at + 1);
}

// "a\b\..\c.xml" -> "a\c.xml"
std::string collapsed(const std::string& p) {
    std::vector<std::string> parts;
    std::string part;
    std::istringstream in(p);
    while (std::getline(in, part, '\\')) {
        if (part.empty() || part == ".") continue;
        if (part == ".." && !parts.empty()) { parts.pop_back(); continue; }
        parts.push_back(part);
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) out += (i ? "\\" : "") + parts[i];
    return out;
}

std::string baseName(const std::string& path) {
    size_t at = path.find_last_of("\\/");
    return at == std::string::npos ? path : path.substr(at + 1);
}

struct Input {
    // normalised path -> text; the XML files and the .toc files.
    std::map<std::string, std::string> files;
    // The files whose results count (the rest are loaded first, as the game
    // has FrameXML in place before any addon).
    std::string checkedPrefix;
    // The .toc files, in the order they are loaded.
    std::vector<std::string> tocs;
};

// From a folder of extracted XML, or - with "--mpq <client Data> <prefix>" -
// straight out of the original client's archives, as the game reads them.
// The folder form is what this always took, and on this project nobody keeps
// an extracted copy in the tree, so the test skipped on every run and the
// emitter's T378 regression (all chat frames unbuilt) went out unseen.
bool gatherFiles(int argc, char** argv, Input& in) {
    auto wanted = [](const std::string& n) {
        return n.size() > 4 && (n.compare(n.size() - 4, 4, ".xml") == 0 ||
                                n.compare(n.size() - 4, 4, ".toc") == 0);
    };
    if (argc > 3 && std::string(argv[1]) == "--mpq") {
        wowee::pipeline::MpqArchiveSet mpq;
        if (!mpq.open(argv[2])) {
            printf("no client archives at %s; nothing to check\n", argv[2]);
            return false;
        }
        // The prefix is given with slashes so no shell or argv parser eats a
        // backslash.
        in.checkedPrefix = normalized(argv[3]);
        const std::string frameXml = "interface\\framexml\\";
        std::vector<std::string> prefixes{in.checkedPrefix};
        if (in.checkedPrefix != frameXml) prefixes.insert(prefixes.begin(), frameXml);
        for (const auto& prefix : prefixes) {
            for (const auto& name : mpq.list(prefix)) {
                if (!wanted(name)) continue;
                auto bytes = mpq.read(name);
                in.files[name] = std::string(bytes.begin(), bytes.end());
                if (name.compare(name.size() - 4, 4, ".toc") == 0) in.tocs.push_back(name);
            }
        }
    } else {
        // Run from the build with no argument and it looks where the game data
        // normally sits, so this can be a test rather than something remembered.
        const char* dir = (argc > 1) ? argv[1] : "Data/interface/framexml";
        if (!std::filesystem::is_directory(dir)) {
            printf("no FrameXML at %s; nothing to check\n", dir);
            return false;
        }
        in.checkedPrefix = normalized(std::filesystem::path(dir).string());
        // Recursive so this works on the addons directory too, where each addon
        // is a folder of its own. FrameXML is flat and unaffected.
        for (auto& e : std::filesystem::recursive_directory_iterator(dir)) {
            const std::string name = normalized(e.path().string());
            if (!wanted(name)) continue;
            std::ifstream f(e.path(), std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            in.files[name] = ss.str();
            if (name.compare(name.size() - 4, 4, ".toc") == 0) in.tocs.push_back(name);
        }
    }
    // FrameXML's own .toc first: its templates are what every addon inherits.
    std::stable_sort(in.tocs.begin(), in.tocs.end(), [](const std::string& a, const std::string& b) {
        return (a.find("\\framexml\\") != std::string::npos) > (b.find("\\framexml\\") != std::string::npos);
    });
    for (const auto& [name, text] : in.files) {
        if (name.compare(0, in.checkedPrefix.size(), in.checkedPrefix) == 0 &&
            name.compare(name.size() - 4, 4, ".xml") == 0)
            return true;
    }
    printf("no XML under %s; nothing to check\n", in.checkedPrefix.c_str());
    return false;
}

// Stand-ins for the engine: objects with their type's methods and no more.
constexpr const char* kStandIns = R"LUA(
local types = ...
local own, all = {}, {}
for _, t in ipairs(types) do
    local mine = {}
    for name in pairs(own[t.parent] or {}) do mine[name] = true end
    for name in t.methods:gmatch('%S+') do mine[name] = true; all[name] = true end
    own[t.name] = mine
end
local typeOf = {}
for name in pairs(own) do typeOf[name:lower()] = name end

local function noop() end
local make
local special = {
    CreateTexture = function(self, name) return make('Texture', name, self) end,
    CreateFontString = function(self, name) return make('FontString', name, self) end,
    CreateTitleRegion = function(self) return make('Region', nil, self) end,
    CreateAnimationGroup = function(self) return make(nil, nil, self) end,
    CreateAnimation = function(self) return make(nil, nil, self) end,
    GetParent = function(self) return rawget(self, '__parent') end,
    GetName = function(self) return rawget(self, '__name') end,
    GetObjectType = function(self) return rawget(self, '__kind') or 'Frame' end,
    HasScript = function() return true end,
}
local metas = {}
local function metaOf(kind)
    local key = kind or '*'
    if metas[key] then return metas[key] end
    local mine = kind and own[kind]
    local meta = {__index = function(_, name)
        if type(name) ~= 'string' then return nil end
        -- A method some type has and this one lacks answers nil
        -- (kWidgetTypesLua), so calling it raises.
        if mine and all[name] and not mine[name] then return nil end
        if special[name] then return special[name] end
        if name:match('^%u') then return noop end
        return nil
    end}
    metas[key] = meta
    return meta
end
function make(kind, name, parent)
    local o = {__kind = kind, __parent = parent}
    if type(name) == 'string' and name ~= '' then
        if name:lower():sub(1, 7) == '$parent' then
            local p = parent and rawget(parent, '__name')
            name = p and (p .. name:sub(8)) or nil
        end
        if name then o.__name = name; rawset(_G, name, o) end
    end
    return setmetatable(o, metaOf(kind))
end
function special.IsObjectType(self, t)
    local kind = rawget(self, '__kind')
    t = typeOf[tostring(t):lower()]
    if not kind or not t then return false end
    for _, x in ipairs(types) do
        if x.name == kind then
            local k = kind
            while k and k ~= '' do
                if k == t then return true end
                local parent
                for _, y in ipairs(types) do if y.name == k then parent = y.parent end end
                k = parent
            end
        end
    end
    return false
end

__WoweeTemplates = {}
__WoweeTemplateTypes = {}
function __WoweeTemplate(f) return f end
function __WoweeMissingTemplate() end
-- Font objects: the engine's FontInstance methods, not checked here.
__WoweeFontMT = metaOf(nil)
function CreateFrame(kind, name, parent, templates)
    -- An unknown type gets the engine's one table of all methods.
    local o = make(typeOf[tostring(kind):lower()], name, parent)
    if type(templates) == 'string' then
        for t in templates:gmatch('[^,%s]+') do
            local f = __WoweeTemplates[t]
            if f then f(o) end
        end
    end
    return o
end
UIParent = make('Frame', 'UIParent')
WorldFrame = make('Frame', 'WorldFrame')
setmetatable(_G, {__index = function(t, name)
    if type(name) == 'string' and name:sub(1, 7) == '__Wowee' then
        local stub = setmetatable({}, {__call = noop})
        rawset(t, name, stub)
        return stub
    end
end})

local failures, limits = {}, {}
function __CheckRun(label, fn, ...)
    local ok, err = pcall(fn, ...)
    if ok then return end
    err = tostring(err)
    local m = err:match("attempt to call method '([%w_]+)'")
    if m and all[m] then failures[#failures + 1] = label .. ': ' .. err
    else limits[#limits + 1] = label .. ': ' .. err end
end
function __CheckTemplates()
    local names = {}
    for n in pairs(__WoweeTemplateTypes) do names[#names + 1] = n end
    table.sort(names)
    for _, n in ipairs(names) do
        local kind = typeOf[tostring(__WoweeTemplateTypes[n]):lower()]
        local f = __WoweeTemplates[n]
        if kind and type(f) == 'function' then
            __CheckRun('template ' .. n .. ' (' .. kind .. ')', f, make(kind, nil, UIParent))
        end
    end
    return failures, limits
end
)LUA";

}  // namespace

int main(int argc, char** argv) {
    Input in;
    if (!gatherFiles(argc, argv, in))
        return 77;  // ctest reads this as skipped, not passed.
    // Somewhere to read the result, because "it compiles" and "it says what it
    // should" are different questions and only the first was ever asked.
    const char* dumpDir = std::getenv("WOWEE_FRAMEXML_EMIT_DIR");
    if (dumpDir && *dumpDir) {
        std::error_code ec;
        std::filesystem::create_directories(dumpDir, ec);
    }
    auto checked = [&](const std::string& name) {
        return name.compare(0, in.checkedPrefix.size(), in.checkedPrefix) == 0;
    };

    // ── Does each file's Lua compile? ──
    lua_State* L = luaL_newstate();
    int ok = 0, bad = 0, shown = 0, unparsed = 0, unbuilt = 0;
    std::map<std::string, wowee::ui::EmitResult> emitted;
    for (const auto& [path, text] : in.files) {
        if (path.compare(path.size() - 4, 4, ".xml") != 0) continue;
        const bool counts = checked(path);
        const std::string fileName = baseName(path);
        wowee::ui::XmlNode root; std::string err;
        // Counted, not skipped. A file the reader cannot get through never
        // reaches the compiler at all, so passing over it quietly reports a
        // clean run on files that in fact never loaded.
        if (!wowee::ui::parseXml(text, root, err)) {
            if (counts) {
                ++unparsed;
                printf("  UNPARSED %-30s %s\n", fileName.c_str(), err.c_str());
            }
            continue;
        }
        auto r = wowee::ui::emitFrameXml(root);
        if (counts && dumpDir && *dumpDir) {
            std::ofstream out(std::filesystem::path(dumpDir) / (fileName + ".lua"));
            if (out) out << r.lua;
        }
        if (counts) {
            for (const auto& w : r.warnings) {
                if (w.find("not a known frame type") != std::string::npos) {
                    printf("  UNBUILT  %-30s %s\n", fileName.c_str(), w.c_str());
                    ++unbuilt;
                }
            }
        }
        std::string chunk = "local __WoweeTemplates={} "
                            "local function __WoweeMissingTemplate() end\n" + r.lua;
        if (r.lua.empty() || luaL_loadbuffer(L, chunk.c_str(), chunk.size(), "=chunk") == 0) {
            if (counts) ++ok;
            emitted[path] = std::move(r);
        } else if (counts) {
            ++bad;
            if (shown++ < 6)
                printf("  FAIL %-34s %s\n", fileName.c_str(), lua_tostring(L, -1));
        }
        lua_settop(L, 0);
    }
    lua_close(L);
    printf("emitted Lua compiles: %d   fails: %d   unparsed XML: %d   "
           "unbuilt elements: %d\n", ok, bad, unparsed, unbuilt);

    // ── Does it build? Load order: each .toc's files (an XML file's includes
    // before it), then whatever no .toc reached. ──
    std::vector<std::string> order;
    std::set<std::string> seen;
    auto visit = [&](auto&& self, const std::string& path) -> void {
        if (!seen.insert(path).second) return;
        auto it = emitted.find(path);
        if (it == emitted.end()) return;
        for (const auto& inc : it->second.includeFiles)
            self(self, collapsed(normalized(dirOf(path) + inc)));
        order.push_back(path);
    };
    for (const auto& toc : in.tocs) {
        std::istringstream lines(in.files[toc]);
        for (std::string line; std::getline(lines, line);) {
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
            size_t start = 0;
            while (start < line.size() && std::isspace(static_cast<unsigned char>(line[start]))) ++start;
            line = line.substr(start);
            if (line.empty() || line[0] == '#') continue;
            visit(visit, collapsed(normalized(dirOf(toc) + line)));
        }
    }
    for (const auto& [path, r] : emitted) visit(visit, path);

    lua_State* B = luaL_newstate();
    // The interface's own libraries (lua_std_libs.cpp), no more.
    for (const lua_CFunction open : {luaopen_base, luaopen_table, luaopen_string, luaopen_math}) {
        lua_pushcfunction(B, open);
        lua_call(B, 0, 0);
    }
    if (luaL_loadstring(B, kStandIns) != 0) {
        printf("stand-ins do not compile: %s\n", lua_tostring(B, -1));
        return 1;
    }
    lua_newtable(B);
    int at = 0;
    for (const auto& t : wowee::addons::kWidgetTypes) {
        lua_newtable(B);
        lua_pushstring(B, t.name);    lua_setfield(B, -2, "name");
        lua_pushstring(B, t.parent);  lua_setfield(B, -2, "parent");
        lua_pushstring(B, t.methods); lua_setfield(B, -2, "methods");
        lua_rawseti(B, -2, ++at);
    }
    if (lua_pcall(B, 1, 0, 0) != 0) {
        printf("stand-ins fail: %s\n", lua_tostring(B, -1));
        return 1;
    }
    for (const auto& path : order) {
        const std::string& lua = emitted[path].lua;
        if (lua.empty()) continue;
        const std::string label = "=" + baseName(path);
        lua_getglobal(B, "__CheckRun");
        lua_pushstring(B, baseName(path).c_str());
        if (luaL_loadbuffer(B, lua.c_str(), lua.size(), label.c_str()) != 0) {
            lua_settop(B, 0);
            continue;
        }
        lua_pcall(B, 2, 0, 0);
        lua_settop(B, 0);
    }
    lua_getglobal(B, "__CheckTemplates");
    lua_pcall(B, 0, 2, 0);
    // Only what a checked file names counts; the files loaded before them
    // (FrameXML under an addon run) are their own test's business.
    std::set<std::string> checkedNames;
    for (const auto& path : order) if (checked(path)) checkedNames.insert(baseName(path));
    // A message names the file it was raised in ("chatframe.xml:12: ..."),
    // a template's too: its function was compiled from that file.
    auto ofChecked = [&](const std::string& msg) {
        for (const auto& n : checkedNames)
            if (msg.find(n + ":") != std::string::npos) return true;
        return false;
    };
    int failures = 0, limits = 0, limitsShown = 0;
    lua_Integer n = static_cast<lua_Integer>(lua_objlen(B, 1));
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(B, 1, static_cast<int>(i));
        const std::string msg = lua_tostring(B, -1);
        lua_pop(B, 1);
        if (!ofChecked(msg)) continue;
        ++failures;
        printf("  NO SUCH METHOD %s\n", msg.c_str());
    }
    n = static_cast<lua_Integer>(lua_objlen(B, 2));
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(B, 2, static_cast<int>(i));
        const std::string msg = lua_tostring(B, -1);
        lua_pop(B, 1);
        if (!ofChecked(msg)) continue;
        ++limits;
        if (limitsShown++ < 8) printf("  (stand-in limit) %s\n", msg.c_str());
    }
    lua_close(B);
    printf("built: %zu files in load order   methods the type lacks: %d   "
           "stand-in limits (not failed): %d\n", order.size(), failures, limits);
    // A file that will not compile is lost whole, and a method the type lacks
    // loses the element that calls it, so these are failures rather than
    // numbers to read past.
    return (bad > 0 || unparsed > 0 || failures > 0) ? 1 : 0;
}
