#pragma once

#include <cstdint>

namespace Apex::Core {

// Stamina-potion discount settings: a drunk Restore Stamina potion grants a
// timed percentage reduction of Apex-owned stamina costs (attack admissions,
// bashes, dodge, bow draw) plus the vanilla sprint drain while active.
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

// Pure tier mapping: a restore magnitude below the lowest tier yields no
// discount; anything at or above a tier threshold takes that tier's values.
DiscountTier discountFor(const PotionSettings& settings, float magnitude);

// Cost multiplier for an active discount (1.0 = full price, 0.0 = free).
inline float costMultiplier(float percent) {
    if (percent <= 0.0F) return 1.0F;
    if (percent >= 100.0F) return 0.0F;
    return 1.0F - percent / 100.0F;
}

}  // namespace Apex::Core
