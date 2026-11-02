#pragma once

#include <cmath>

#include <cstdint>

namespace Apex::Core {

// Stamina-potion discount settings: a drunk Restore Stamina potion grants a
// timed percentage reduction of Apex-owned stamina costs and sprint drain.
// Power attacks receive the same modifier through their native cost calculation.
// Bashes retain their native costs.
struct PotionSettings {
    // Master switch for the potion discount watcher.
    bool enabled{true};
    // Discount percent per vanilla potion tier. Tiers are matched by the
    // potion's restore magnitude against the vanilla thresholds
    // (25/50/75/100/150); modded potions floor to the nearest lower tier.
    float discountMinor{10.0F};
    float discountStandard{20.0F};
    float discountPlentiful{30.0F};
    float discountVigorous{50.0F};
    float discountExtreme{75.0F};
    // The "ultimate" tier (vanilla magnitude 9999) reduces costs to zero.
    float discountUltimate{100.0F};
    // Buff durations. The ultimate tier gets its own, shorter window.
    std::uint32_t standardSeconds{60};
    std::uint32_t ultimateSeconds{20};
};

// Vanilla restore magnitudes for the built-in potion tiers (Skyrim.esm
// RestoreStamina01..06): 25/50/75/100/150, and 9999 for the 100% ultimate.
// These are data facts, not tuning, so they are not INI keys.
inline constexpr float kTierMinor = 25.0F;
inline constexpr float kTierStandard = 50.0F;
inline constexpr float kTierPlentiful = 75.0F;
inline constexpr float kTierVigorous = 100.0F;
inline constexpr float kTierExtreme = 150.0F;
inline constexpr float kTierUltimate = 9999.0F;

struct DiscountTier {
    // Discount percent (0..100), or 0 when the magnitude matches no tier.
    float percent{0.0F};
    // Buff duration in seconds; 0 when percent is 0.
    std::uint32_t seconds{0};
};

// Editor-ID contract with the generated data ESP ("ApexCombat - Potions.esp"):
// the generator stamps each tiered potion with one keyword whose editor ID is
// kPotionKeywordPrefix followed by the tier's name below; the runtime resolves
// those editor IDs once at startup and polls HasKeyword when a potion is drunk.
inline constexpr const char* kPotionKeywordPrefix = "ApexPotionStamina";
inline constexpr const char* kTierNames[6] = {
    "Minor", "Standard", "Plentiful", "Vigorous", "Extreme", "Ultimate"};

// Potion tier index used by the keyword-stamped data ESP
// ("ApexCombat - Potions.esp"): each tier keyword corresponds to one index.
inline constexpr int kTierNone = -1;
inline constexpr int kTierIndexMinor = 0;      // magnitude >= 25
inline constexpr int kTierIndexStandard = 1;   // magnitude >= 50
inline constexpr int kTierIndexPlentiful = 2;  // magnitude >= 75
inline constexpr int kTierIndexVigorous = 3;   // magnitude >= 100
inline constexpr int kTierIndexExtreme = 4;    // magnitude >= 150
inline constexpr int kTierIndexUltimate = 5;   // magnitude >= 9999

// Pure tier mapping: a restore magnitude below the lowest tier yields no
// discount; anything at or above a tier threshold takes that tier's values.
DiscountTier discountFor(const PotionSettings& settings, float magnitude);

// Same mapping by tier index (kTierNone = no discount). This is the runtime
// entry point when the tier comes from a keyword on the potion rather than
// from a scanned magnitude.
DiscountTier discountForTier(const PotionSettings& settings, int tier);

// Tier index for a restore magnitude; kTierNone when below the lowest tier.
// Shared by the ESP generator's contract tests and the runtime's fallback.
int tierIndexForMagnitude(float magnitude);

// Cost multiplier for an active discount (1.0 = full price, 0.0 = free).
inline float costMultiplier(float percent) {
    if (!std::isfinite(percent) || percent <= 0.0F) return 1.0F;
    if (percent >= 100.0F) return 0.0F;
    return 1.0F - percent / 100.0F;
}

}  // namespace Apex::Core
