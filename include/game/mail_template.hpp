#pragma once

#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace wowee::game {

/// What MailTemplate.dbc has a letter say.
struct MailTemplateText {
    std::string subject;
    std::string body;
};

/// A letter's subject (0x00571c50): its template's, whatever was sent with it,
/// and what was sent where it has none or one without a row.
inline const std::string& mailSubject(const std::string& sent, const MailTemplateText* text) {
    return text ? text->subject : sent;
}

/// The row of a template, null for none.
inline const MailTemplateText* mailTemplate(pipeline::AssetManager* assets, uint32_t templateId) {
    static std::unordered_map<uint32_t, MailTemplateText> rows;
    static bool loaded = false;
    if (!loaded && assets && assets->isInitialized()) {
        loaded = true;
        if (auto dbc = assets->loadDBC("MailTemplate.dbc"); dbc && dbc->isLoaded() && dbc->getFieldCount() >= 19)
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i)
                rows[dbc->getUInt32(i, 0)] = {dbc->getString(i, 1), dbc->getString(i, 18)};
    }
    const auto it = rows.find(templateId);
    return templateId != 0 && it != rows.end() ? &it->second : nullptr;
}

}  // namespace wowee::game
