#pragma once

#include <string>

namespace wowee {
namespace ui {

/// Which form of a `|4one:two[:three];` escape a count takes (0x00817d10),
/// by the rule the client picks for its locale at start-up (0x00405dd0 ->
/// 0x00817d00, the table at 0x00ad2fe0):
///   0 - enUS, enGB, deDE, esES, esMX and anything else: the second form for
///       any count but 1;
///   1 - koKR, frFR, zhCN, zhTW: the second form for a count over 1;
///   2 - ruRU: the third form for 11..14 (mod 100), the first for a last
///       digit of 1, the second for 2..4, else the third.
int pluralRuleForLocale(const std::string& locale);
int pluralFormIndex(int rule, long count);

/// The rule every escape is resolved under, set once the client's locale is
/// known (as 0x00817d00 sets 0x00d3f7a0). Rule 0 until then.
void setPluralLocale(const std::string& locale);
int pluralFormIndex(long count);

/// Resolve WoW's `|4form:form[:form];` escapes as the font-string text pass
/// does (0x00481fc0, reached from 0x004820e0 for an escape numbered 4).
///
/// The count is the value of the last run of digits read anywhere before the
/// escape - colour codes and words included - and 0 when there is none. The
/// form its index names is written, its leading spaces dropped; an escape
/// with fewer forms than that index writes nothing, and one never closed by
/// a ';' ends the text there. `|4` followed by a digit, and every other bar
/// escape, is written as it stands.
std::string resolvePluralEscapes(const std::string& text);

}  // namespace ui
}  // namespace wowee
