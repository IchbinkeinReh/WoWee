#pragma once

// Turns FrameXML into the Lua that builds it.
//
// The alternative was to build widgets from C++ directly, walking the XML and
// calling into the tree. That would have meant a second implementation of
// everything CreateFrame already does - parenting, naming, templates, script
// binding - kept in step with the first by hand. Emitting Lua instead means XML
// frames and hand-written frames travel the same path, so anything fixed for
// one is fixed for both, and a template defined in XML is usable from a script
// without translation.
//
// Emitting source rather than driving Lua directly also makes this testable
// without a Lua state: the output is a string, and a test can read it.

#include <cstddef>
#include <string>
#include <vector>

namespace wowee {
namespace ui {

struct XmlNode;

struct EmitResult {
    std::string lua;
    /// Files named by <Script file="..."/> and <Include file="..."/>, in the
    /// order they appeared. The caller loads them; resolving paths is its job,
    /// not the emitter's.
    std::vector<std::string> scriptFiles;
    std::vector<std::string> includeFiles;
    /// The same files in document order, each with how much of `lua` had been
    /// written when its element was read. The client loads a <Ui> document
    /// top to bottom: SecureTemplates.xml declares SecureFrameTemplate and
    /// then runs SecureStateDriver.lua, which builds its manager frame from
    /// that template - so the frames before a file are in place before it
    /// runs, and the frames after it are not yet.
    struct FileAt {
        std::string file;
        bool include = false;
        std::size_t luaOffset = 0;
        std::size_t element = 0;   // of `elements`
    };
    std::vector<FileAt> filesInOrder;
    /// The elements of the file, in document order: where in `lua` each begins and what it
    /// is called. The client builds each for itself, and one that fails ends nothing but
    /// itself. What is before the first is for a caller that runs `lua` whole.
    struct Element {
        std::string name;
        std::size_t luaOffset = 0;
    };
    std::vector<Element> elements;
    /// The scripts written in the file: where in `lua` the statement that sets one is, and
    /// the function it sets. One that does not compile is left unset (0x0081a0f0's message),
    /// and what it is of is built.
    struct Script {
        std::string name;
        std::size_t luaOffset = 0;
        std::size_t length = 0;
        std::string function;
    };
    std::vector<Script> scripts;
    std::vector<std::string> warnings;
};

/// Emit Lua for a parsed <Ui> document. `virtualPrefix` names the table that
/// virtual (template) frames are recorded in so later `inherits` can find them.
EmitResult emitFrameXml(const XmlNode& root);

/// Whether a name begins with "$parent", in any case (0x0048b540).
[[nodiscard]] bool namedAfterParent(const std::string& name);

/// Substitute FrameXML's $parent token. A child named "$parentText" inside
/// "FooFrame" becomes "FooFrameText"; this is how nearly every region in the
/// original interface is named.
std::string substituteParent(const std::string& name, const std::string& parentName);

} // namespace ui
} // namespace wowee
