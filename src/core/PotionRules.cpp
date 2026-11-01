#include "PotionRules.h"

namespace Apex::Core {

DiscountTier discountFor(const PotionSettings& settings, float magnitude) {
    if (!settings.enabled || magnitude <= 0.0F) return {};
    if (magnitude >= kTierUltimate) return {settings.discountUltimate, settings.ultimateSeconds};
    if (magnitude >= kTierExtreme) return {settings.discountExtreme, settings.standardSeconds};
    if (magnitude >= kTierVigorous) return {settings.discountVigorous, settings.standardSeconds};
    if (magnitude >= kTierPlentiful) return {settings.discountPlentiful, settings.standardSeconds};
    if (magnitude >= kTierStandard) return {settings.discountStandard, settings.standardSeconds};
    if (magnitude >= kTierMinor) return {settings.discountMinor, settings.standardSeconds};
    return {};
}

}  // namespace Apex::Core
