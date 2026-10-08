// How the character component layers worn items' textures on the body, and
// a guild tabard's emblem (0x004f2880, 0x009f6a00, 0x004ec1c0, 0x004e8f00).
#include <catch_amalgamated.hpp>

#include "core/character_component.hpp"

using namespace wowee::core;
using wowee::game::GuildEmblem;

TEST_CASE("equipment slots and inventory types name the component's items (0x004f2880)", "[component]") {
    CHECK(componentItemIndex(3) == 2);   // shirt
    CHECK(componentItemIndex(4) == 3);   // chest
    CHECK(componentItemIndex(9) == 8);   // hands
    CHECK(componentItemIndex(18) == 9);  // tabard
    CHECK(componentItemIndex(0) == -1);  // the head is a model
    CHECK(componentItemIndex(14) == -1); // so is the cloak
    CHECK(componentItemForInventoryType(20) == 3);  // a robe is the chest
    CHECK(componentItemForInventoryType(19) == 9);
    CHECK(componentItemForInventoryType(16) == -1);
}

TEST_CASE("each item has its layer in a region, or none (0x009f6a00)", "[component]") {
    // The chest over the legs on the thigh, the belt over both.
    CHECK(componentTextureLayer(5, 5) == 0);
    CHECK(componentTextureLayer(3, 5) == 1);
    CHECK(componentTextureLayer(4, 5) == 2);
    // The belt over the tabard on the lower torso.
    CHECK(componentTextureLayer(9, 4) == 4);
    CHECK(componentTextureLayer(4, 4) == 5);
    // A shirt paints no legs, a chest no hands or feet.
    CHECK(componentTextureLayer(2, 5) == -1);
    CHECK(componentTextureLayer(3, 2) == -1);
    CHECK(componentTextureLayer(3, 7) == -1);
}

TEST_CASE("a guild tabard carries the guild's design (0x004ec1c0)", "[component]") {
    const GuildEmblem e{.emblemStyle = 12, .emblemColor = 3, .borderStyle = 1, .borderColor = 7,
                        .backgroundColor = 40};
    const auto t = guildTabardTextures(e);
    REQUIRE(t.size() == 6);
    CHECK(t[0].region == 3);
    CHECK(t[0].layer == 2);
    CHECK(t[0].path == "Textures\\GuildEmblems\\Background_40_TU_U.blp");
    CHECK(t[1].layer == 3);
    CHECK(t[1].path == "Textures\\GuildEmblems\\Border_01_07_TU_U.blp");
    CHECK(t[2].layer == 4);
    CHECK(t[2].path == "Textures\\GuildEmblems\\Emblem_12_03_TU_U.blp");
    CHECK(t[5].region == 4);
    CHECK(t[5].path == "Textures\\GuildEmblems\\Emblem_12_03_TL_U.blp");
    // Nothing until the design is known (0x007eada0).
    CHECK(guildTabardTextures(GuildEmblem{}).empty());
}

TEST_CASE("one texture a layer, the last set, drawn layer by layer (0x004e8f00)", "[component]") {
    std::vector<ComponentTexture> textures = {
        {4, 5, "belt"}, {4, 4, "tabard"}, {4, 1, "chest"}, {3, 4, "tabard-upper"},
    };
    const GuildEmblem e{.emblemStyle = 1, .emblemColor = 1, .borderStyle = 1, .borderColor = 1,
                        .backgroundColor = 1};
    for (auto& t : guildTabardTextures(e)) textures.push_back(t);
    const auto layers = componentRegionLayers(textures);
    REQUIRE(layers.size() == 8);
    // Upper torso: background, border, the emblem in the tabard's place.
    CHECK(layers[0].first == 3);
    CHECK(layers[0].second.find("Background_01_TU") != std::string::npos);
    CHECK(layers[1].second.find("Border_01_01_TU") != std::string::npos);
    CHECK(layers[2].second.find("Emblem_01_01_TU") != std::string::npos);
    // Lower torso: chest, background, border, emblem, then the belt on top.
    CHECK(layers[3].second == "chest");
    CHECK(layers[6].second.find("Emblem_01_01_TL") != std::string::npos);
    CHECK(layers[7].second == "belt");
}

TEST_CASE("an NPC's display slots name the component's items as a player's do", "[component]") {
    // Helm, shoulders: models.
    CHECK(componentItemForNpcSlot(0) == -1);
    CHECK(componentItemForNpcSlot(1) == -1);
    // Shirt to tabard: the same items the equipment slots name.
    CHECK(componentItemForNpcSlot(2) == componentItemIndex(3));
    CHECK(componentItemForNpcSlot(3) == componentItemIndex(4));
    CHECK(componentItemForNpcSlot(6) == componentItemIndex(7));
    CHECK(componentItemForNpcSlot(9) == componentItemIndex(18));
    // Cape: a model.
    CHECK(componentItemForNpcSlot(10) == -1);
}

TEST_CASE("a tabard is painted again on a new guild or a new design", "[component]") {
    using wowee::game::tabardNeedsRepaint;
    const GuildEmblem a{.emblemStyle = 1, .emblemColor = 2, .borderStyle = 3, .borderColor = 4, .backgroundColor = 5};
    GuildEmblem b = a;
    b.emblemColor = 7;
    // Joining, leaving or changing guild (0x006e1bb0).
    CHECK(tabardNeedsRepaint(0, std::nullopt, 12, std::nullopt));
    CHECK(tabardNeedsRepaint(12, a, 0, std::nullopt));
    CHECK(tabardNeedsRepaint(12, a, 13, a));
    // The design arriving, or changing (0x006d2840, 0x006e1b40).
    CHECK(tabardNeedsRepaint(12, std::nullopt, 12, a));
    CHECK(tabardNeedsRepaint(12, a, 12, b));
    // Nothing new: the same design, or none known yet.
    CHECK_FALSE(tabardNeedsRepaint(12, a, 12, a));
    CHECK_FALSE(tabardNeedsRepaint(12, a, 12, std::nullopt));
    CHECK_FALSE(tabardNeedsRepaint(0, std::nullopt, 0, std::nullopt));
}
