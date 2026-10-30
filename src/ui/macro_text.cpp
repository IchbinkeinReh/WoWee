#include "ui/macro_text.hpp"

namespace wowee {
namespace ui {
namespace {

/// One line of a macro body, trimmed the way both readers trimmed it.
///
/// Walks by index rather than splitting, because a macro body has no trailing
/// newline and the last line has to be read all the same.
class LineReader {
public:
    explicit LineReader(const std::string& text) : text_(text) {}

    bool next(std::string& out) {
        if (pos_ > text_.size()) return false;
        const size_t nl = text_.find('\n', pos_);
        out = (nl != std::string::npos) ? text_.substr(pos_, nl - pos_)
                                        : text_.substr(pos_);
        // A macro written or edited on Windows carries these, and a command
        // with one on the end matches nothing.
        if (!out.empty() && out.back() == '\r') out.pop_back();
        const size_t start = out.find_first_not_of(" \t");
        if (start != std::string::npos) out = out.substr(start);
        else out.clear();
        if (nl == std::string::npos) {
            pos_ = text_.size() + 1;
        } else {
            pos_ = nl + 1;
        }
        return true;
    }

private:
    const std::string& text_;
    size_t pos_ = 0;
};

/// The two prefixes a macro line may carry and still not be executed.
///
/// 0x00564db0 - the routine that runs a macro, from the bar, from RunMacro,
/// from RunMacroText and from a "MACRO name" binding alike - walks the body
/// line by line and hands over a line only when its first character is
/// neither of these. '#' is the directive line the button reads a tooltip from
/// (see macroShowtooltipArg); '-' the client skips beside it, and nothing the
/// interface or this client writes into a macro body begins that way, so a
/// line that starts with one is simply not something to run. It used to be
/// here: a body line "-hello" said "-hello" in chat, where the real client
/// says nothing at all.
///
/// The same loop is also where /stopmacro takes effect - its condition holds
/// while the record of the running macro is still set, which is what stops
/// ChatPanel's own loop short. And the real client hands each of these lines
/// to FrameXML (EXECUTE_CHAT_LINE, 383, answered by the hidden MacroEditBox in
/// chatframe.lua:2468, which copies the chat frame's target attributes and
/// sends the line as chat), while this client runs them through its own slash
/// dispatch instead. Raised here as well, every line would run twice.
bool executedAsMacroLine(const std::string& line) {
    return !line.empty() && line.front() != '#' && line.front() != '-';
}

}  // namespace

std::vector<std::string> macroCommandLines(const std::string& macroText) {
    std::vector<std::string> commands;
    LineReader reader(macroText);
    std::string line;
    while (reader.next(line)) {
        if (executedAsMacroLine(line)) {
            commands.push_back(line);
        }
    }
    return commands;
}

std::string macroShowtooltipArg(const std::string& macroText) {
    LineReader reader(macroText);
    std::string line;
    while (reader.next(line)) {
        if (line.rfind("#showtooltip", 0) != 0 && line.rfind("#show", 0) != 0) {
            continue;
        }
        const size_t space = line.find(' ');
        if (space != std::string::npos) {
            std::string arg = line.substr(space + 1);
            const size_t first = arg.find_first_not_of(" \t");
            // Both copies this replaces tested only whether the argument was
            // empty, which a run of spaces is not, so "#showtooltip   " came
            // back as "   " and was then looked up as the name of a spell.
            if (first != std::string::npos) {
                arg = arg.substr(first);
                const size_t last = arg.find_last_not_of(" \t");
                if (last != std::string::npos) arg.resize(last + 1);
                if (!arg.empty()) return arg;
            }
        }
        // The directive with nothing usable after it: follow the macro.
        return "__auto__";
    }
    return {};
}

}  // namespace ui
}  // namespace wowee
