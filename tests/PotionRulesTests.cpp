#include "core/PotionRules.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <source_location>

using namespace Apex::Core;

namespace {
int checks = 0;
void check(bool condition, const std::source_location location = std::source_location::current())
{
    ++checks;
    if (!condition) {
        std::cerr << location.file_name() << ':' << location.line() << ": check failed\n";
        std::exit(EXIT_FAILURE);
    }
}

bool near(float a, float b)
{
    return a > b - 0.001F && a < b + 0.001F;
}

void vanillaTiersMap()
{
    const PotionSettings settings;
    const auto minor = discountFor(settings, 25.0F);
    check(near(minor.percent, 10.0F) && minor.seconds == 60);
    check(near(discountFor(settings, 50.0F).percent, 20.0F));
    check(near(discountFor(settings, 75.0F).percent, 30.0F));
    check(near(discountFor(settings, 100.0F).percent, 50.0F));
    const auto extreme = discountFor(settings, 150.0F);
    check(near(extreme.percent, 75.0F) && extreme.seconds == 60);
    const auto ultimate = discountFor(settings, 9999.0F);
    check(near(ultimate.percent, 100.0F) && ultimate.seconds == 20);
}

void moddedPotionsFloorToNearestTier()
{
    const PotionSettings settings;
    // Between tiers: floor to the lower tier.
    check(near(discountFor(settings, 30.0F).percent, 10.0F));
    check(near(discountFor(settings, 74.9F).percent, 20.0F));
    check(near(discountFor(settings, 149.0F).percent, 50.0F));
    // A player-brewed potion beyond extreme but short of ultimate: extreme.
    const auto brewed = discountFor(settings, 400.0F);
    check(near(brewed.percent, 75.0F) && brewed.seconds == 60);
    // Below the lowest tier: nothing.
    check(near(discountFor(settings, 10.0F).percent, 0.0F));
    check(discountFor(settings, 10.0F).seconds == 0);
}

void edgeInputsYieldNothing()
{
    const PotionSettings settings;
    check(near(discountFor(settings, 0.0F).percent, 0.0F));
    check(near(discountFor(settings, -25.0F).percent, 0.0F));
    check(tierIndexForMagnitude(std::numeric_limits<float>::infinity()) == kTierNone);
    check(tierIndexForMagnitude(std::numeric_limits<float>::quiet_NaN()) == kTierNone);
    check(near(costMultiplier(std::numeric_limits<float>::quiet_NaN()), 1.0F));
    check(near(costMultiplier(std::numeric_limits<float>::infinity()), 1.0F));
    // Disabled watcher yields nothing regardless of magnitude.
    PotionSettings off;
    off.enabled = false;
    check(near(discountFor(off, 9999.0F).percent, 0.0F));
}

void customPercentsAndDurationsFlow()
{
    PotionSettings settings;
    settings.discountMinor = 5.0F;
    settings.discountUltimate = 90.0F;
    settings.standardSeconds = 45;
    settings.ultimateSeconds = 15;
    const auto minor = discountFor(settings, 25.0F);
    check(near(minor.percent, 5.0F) && minor.seconds == 45);
    const auto ultimate = discountFor(settings, 9999.0F);
    check(near(ultimate.percent, 90.0F) && ultimate.seconds == 15);
}

void tierIndexMappingMatchesMagnitudeMapping()
{
    check(tierIndexForMagnitude(0.0F) == kTierNone);
    check(tierIndexForMagnitude(24.9F) == kTierNone);
    check(tierIndexForMagnitude(25.0F) == kTierIndexMinor);
    check(tierIndexForMagnitude(60.0F) == kTierIndexStandard);
    check(tierIndexForMagnitude(9999.0F) == kTierIndexUltimate);
    check(tierIndexForMagnitude(100000.0F) == kTierIndexUltimate);
}

void keywordTierDiscountsMatchMagnitudeDiscounts()
{
    const PotionSettings settings;
    // Every keyword tier must produce the same values the magnitude path
    // produces for that tier's vanilla threshold potion.
    const float thresholds[] = {kTierMinor, kTierStandard, kTierPlentiful,
                               kTierVigorous, kTierExtreme, kTierUltimate};
    const int indices[] = {kTierIndexMinor, kTierIndexStandard, kTierIndexPlentiful,
                           kTierIndexVigorous, kTierIndexExtreme, kTierIndexUltimate};
    for (int i = 0; i < 6; ++i) {
        const auto byMagnitude = discountFor(settings, thresholds[i]);
        const auto byKeyword = discountForTier(settings, indices[i]);
        check(near(byMagnitude.percent, byKeyword.percent));
        check(byMagnitude.seconds == byKeyword.seconds);
    }
    check(near(discountForTier(settings, kTierNone).percent, 0.0F));
    check(near(discountForTier(settings, 99).percent, 0.0F));
    // Disabled watcher yields nothing via the keyword path too.
    PotionSettings off;
    off.enabled = false;
    check(near(discountForTier(off, kTierIndexUltimate).percent, 0.0F));
}

void costMultiplierBounds()
{
    check(near(costMultiplier(0.0F), 1.0F));
    check(near(costMultiplier(25.0F), 0.75F));
    check(near(costMultiplier(100.0F), 0.0F));
    check(near(costMultiplier(-5.0F), 1.0F));
    check(near(costMultiplier(150.0F), 0.0F));
}
}  // namespace

int main()
{
    vanillaTiersMap();
    moddedPotionsFloorToNearestTier();
    edgeInputsYieldNothing();
    customPercentsAndDurationsFlow();
    tierIndexMappingMatchesMagnitudeMapping();
    keywordTierDiscountsMatchMagnitudeDiscounts();
    costMultiplierBounds();
    std::cout << "potion tests passed (" << checks << " checks)\n";
    return 0;
}
