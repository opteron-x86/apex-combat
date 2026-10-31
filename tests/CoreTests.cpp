#include "core/CombatRules.h"

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

void acceptedStagesDebitOnce()
{
    CombatRules rules;
    float stamina = 100;
    auto admission = rules.beginAttack(1, 1, AttackKind::LightOneHanded, stamina, 0);
    check(admission.accepted);
    near(admission.staminaDebit, 0);
    near(admission.nominalCost, 20);
    auto repeated = rules.beginAttack(1, 1, AttackKind::LightTwoHanded, stamina, 1);
    check(repeated.accepted && repeated.duplicate);
    near(repeated.nominalCost, 20);  // Duplicate notification cannot change the stage's price.
    auto committed = rules.commitAttack(1, 1, stamina, 2);
    check(committed.accepted && !committed.engineOwnsCost);
    stamina -= committed.staminaDebit;
    near(stamina, 80);
    auto duplicate = rules.commitAttack(1, 1, stamina, 3);
    check(!duplicate.accepted && duplicate.duplicate);
    near(duplicate.staminaDebit, 0);
    check(!rules.cancelAttack(1, 1, 4));
    check(rules.beginAttack(1, 2, AttackKind::LightTwoHanded, stamina, 5).accepted);
    auto oldHit = rules.commitAttack(1, 1, stamina, 6);
    check(!oldHit.accepted);
    auto second = rules.commitAttack(1, 2, stamina, 7);
    check(second.accepted);
    stamina -= second.staminaDebit;
    near(stamina, 47);
    check(rules.beginAttack(1, 3, AttackKind::Unarmed, stamina, 8).accepted);
    stamina -= rules.commitAttack(1, 3, stamina, 9).staminaDebit;
    near(stamina, 37);
}

void positiveStaminaAndParryRecovery()
{
    CombatRules rules;
    check(rules.beginAttack(1, 1, AttackKind::LightOneHanded, 1, 0).accepted);
    const auto attack = rules.commitAttack(1, 1, 1, 1);
    check(attack.accepted && attack.exhausted);
    near(attack.staminaDebit, 1);
    check(rules.isExhausted(1));
    const auto empty = rules.beginAttack(1, 2, AttackKind::Unarmed, 0, 2);
    check(!empty.accepted && empty.reason == RejectReason::NoStamina);
    check(rules.setBlocking(1, true, 3));  // Empty stamina does not prevent the recovery action.
    const auto parry = rules.successfulBlock(1, 1, 0, 100, 50);
    check(parry.perfect && !parry.exhausted);
    near(parry.staminaRestore, 25);
    check(!rules.isExhausted(1));
    check(rules.beginAttack(1, 3, AttackKind::LightOneHanded, parry.staminaRestore, 51).accepted);
    auto paid = rules.commitAttack(1, 3, parry.staminaRestore, 52);
    near(paid.staminaDebit, 20);
    check(!paid.exhausted);
    const auto entered = rules.observeStamina(1, 0, 53);
    check(entered.exhausted && entered.entered && !entered.recovered);
    const auto stillEmpty = rules.observeStamina(1, -5, 54);
    check(stillEmpty.exhausted && !stillEmpty.entered);
    const auto recovered = rules.observeStamina(1, 0.001F, 55);
    check(!recovered.exhausted && recovered.recovered);
}

void strictAffordabilityRechecksAtCommit()
{
    Settings settings;
    settings.affordability = Affordability::FullCost;
    CombatRules rules(settings);
    auto low = rules.beginAttack(1, 1, AttackKind::LightOneHanded, 19.999F, 0);
    check(!low.accepted && low.reason == RejectReason::InsufficientStamina && !low.exhausted);
    check(rules.beginAttack(1, 2, AttackKind::LightOneHanded, 20, 1).accepted);
    const auto interrupted = rules.commitAttack(1, 2, 19, 2);
    check(!interrupted.accepted && interrupted.reason == RejectReason::InsufficientStamina);
    near(interrupted.staminaDebit, 0);
    check(!rules.commitAttack(1, 2, 40, 3).accepted);
    check(rules.beginAttack(1, 3, AttackKind::LightOneHanded, 20, 4).accepted);
    auto exact = rules.commitAttack(1, 3, 20, 5);
    check(exact.accepted && exact.exhausted);
    near(exact.staminaDebit, 20);
    check(rules.beginAttack(1, 4, AttackKind::LightOneHanded, 20.001F, 6).accepted);
    check(!rules.commitAttack(1, 4, 20.001F, 7).exhausted);
}

void powerCostsHaveOneOwner()
{
    Settings settings;
    settings.affordability = Affordability::FullCost;
    CombatRules rules(settings);
    check(!rules.beginAttack(1, 1, AttackKind::Power, 39, 0, 40).accepted);
    auto start = rules.beginAttack(1, 2, AttackKind::Power, 40, 1, 40);
    check(start.accepted && start.engineOwnsCost);
    auto commit = rules.commitAttack(1, 2, 40, 2);
    check(commit.accepted && commit.engineOwnsCost);
    near(commit.nominalCost, 40);
    near(commit.staminaDebit, 0);
    check(!commit.exhausted);  // Actual engine debit must be observed, not guessed.
    check(rules.observeStamina(1, 0, 3).exhausted);
    settings.chargePowerAttacks = true;
    rules.setSettings(settings);
    check(rules.beginAttack(1, 1, AttackKind::Power, 40, 4, 40).accepted);
    auto owned = rules.commitAttack(1, 1, 40, 5);
    check(owned.accepted && !owned.engineOwnsCost && owned.exhausted);
    near(owned.staminaDebit, 40);
}

void cancelledExpiredAndSupersededStages()
{
    CombatRules rules;
    check(rules.beginAttack(1, 1, AttackKind::Unarmed, 100, 0).accepted);
    check(rules.cancelAttack(1, 1, 1));
    check(!rules.commitAttack(1, 1, 100, 2).accepted);
    check(!rules.beginAttack(1, 1, AttackKind::Unarmed, 100, 3).accepted);
    check(rules.beginAttack(1, 2, AttackKind::Unarmed, 100, 4).accepted);
    check(rules.beginAttack(1, 3, AttackKind::LightOneHanded, 100, 5).accepted);
    check(!rules.commitAttack(1, 2, 100, 6).accepted);
    check(rules.commitAttack(1, 3, 100, 7).accepted);
    check(rules.beginAttack(1, 4, AttackKind::Unarmed, 100, 10).accepted);
    check(rules.beginAttack(1, 4, AttackKind::Unarmed, 100, 1009).accepted);
    auto expired = rules.commitAttack(1, 4, 100, 1011);
    check(!expired.accepted && expired.reason == RejectReason::StageExpired);
    near(expired.staminaDebit, 0);
    check(rules.beginAttack(1, 5, AttackKind::Unarmed, 100, 2000).accepted);
    check(rules.commitAttack(1, 5, 100, 3000).accepted);
    check(rules.commitAttack(1, 6, 100, 3001).reason == RejectReason::MissingStage);
    check(rules.commitAttack(99, 1, 100, 3002).reason == RejectReason::MissingStage);
    check(rules.beginAttack(1, 6, AttackKind::Unarmed, 100, 4000).accepted);
    check(rules.beginAttack(1, 6, AttackKind::Unarmed, 100, 5001).reason == RejectReason::StageExpired);
}

void blockTimingAndSingleConsumption()
{
    CombatRules rules;
    check(!rules.successfulBlock(1, 1, 50, 100, 0).perfect);
    check(rules.setBlocking(1, true, 1000));
    check(!rules.setBlocking(1, true, 1100));  // Holding cannot extend the window.
    const auto boundary = rules.successfulBlock(1, 2, 50, 100, 1150);
    check(boundary.perfect);
    near(boundary.staminaRestore, 25);
    const auto secondHit = rules.successfulBlock(1, 3, 75, 100, 1150);
    check(!secondHit.perfect);
    near(secondHit.staminaRestore, 0);
    check(!rules.setBlocking(1, false, 1200));
    check(rules.setBlocking(1, true, 1201));
    auto replay = rules.successfulBlock(1, 2, 75, 100, 1202);
    check(!replay.perfect && replay.reason == ParryReason::DuplicateToken);
    check(rules.successfulBlock(1, 4, 75, 100, 1203).perfect);
    check(!rules.setBlocking(1, false, 1300));
    check(rules.setBlocking(1, true, 1401));
    auto late = rules.successfulBlock(1, 5, 50, 100, 1552);
    check(!late.perfect && late.reason == ParryReason::NormalBlock);
    near(late.staminaRestore, 0);
    check(!rules.setBlocking(1, true, 1553));
    check(!rules.successfulBlock(1, 6, 50, 100, 1554).perfect);
}

void blockSpamAndRelease()
{
    CombatRules rules;
    check(rules.setBlocking(1, true, 0));
    check(!rules.setBlocking(1, false, 1));
    check(!rules.setBlocking(1, true, 100));
    check(!rules.successfulBlock(1, 1, 0, 100, 110).perfect);
    check(!rules.setBlocking(1, false, 120));
    check(!rules.setBlocking(1, true, 200));  // Starts are measured from the previous start.
    check(!rules.setBlocking(1, false, 210));
    check(rules.setBlocking(1, true, 400));
    check(!rules.setBlocking(1, false, 410));
    check(!rules.successfulBlock(1, 2, 0, 100, 420).perfect);
    Settings settings;
    settings.parryRearmDelayMs = 0;
    rules.setSettings(settings);
    check(rules.setBlocking(1, true, 0));
    check(!rules.setBlocking(1, false, 0));
    check(rules.setBlocking(1, true, 0));
    check(rules.successfulBlock(1, 1, 0, 100, 0).perfect);
}

void restorationBoundsAndActorIsolation()
{
    CombatRules rules;
    check(rules.setBlocking(1, true, 0));
    auto nearFull = rules.successfulBlock(1, 1, 90, 100, 1);
    check(nearFull.perfect);
    near(nearFull.staminaRestore, 10);
    check(rules.setBlocking(2, true, 0));
    auto full = rules.successfulBlock(2, 1, 100, 100, 1);
    check(full.perfect);
    near(full.staminaRestore, 0);
    check(!rules.successfulBlock(3, 1, 0, 100, 1).perfect);
    check(rules.beginAttack(1, 1, AttackKind::Unarmed, 10, 2).accepted);
    check(rules.beginAttack(2, 1, AttackKind::Unarmed, 10, 2).accepted);
    check(rules.commitAttack(1, 1, 10, 3).accepted);
    check(rules.commitAttack(2, 1, 10, 3).accepted);
    check(rules.setBlocking(4, true, 0));
    auto negative = rules.successfulBlock(4, 1, -30, 100, 1);
    near(negative.staminaRestore, 25);
    check(negative.exhausted);
    check(rules.setBlocking(5, true, 0));
    near(rules.successfulBlock(5, 1, 120, 100, 1).staminaRestore, 0);
}

void clockRegressionAndReset()
{
    CombatRules rules;
    check(rules.beginAttack(1, 1, AttackKind::Unarmed, 100, 1000).accepted);
    check(rules.setBlocking(1, true, 1001));
    const auto rewind = rules.commitAttack(1, 1, 100, 900);
    check(!rewind.accepted && rewind.reason == RejectReason::ClockRegressed);
    check(!rules.successfulBlock(1, 1, 0, 100, 901).perfect);
    check(!rules.beginAttack(1, 1, AttackKind::Unarmed, 100, 902).accepted);
    check(rules.beginAttack(1, 2, AttackKind::Unarmed, 100, 903).accepted);
    check(rules.commitAttack(1, 2, 100, 904).accepted);
    check(!rules.setBlocking(1, false, 905));
    check(rules.setBlocking(1, true, 1100));
    auto rewindHit = rules.successfulBlock(1, 2, 0, 100, 1000);
    check(rewindHit.reason == ParryReason::ClockRegressed && !rewindHit.perfect);
    check(!rules.successfulBlock(1, 2, 0, 100, 1001).perfect);
    rules.resetActor(1);
    check(rules.actorCount() == 0);
    check(rules.beginAttack(1, 1, AttackKind::Unarmed, 100, 0).accepted);
    check(rules.setBlocking(2, true, 0));
    rules.resetSession();
    check(rules.actorCount() == 0 && !rules.isExhausted(1));
    check(rules.commitAttack(1, 1, 100, 0).reason == RejectReason::MissingStage);
    // Very large intervals expire safely instead of overflowing signed arithmetic.
    check(rules.setBlocking(1, true, std::numeric_limits<TimeMs>::min()));
    check(!rules.successfulBlock(1, 1, 0, 100, std::numeric_limits<TimeMs>::max()).perfect);
}

void perWeaponCostsAndDualWield()
{
    CombatRules rules;
    auto dagger = rules.beginAttack(1, 1, AttackKind::Dagger, 100, 0);
    check(dagger.accepted);
    near(dagger.nominalCost, 10);
    near(rules.commitAttack(1, 1, 100, 1).staminaDebit, 10);
    auto axe = rules.beginAttack(1, 2, AttackKind::OneHandedAxe, 90, 2);
    near(axe.nominalCost, 25);
    auto mace = rules.beginAttack(1, 3, AttackKind::OneHandedMace, 90, 3);
    near(mace.nominalCost, 25);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    // Dual-wield costs arrive from the runtime as an averaged per-hand estimate.
    auto dual = rules.beginAttack(1, 4, AttackKind::DualWield, 90, 4, 0.0F, 32.5F);
    check(dual.accepted);
    near(dual.nominalCost, 32.5F);
    near(rules.commitAttack(1, 4, 90, 5).staminaDebit, 32.5F);
    check(rules.beginAttack(1, 5, AttackKind::DualWield, 60, 6, 0.0F, 0).reason == RejectReason::InvalidInput);
    check(rules.beginAttack(1, 5, AttackKind::DualWield, 60, 6, 0.0F, nan).reason == RejectReason::InvalidInput);
    check(rules.beginAttack(1, 5, AttackKind::DualWield, 60, 6, 0.0F, inf).reason == RejectReason::InvalidInput);
    check(rules.beginAttack(1, 5, AttackKind::DualWield, 60, 6, 0.0F, -5).reason == RejectReason::InvalidInput);
    check(rules.actorCount() == 1);

    Settings lowMultiplier;
    lowMultiplier.dualWieldMultiplier = 0.5F;
    bool rejected = false;
    try { CombatRules invalid(lowMultiplier); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected);
    Settings settings;
    settings.dualWieldMultiplier = 2.0F;
    settings.daggerCost = 15;
    settings.axeCost = 30;
    settings.maceCost = 45;
    CombatRules tuned(settings);
    near(tuned.beginAttack(2, 1, AttackKind::Dagger, 100, 0).nominalCost, 15);
    near(tuned.beginAttack(2, 2, AttackKind::OneHandedAxe, 100, 1).nominalCost, 30);
    near(tuned.beginAttack(2, 3, AttackKind::OneHandedMace, 100, 2).nominalCost, 45);
}

void invalidInputsCannotSpendOrRecover()
{
    CombatRules rules;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    check(rules.beginAttack(0, 1, AttackKind::Unarmed, 100, 0).reason == RejectReason::InvalidInput);
    check(rules.beginAttack(1, 0, AttackKind::Unarmed, 100, 0).reason == RejectReason::InvalidInput);
    check(rules.beginAttack(1, 1, AttackKind::Unarmed, nan, 0).reason == RejectReason::InvalidInput);
    check(rules.beginAttack(1, 1, AttackKind::Power, 100, 0, inf).reason == RejectReason::InvalidInput);
    check(rules.beginAttack(1, 1, static_cast<AttackKind>(999), 100, 0).reason == RejectReason::InvalidInput);
    check(rules.actorCount() == 0);
    check(rules.observeStamina(1, 0, 0).exhausted);
    check(rules.observeStamina(1, nan, 1).exhausted);
    check(rules.setBlocking(1, true, 2));
    check(rules.successfulBlock(1, 1, 0, 0, 3).reason == ParryReason::InvalidInput);
    check(rules.successfulBlock(1, 1, nan, 100, 3).reason == ParryReason::InvalidInput);
    check(rules.successfulBlock(1, 1, 0, 100, 3).perfect);
    Settings bad;
    bad.lightOneHandedCost = -1;
    bool rejected = false;
    try { rules.setSettings(bad); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected && rules.actorCount() == 1);
    near(rules.settings().lightOneHandedCost, 20);
    bad = {};
    bad.parryRearmDelayMs = -1;
    rejected = false;
    try { CombatRules invalid(bad); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected);
}
}  // namespace

int main()
{
    acceptedStagesDebitOnce();
    positiveStaminaAndParryRecovery();
    strictAffordabilityRechecksAtCommit();
    powerCostsHaveOneOwner();
    cancelledExpiredAndSupersededStages();
    blockTimingAndSingleConsumption();
    blockSpamAndRelease();
    restorationBoundsAndActorIsolation();
    clockRegressionAndReset();
    perWeaponCostsAndDualWield();
    invalidInputsCannotSpendOrRecover();
    std::cout << "Combat rules: " << checks << " checks passed\n";
}
