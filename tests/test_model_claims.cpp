// The models tiles on their way count on.
#include <catch_amalgamated.hpp>

#include "rendering/model_claims.hpp"

using wowee::rendering::ModelClaims;

TEST_CASE("a model a tile counts on is not thrown out before the tile is up", "[terrain][model_claims]") {
    auto models = std::make_shared<ModelClaims>();
    models->uploaded(7);
    {
        ModelClaims::Hold tile(models);
        // The renderer has it: the tile does not read it.
        CHECK(tile.take(7));
        CHECK_FALSE(models->letGo(7));
        CHECK(models->isUploaded(7));
        // One it has not, the tile reads; that counts too, once it is there.
        CHECK_FALSE(tile.take(8));
        models->uploaded(8);
        CHECK_FALSE(models->letGo(8));
        // Another's.
        CHECK(models->letGo(9));
    }
    CHECK(models->letGo(7));
    CHECK_FALSE(models->isUploaded(7));
    CHECK(models->letGo(8));
}

TEST_CASE("two tiles count on a model each for itself", "[terrain][model_claims]") {
    auto models = std::make_shared<ModelClaims>();
    models->uploaded(1);
    auto first = std::make_unique<ModelClaims::Hold>(models);
    auto second = std::make_unique<ModelClaims::Hold>(models);
    CHECK(first->take(1));
    CHECK(first->take(1));
    CHECK(second->take(1));
    first.reset();
    CHECK_FALSE(models->letGo(1));
    // Handed on, it counts on.
    ModelClaims::Hold moved = std::move(*second);
    second.reset();
    CHECK_FALSE(models->letGo(1));
    moved = ModelClaims::Hold(models);
    CHECK(models->letGo(1));
    // Thrown out before: the tile reads it.
    ModelClaims::Hold after(models);
    CHECK_FALSE(after.take(1));
    // All forgotten with the map: what counts still counts.
    models->uploaded(2);
    CHECK(after.take(2));
    models->clear();
    CHECK_FALSE(models->isUploaded(2));
    CHECK_FALSE(models->letGo(2));
}
