#include "doctest/doctest.h"
#include "game/item.h"

TEST_CASE("armorTierFromMaterial maps material class to weight tier") {
    CHECK(armorTierFromMaterial("armor_plate")    == ArmorTier::HEAVY);
    CHECK(armorTierFromMaterial("helmet_plate")   == ArmorTier::HEAVY);
    CHECK(armorTierFromMaterial("boots_leather")  == ArmorTier::MEDIUM);
    CHECK(armorTierFromMaterial("gloves_leather") == ArmorTier::MEDIUM);
    CHECK(armorTierFromMaterial("armor_cloth")    == ArmorTier::LIGHT);
    CHECK(armorTierFromMaterial("Cloth_Robe")     == ArmorTier::LIGHT);
    CHECK(armorTierFromMaterial("legendary_armor")== ArmorTier::MEDIUM);
    CHECK(armorTierFromMaterial("")               == ArmorTier::MEDIUM);
    CHECK(armorTierFromMaterial(nullptr)          == ArmorTier::MEDIUM); // null-safe
}
