#include "core/PoiseRules.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <source_location>
#include <stdexcept>

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
void near(float actual, float expected,
          const std::source_location location = std::source_location::current())
{
    check(std::fabs(actual - expected) < 0.0001F, location);
}

void damageBreakAndChainGuard()
{
    PoiseRules rules;
    // Sword 25, max 100: four hits break on the fourth.
    check(rules.applyDamage(1, PoiseKind::Sword, false, PoiseSource::Hit, 0).applied);
    near(rules.poise(1, 0), 75);
    check(rules.applyDamage(1, PoiseKind::Sword, false, PoiseSource::Hit, 1).applied);
    check(rules.applyDamage(1, PoiseKind::Sword, false, PoiseSource::Hit, 2).applied);
    const auto breaking = rules.applyDamage(1, PoiseKind::Sword, false, PoiseSource::Hit, 3);
    check(breaking.applied && breaking.broke);
    near(breaking.poiseAfter, 0);
    check(breaking.staggeredUntil == 1503);
    check(rules.isStaggered(1, 3));
    // StaggerMode semantics: no further poise damage inside the window.
    const auto guarded = rules.applyDamage(1, PoiseKind::Sword, false, PoiseSource::Hit, 100);
    check(!guarded.applied && guarded.reason == PoiseRejectReason::Staggered);
    near(guarded.poiseAfter, 0);
    check(rules.isStaggered(1, 1499));
    // Window expiry refills poise to max lazily.
    near(rules.poise(1, 1503), 100);
    check(!rules.isStaggered(1, 1503));
    const auto after = rules.applyDamage(1, PoiseKind::Sword, false, PoiseSource::Hit, 1503);
    check(after.applied && !after.broke);
    near(after.poiseAfter, 75);
}

void regenAndDamageDelay()
{
    PoiseRules rules;
    check(rules.applyDamage(1, PoiseKind::TwoHanded, false, PoiseSource::Hit, 0).applied);
    near(rules.poise(1, 0), 60);
    near(rules.poise(1, 2999), 60);  // Inside the recovery delay.
    near(rules.poise(1, 3000), 60);
    near(rules.poise(1, 3500), 70);  // 20 per second afterwards.
    near(rules.poise(1, 5000), 100);
    // Regen is capped and does not create a second delay window.
    near(rules.poise(1, 9000), 100);
    // Unseen actors read as full poise and are not registered by poise().
    near(rules.poise(2, 0), 100);
    check(rules.actorCount() == 1);
    // Fresh damage restarts the delay from the damage time.
    check(rules.applyDamage(1, PoiseKind::Sword, false, PoiseSource::Hit, 9500).applied);
    near(rules.poise(1, 12499), 75);
    near(rules.poise(1, 13000), 85);  // 20 per second, not yet capped.
}

void multipliersAndParry()
{
    PoiseRules rules;
    // Power attacks use the multiplier.
    const auto power = rules.applyDamage(1, PoiseKind::Dagger, true, PoiseSource::Hit, 0);
    check(power.applied);
    near(power.poiseAfter, 70);  // 20 * 1.5
    // Blocked contacts pass the blocked fraction.
    const auto blocked = rules.applyDamage(1, PoiseKind::Mace, false, PoiseSource::BlockedHit, 1);
    check(blocked.applied);
    near(blocked.poiseAfter, 52.5F);  // 35 * 0.5
    // Perfect parry deals the flat configured amount, ignoring kind.
    const auto parry = rules.applyDamage(2, PoiseKind::Unarmed, false, PoiseSource::PerfectParry, 2);
    check(parry.applied);
    near(parry.poiseAfter, 50);
    // Zero-damage configuration cannot break an actor.
    PoiseSettings zero;
    zero.maxPoise = 50;
    zero.swordDamage = 0;
    PoiseRules harmless(zero);
    const auto nothing = harmless.applyDamage(3, PoiseKind::Sword, false, PoiseSource::Hit, 0);
    check(nothing.applied && !nothing.broke);
    near(nothing.poiseAfter, 50);
}

// The counterattack bonus multiplies hit and blocked contacts, stacks with the
// power multiplier, and is ignored by the flat perfect-parry counter-damage.
void counterattackPoiseBonus()
{
    PoiseRules rules;
    // Sword light hit with the window bonus: 25 * 1.25.
    const auto bonus = rules.applyDamage(1, PoiseKind::Sword, false, PoiseSource::Hit, 0, 1.25F);
    check(bonus.applied);
    near(bonus.poiseAfter, 68.75F);
    // Stacks with the power multiplier: 20 * 1.5 * 1.25 = 37.5.
    const auto power = rules.applyDamage(1, PoiseKind::Dagger, true, PoiseSource::Hit, 1, 1.25F);
    near(power.poiseAfter, 31.25F);  // 100 - 68.75
    // Blocked contacts take the bonus before the block reduction: 35 * 1.25 * 0.5.
    const auto blocked = rules.applyDamage(2, PoiseKind::Mace, false, PoiseSource::BlockedHit, 0, 1.25F);
    near(blocked.poiseAfter, 78.125F);
    // The flat perfect-parry counter-damage ignores the multiplier.
    const auto parry = rules.applyDamage(3, PoiseKind::Unarmed, false, PoiseSource::PerfectParry, 0, 1.25F);
    near(parry.poiseAfter, 50);
    // Multiplier 1.0 reproduces the plain damage.
    PoiseRules plain;
    const auto neutral = plain.applyDamage(1, PoiseKind::Sword, false, PoiseSource::Hit, 0, 1.0F);
    near(neutral.poiseAfter, 75);
    // Non-finite and negative multipliers are rejected as invalid input.
    const auto nan = plain.applyDamage(2, PoiseKind::Sword, false, PoiseSource::Hit, 0,
                                       std::numeric_limits<float>::quiet_NaN());
    check(!nan.applied && nan.reason == PoiseRejectReason::InvalidInput);
    const auto negative = plain.applyDamage(2, PoiseKind::Sword, false, PoiseSource::Hit, 1, -1.0F);
    check(!negative.applied && negative.reason == PoiseRejectReason::InvalidInput);
    // Settings validation rejects bonus multipliers below 1.
    PoiseSettings weak;
    weak.counterattackPoiseMultiplier = 0.5F;
    bool rejected = false;
    try { PoiseRules invalid(weak); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected);
}

void validationAndResets()
{
    PoiseSettings bad;
    bad.maxPoise = 0;
    bool threw = false;
    try {
        PoiseRules invalid(bad);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw);
    PoiseSettings nan;
    nan.powerMultiplier = std::nanf("");
    threw = false;
    try {
        PoiseRules invalidNan(nan);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw);
    PoiseRules rules;
    rules.applyDamage(1, PoiseKind::Axe, false, PoiseSource::Hit, 0);
    check(rules.actorCount() == 1);
    rules.resetActor(1);
    check(rules.actorCount() == 0);
    near(rules.poise(1, 1), 100);
    rules.applyDamage(2, PoiseKind::Axe, false, PoiseSource::Hit, 0);
    rules.resetSession();
    check(rules.actorCount() == 0);
    // Invalid input is rejected, not applied.
    const auto zeroActor = rules.applyDamage(0, PoiseKind::Sword, false, PoiseSource::Hit, 0);
    check(!zeroActor.applied && zeroActor.reason == PoiseRejectReason::InvalidInput);
    check(rules.actorCount() == 0);
    const auto stale = rules.applyDamage(1, PoiseKind::Sword, false, PoiseSource::Hit, -1);
    check(!stale.applied && stale.reason == PoiseRejectReason::InvalidInput);
}

void replacementSettingsClearState()
{
    PoiseRules rules;
    rules.applyDamage(1, PoiseKind::Sword, false, PoiseSource::Hit, 0);
    PoiseSettings tuned;
    tuned.maxPoise = 40;
    tuned.swordDamage = 40;
    rules.setSettings(tuned);
    check(rules.actorCount() == 0);
    const auto result = rules.applyDamage(1, PoiseKind::Sword, false, PoiseSource::Hit, 1);
    check(result.broke);
    near(result.poiseAfter, 0);
    check(rules.settings().maxPoise == 40);
}
}  // namespace

int main()
{
    damageBreakAndChainGuard();
    regenAndDamageDelay();
    multipliersAndParry();
    counterattackPoiseBonus();
    validationAndResets();
    replacementSettingsClearState();
    std::cout << "poise tests: " << checks << " checks passed\n";
    return EXIT_SUCCESS;
}
