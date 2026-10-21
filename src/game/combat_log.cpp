#include "game/combat_log.hpp"

#include <cctype>
#include <cstdio>

namespace wowee::game::combat_log {

namespace {

constexpr std::array<const char*, kEventCount> kEventNames = {
    "ENVIRONMENTAL_DAMAGE", "SWING_DAMAGE", "SWING_MISSED", "RANGE_DAMAGE", "RANGE_MISSED",
    "SPELL_CAST_START", "SPELL_CAST_SUCCESS", "SPELL_CAST_FAILED", "SPELL_MISSED", "SPELL_DAMAGE",
    "SPELL_HEAL", "SPELL_ENERGIZE", "SPELL_DRAIN", "SPELL_LEECH", "SPELL_INSTAKILL",
    "SPELL_SUMMON", "SPELL_CREATE", "SPELL_INTERRUPT", "SPELL_EXTRA_ATTACKS", "SPELL_DURABILITY_DAMAGE",
    "SPELL_DURABILITY_DAMAGE_ALL", "SPELL_AURA_APPLIED", "SPELL_AURA_APPLIED_DOSE",
    "SPELL_AURA_REMOVED_DOSE", "SPELL_AURA_REMOVED",
    "SPELL_AURA_REFRESH", "SPELL_DISPEL", "SPELL_STOLEN", "SPELL_AURA_BROKEN", "SPELL_AURA_BROKEN_SPELL",
    "DAMAGE_AURA_BROKEN", "ENCHANT_APPLIED", "ENCHANT_REMOVED", "SPELL_PERIODIC_MISSED",
    "SPELL_PERIODIC_DAMAGE",
    "SPELL_PERIODIC_HEAL", "SPELL_PERIODIC_ENERGIZE", "SPELL_PERIODIC_DRAIN", "SPELL_PERIODIC_LEECH",
    "SPELL_DISPEL_FAILED",
    "DAMAGE_SHIELD", "DAMAGE_SHIELD_MISSED", "DAMAGE_SPLIT", "PARTY_KILL", "UNIT_DIED",
    "UNIT_DESTROYED", "SPELL_RESURRECT", "SPELL_BUILDING_DAMAGE", "SPELL_BUILDING_HEAL", "UNIT_DISSIPATES",
};

/// 0x00adbfac.
constexpr std::array<const char*, 14> kMissNames = {
    "NONE", "MISS", "RESIST", "DODGE", "PARRY", "BLOCK", "EVADE",
    "IMMUNE", "IMMUNE", "DEFLECT", "ABSORB", "REFLECT", "COMBAT_TEXT_NONE", "COMBAT_TEXT_MISS",
};

/// 0x00ad671c: the environmental damage the packet's type names.
constexpr std::array<const char*, 8> kEnvironmentalNames = {
    "FATIGUE", "DROWNING", "FALLING", "LAVA", "SLIME", "FIRE", "None", "Root",
};

template <size_t N>
Arg tableName(const std::array<const char*, N>& table, uint32_t index) {
    // The client indexes the table without a check; past it is nothing it
    // could name.
    return index < N ? Arg::str(table[index]) : Arg::nil();
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

/// "name" or "name-realm", nil for a side nothing named.
Arg partyName(const Party& p) {
    if (!p.name) return Arg::nil();
    if (!p.realm.empty()) return Arg::str(*p.name + "-" + p.realm);
    if (p.name->empty()) return Arg::nil();
    return Arg::str(*p.name);
}

/// The three values a spell is pushed as: id, name and school; nil and 0
/// for one the client has no row of.
void pushSpell(std::vector<Arg>& out, uint32_t spellId, const SpellLookup& spells) {
    out.push_back(Arg::num(spellId));
    const auto info = spells ? spells(spellId) : std::nullopt;
    if (info) {
        out.push_back(Arg::str(info->name));
        out.push_back(Arg::num(info->school));
    } else {
        out.push_back(Arg::nil());
        out.push_back(Arg::num(0));
    }
}

Arg numberOrNil(int32_t v) { return v == 0 ? Arg::nil() : Arg::num(v); }
Arg oneOrNil(bool v) { return v ? Arg::num(1) : Arg::nil(); }

}  // namespace

const char* eventName(Event event) {
    const auto i = static_cast<size_t>(event);
    return i < kEventCount ? kEventNames[i] : "";
}

uint32_t unitFlags(const UnitWorld& world, uint64_t guid) {
    using namespace object;
    if (guid == 0) return kNone;
    const bool inRaid = world.inRaid();
    const bool grouped = inRaid || world.hasPartyMembers();

    // Who controls it: a unit's charmer or maker, a game object's maker, and
    // that controller's own the same way when it is not a player.
    uint64_t ctrl = guid;
    const UnitWorld::Object obj = world.object(guid);
    const bool isUnit = obj.kind == UnitWorld::Object::Kind::Unit;
    if (obj.kind != UnitWorld::Object::Kind::None) {
        if (isUnit) {
            const uint64_t owner = obj.charmedBy != 0 ? obj.charmedBy : obj.createdBy;
            if (owner != 0) ctrl = owner;
        } else if (obj.kind == UnitWorld::Object::Kind::GameObject) {
            ctrl = obj.createdBy;
        }
        if (ctrl != guid && !isPlayerGuid(ctrl)) {
            const UnitWorld::Object above = world.object(ctrl);
            if (above.kind == UnitWorld::Object::Kind::Unit) {
                const uint64_t owner = above.charmedBy != 0 ? above.charmedBy : above.createdBy;
                if (owner != 0) ctrl = owner;
            }
        }
    }

    uint32_t flags;
    const uint32_t kind = guidKind(guid);
    if (isPlayerGuid(guid) || kind == kGuidCreature || kind == kGuidVehicle || kind == kGuidPet) {
        if (ctrl == guid) {
            flags = isPlayerGuid(guid) ? kTypePlayer : (kind == kGuidPet ? kTypePet : kTypeNpc);
        } else {
            // Somebody else's: a pet when charmed or summoned, a guardian when
            // only made.
            const uint64_t by = obj.charmedBy != 0 ? obj.charmedBy : obj.summonedBy;
            flags = by == 0 ? kTypeGuardian : kTypePet;
        }
    } else {
        flags = kTypeObject;
    }
    flags |= isPlayerGuid(ctrl) ? kControlPlayer : kControlNpc;

    if (ctrl == world.activePlayer()) {
        flags |= kAffiliationMine | kReactionFriendly;
    } else {
        bool groupCheck = true;
        if (isUnit) {
            if (const auto r = world.reactionToPlayer(guid)) {
                if (*r > 3) {
                    flags |= kReactionFriendly;
                } else if (*r <= 1) {
                    flags |= kReactionHostile;
                    groupCheck = false;
                }
            }
        }
        if (groupCheck && grouped) {
            if (world.partyMember(ctrl)) flags |= kAffiliationParty | kReactionFriendly;
            else if (inRaid && world.raidMember(ctrl)) flags |= kAffiliationRaid | kReactionFriendly;
        }
        if ((flags & 0xFu) == 0) flags |= kAffiliationOutsider;
        if ((flags & 0xF0u) == 0) flags |= kReactionNeutral;
    }
    if (guid == world.target()) flags |= kTarget;
    if (guid == world.focus()) flags |= kFocus;

    if ((flags & kTypePlayer) != 0) {
        bool roles = true;
        if ((flags & (kAffiliationParty | kAffiliationRaid)) == 0) {
            if (world.activePlayer() != ctrl) roles = false;
            else if (!grouped) return flags;
        }
        if (roles) {
            const uint32_t member = world.memberFlags(guid, inRaid);
            if ((member & 2u) != 0) flags |= kMainTank;
            else if ((member & 4u) != 0) flags |= kMainAssist;
        }
    }
    if (grouped) {
        const uint32_t icon = world.raidTarget(guid);
        if (icon < 8) flags |= kRaidTarget1 << icon;
    }
    return flags;
}

std::string guidString(uint64_t guid) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(guid));
    return buf;
}

uint64_t parseGuid(std::string_view text) {
    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text.remove_prefix(2);
    uint64_t v = 0;
    for (size_t i = 0; i < text.size() && i < 16; ++i) {
        const char c = text[i];
        uint64_t d;
        if (c >= '0' && c <= '9') d = static_cast<uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<uint64_t>(c - 'A' + 10);
        else break;
        v = (v << 4) | d;
    }
    return v;
}

std::vector<Arg> entryArgs(const Entry& e, double timestamp, const SpellLookup& spells) {
    std::vector<Arg> out;
    out.reserve(20);
    out.push_back(Arg::num(timestamp));
    out.push_back(Arg::str(eventName(e.event)));
    out.push_back(Arg::str(guidString(e.src.guid)));
    out.push_back(partyName(e.src));
    out.push_back(Arg::num(e.src.flags));
    out.push_back(Arg::str(guidString(e.dst.guid)));
    out.push_back(partyName(e.dst));
    out.push_back(Arg::num(e.dst.flags));
    if (e.spellId != 0) pushSpell(out, e.spellId, spells);

    const uint32_t s = e.suffix;
    if (s & suffix::kAmount) out.push_back(Arg::num(e.amount));
    if (s & suffix::kFailedType) out.push_back(Arg::str(e.failedType));
    if (s & suffix::kMissType) out.push_back(tableName(kMissNames, e.missType));
    if (s & suffix::kEnvironmental) out.push_back(tableName(kEnvironmentalNames, e.environmentalType));
    if (s & suffix::kDamage) {
        out.push_back(Arg::num(e.damage.amount));
        out.push_back(Arg::num(e.damage.overkill));
        out.push_back(Arg::num(e.damage.school));
        out.push_back(numberOrNil(e.damage.resisted));
        out.push_back(numberOrNil(e.damage.blocked));
        out.push_back(numberOrNil(e.damage.absorbed));
        out.push_back(oneOrNil(e.damage.critical));
        out.push_back(oneOrNil(e.damage.glancing));
        out.push_back(oneOrNil(e.damage.crushing));
    }
    if (s & suffix::kMissAbsorbed) out.push_back(Arg::num(e.missAbsorbed));
    if (s & suffix::kMissResisted) out.push_back(Arg::num(e.missResisted));
    if (s & suffix::kMissBlocked) out.push_back(Arg::num(e.missBlocked));
    if (s & suffix::kHeal) {
        out.push_back(Arg::num(e.heal.amount));
        out.push_back(Arg::num(e.heal.overhealing));
        out.push_back(Arg::num(e.heal.absorbed));
        out.push_back(oneOrNil(e.heal.critical));
    }
    if (s & suffix::kEnergize) {
        out.push_back(Arg::num(e.power.amount));
        out.push_back(Arg::num(e.power.powerType));
    }
    if (s & suffix::kDrain) {
        out.push_back(Arg::num(e.power.amount));
        out.push_back(Arg::num(e.power.powerType));
        out.push_back(numberOrNil(e.power.extraAmount));
    }
    if (s & suffix::kExtraSpell) pushSpell(out, e.extraSpellId, spells);
    if (s & suffix::kEnchantName) out.push_back(Arg::str(e.enchantName));
    if (s & suffix::kItem) {
        out.push_back(Arg::num(e.itemId));
        out.push_back(e.itemName ? Arg::str(*e.itemName) : Arg::nil());
    }
    if (s & suffix::kAuraType) out.push_back(Arg::str(e.auraDebuff ? "DEBUFF" : "BUFF"));
    if (s & suffix::kAuraAmount) out.push_back(Arg::num(e.auraAmount));
    return out;
}

void Filter::setEvents(const std::optional<std::string>& list) {
    if (!list) {
        events.fill(true);
        return;
    }
    events.fill(false);
    std::string_view rest = *list;
    while (!rest.empty()) {
        const size_t start = rest.find_first_not_of(" ,");
        if (start == std::string_view::npos) break;
        rest.remove_prefix(start);
        const size_t end = rest.find_first_of(" ,");
        // 0x0076f1e0 hands back at most 127 characters of a token.
        const std::string_view token = rest.substr(0, std::min<size_t>(end, 127));
        for (size_t i = 0; i < kEventCount; ++i) {
            if (equalsIgnoreCase(token, kEventNames[i])) {
                events[i] = true;
                break;
            }
        }
        if (end == std::string_view::npos) break;
        rest.remove_prefix(end);
    }
}

bool Filter::matches(const Entry& e) const {
    const auto index = static_cast<size_t>(e.event);
    if (index >= kEventCount || !events[index]) return false;
    if (srcGuid == 0) {
        if (!maskComplete(srcMask & e.src.flags)) return false;
    } else if (e.src.guid != srcGuid) {
        return false;
    }
    if (dstGuid == 0) {
        if (!maskComplete(dstMask & e.dst.flags)) return false;
    } else if (e.dst.guid != dstGuid) {
        return false;
    }
    if (spellId != 0) return e.spellId == spellId;
    if (spellName) return e.filterSpellName && equalsIgnoreCase(*e.filterSpellName, *spellName);
    return true;
}

void Log::reset(int64_t nowMs, int64_t unixSeconds) {
    entries_.clear();
    draft_.reset();
    current_ = entries_.end();
    announce_ = entries_.end();
    filters_.clear();
    baseMs_ = nowMs;
    baseUnix_ = unixSeconds;
}

double Log::timestampOf(const Entry& entry) const {
    // An unsigned difference, as the client takes it.
    const auto diff = static_cast<uint32_t>(entry.timeMs - baseMs_);
    return static_cast<double>(diff) * 0.001 + static_cast<double>(baseUnix_);
}

void Log::remove(It it) {
    if (announce_ == it) announce_ = std::next(it);
    if (current_ == it) current_ = std::next(it);
    entries_.erase(it);
}

Entry& Log::create(Event event) {
    // The oldest goes once it has been kept long enough - and only when a new
    // one is made, one at a time.
    if (!entries_.empty() && hooks_.nowMs) {
        const int64_t keepMs = static_cast<int64_t>(hooks_.retentionSeconds ? hooks_.retentionSeconds() : 300) * 1000;
        if (static_cast<int32_t>(hooks_.nowMs() - keepMs - entries_.front().timeMs) >= 0) {
            remove(entries_.begin());
        }
    }
    draft_.emplace();
    draft_->event = event;
    return *draft_;
}

void Log::finalize(Entry& e) const {
    // 0x0074d210: a creature whose template hides it is logged without its
    // spawn - the low 24 bits of the guid go.
    if (!hooks_.hidesGuid) return;
    for (Party* p : {&e.src, &e.dst}) {
        const uint32_t kind = guidKind(p->guid);
        if ((kind == kGuidCreature || kind == kGuidVehicle) && hooks_.hidesGuid(p->guid))
            p->guid &= 0xFFFFFFFFFF000000ull;
    }
}

void Log::commit(uint32_t delayMs) {
    if (!draft_) return;
    draft_->timeMs = (hooks_.nowMs ? hooks_.nowMs() : 0) - static_cast<int64_t>(delayMs);
    entries_.push_back(std::move(*draft_));
    draft_.reset();
    const It added = std::prev(entries_.end());
    if (announce_ != entries_.end()) return;  // held behind one still waiting
    announce_ = added;
    if (added->waiting()) return;
    finalize(*added);
    announcePending();
}

void Log::announcePending() {
    // 0x0074f910: in order, until one still waits for a name. A handler may
    // add or clear entries meanwhile; one it adds is reached by this loop,
    // and one it removes has already moved the place on.
    if (announcing_) return;
    announcing_ = true;
    while (announce_ != entries_.end() && !announce_->waiting()) {
        const It at = announce_;
        if (hooks_.announce) hooks_.announce(*at, passesFilters(*at));
        if (announce_ == at) ++announce_;
    }
    announcing_ = false;
}

void Log::answer(const NameKey& key, const std::string& name, const std::string& realm) {
    for (It it = entries_.begin(); it != entries_.end(); ++it) {
        bool touched = false;
        for (Party* p : {&it->src, &it->dst}) {
            if (p->waiting && *p->waiting == key) {
                p->name = name;
                p->realm = realm;
                p->waiting.reset();
                touched = true;
            }
        }
        if (it->itemWaiting && *it->itemWaiting == key) {
            it->itemName = name;
            it->itemWaiting.reset();
            touched = true;
        }
        if (touched && !it->waiting()) finalize(*it);
    }
    if (announce_ != entries_.end() && !announce_->waiting()) announcePending();
}

bool Log::passesFilters(const Entry& entry) const {
    if (filters_.empty()) return true;
    for (const Filter& f : filters_) {
        if (f.matches(entry)) return true;
    }
    return false;
}

int Log::numEntries(bool ignoreFilter) const {
    int n = 0;
    for (const Entry& e : entries_) {
        if (e.waiting()) break;
        if (ignoreFilter || passesFilters(e)) ++n;
    }
    return n;
}

bool Log::setCurrent(int index, bool ignoreFilter) {
    current_ = entries_.end();
    if (index >= 1) {
        // From the oldest, among those announced.
        for (It it = entries_.begin(); it != entries_.end() && it != announce_; ++it) {
            if (!ignoreFilter && !passesFilters(*it)) continue;
            if (--index == 0) {
                current_ = it;
                break;
            }
        }
    } else {
        // From the newest announced, back: 0 is that one.
        It it = announce_;
        while (it != entries_.begin()) {
            --it;
            if (!ignoreFilter && !passesFilters(*it)) continue;
            if (index == 0) {
                current_ = it;
                break;
            }
            ++index;
        }
    }
    return current_ != entries_.end();
}

bool Log::advance(int count, bool ignoreFilter) {
    if (count == 0) return current_ != entries_.end();
    It found = entries_.end();
    It it = current_;
    if (it != entries_.end()) {
        if (count < 0) {
            while (true) {
                if (ignoreFilter || passesFilters(*it)) {
                    if (count == 0) {
                        found = it;
                        break;
                    }
                    ++count;
                }
                if (it == entries_.begin()) break;
                --it;
            }
        } else {
            while (it != announce_) {
                if (ignoreFilter || passesFilters(*it)) {
                    if (count == 0) {
                        found = it;
                        break;
                    }
                    --count;
                }
                ++it;
            }
        }
    }
    current_ = found;
    return current_ != entries_.end();
}

const Entry* Log::current() const {
    return current_ != entries_.end() ? &*current_ : nullptr;
}

void Log::clearEntries() {
    while (!entries_.empty()) remove(entries_.begin());
}

}  // namespace wowee::game::combat_log
