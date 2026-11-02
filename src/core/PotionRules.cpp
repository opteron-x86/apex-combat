#include "PotionRules.h"

namespace Apex::Core {

int tierIndexForMagnitude(float magnitude) {
    if (!std::isfinite(magnitude) || magnitude <= 0.0F) return kTierNone;
    if (magnitude >= kTierUltimate) return kTierIndexUltimate;
    if (magnitude >= kTierExtreme) return kTierIndexExtreme;
    if (magnitude >= kTierVigorous) return kTierIndexVigorous;
    if (magnitude >= kTierPlentiful) return kTierIndexPlentiful;
    if (magnitude >= kTierStandard) return kTierIndexStandard;
    if (magnitude >= kTierMinor) return kTierIndexMinor;
    return kTierNone;
}

DiscountTier discountForTier(const PotionSettings& settings, int tier) {
    if (!settings.enabled || tier == kTierNone) return {};
    switch (tier) {
        case kTierIndexMinor: return {settings.discountMinor, settings.standardSeconds};
        case kTierIndexStandard: return {settings.discountStandard, settings.standardSeconds};
        case kTierIndexPlentiful: return {settings.discountPlentiful, settings.standardSeconds};
        case kTierIndexVigorous: return {settings.discountVigorous, settings.standardSeconds};
        case kTierIndexExtreme: return {settings.discountExtreme, settings.standardSeconds};
        case kTierIndexUltimate: return {settings.discountUltimate, settings.ultimateSeconds};
        default: return {};
    }
}

DiscountTier discountFor(const PotionSettings& settings, float magnitude) {
    if (!settings.enabled || magnitude <= 0.0F) return {};
    return discountForTier(settings, tierIndexForMagnitude(magnitude));
}

}  // namespace Apex::Core
