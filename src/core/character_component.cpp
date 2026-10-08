#include "core/character_component.hpp"

#include "pipeline/dbc_layout.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/item_textures.hpp"

namespace wowee::core {

std::vector<std::pair<int, std::string>> characterComponentLayers(pipeline::AssetManager& assets,
                                                                  const pipeline::DBCFile& itemDisplayInfo,
                                                                  const std::vector<ComponentItem>& items,
                                                                  bool female,
                                                                  const std::optional<game::GuildEmblem>& emblem) {
    const auto* layouts = pipeline::getActiveDBCLayout();
    const auto* layout = layouts ? layouts->getLayout("ItemDisplayInfo") : nullptr;
    uint32_t regionFields[8];
    pipeline::getItemDisplayInfoTextureFields(itemDisplayInfo, layout, regionFields);
    const uint32_t flagsField = layout ? layout->tryField("Flags") : 0xFFFFFFFFu;

    std::vector<ComponentTexture> textures;
    for (const auto& item : items) {
        if (item.componentItem < 0 || item.displayId == 0) continue;
        const int32_t row = itemDisplayInfo.findRecordById(item.displayId);
        if (row < 0) continue;
        const auto r = static_cast<uint32_t>(row);
        // 0x004f2640: each region the display names a texture for, where
        // the table gives the item a layer there.
        for (int region = 0; region < 8; ++region) {
            const int layer = componentTextureLayer(item.componentItem, region);
            if (layer < 0) continue;
            const std::string name = itemDisplayInfo.getString(r, regionFields[region]);
            if (name.empty()) continue;
            std::string path = pipeline::resolveItemRegionTexture(assets, region, name, female);
            if (path.empty()) continue;
            textures.push_back({region, layer, std::move(path)});
        }
        // The guild tabard's emblem, once the guild's design is known.
        if (item.componentItem == 9 && emblem && flagsField < itemDisplayInfo.getFieldCount() &&
            (itemDisplayInfo.getUInt32(r, flagsField) & kItemDisplayFlagGuildTabard) != 0) {
            for (auto& t : guildTabardTextures(*emblem)) textures.push_back(std::move(t));
        }
    }
    return componentRegionLayers(textures);
}

}  // namespace wowee::core
