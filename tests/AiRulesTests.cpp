#include "core/AiRules.h"

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

constexpr ActorId kTarget = 0x100;
constexpr ActorId kA = 0x201;
constexpr ActorId kB = 0x202;
constexpr ActorId kC = 0x203;

bool throwsInvalid(const AiSettings& settings)
{
    AiRules rules;
    try {
        rules.setSettings(settings);
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

void slotRespectsMaxAttackers()
{
    AiRules rules;
    check(rules.tryAcquire(kTarget, kA, 1000, 0.5F).granted);
    check(rules.tryAcquire(kTarget, kB, 1000, 0.5F).granted);
    const auto third = rules.tryAcquire(kTarget, kC, 1000, 0.5F);
    check(!third.granted && third.reason == SlotRejectReason::NoSlot);
    check(rules.holderCount(kTarget, 1000) == 2);
}

void slotsArePerTarget()
{
    AiRules rules;
    check(rules.tryAcquire(kTarget, kA, 0, 0.5F).granted);
    check(rules.tryAcquire(kTarget, kB, 0, 0.5F).granted);
    check(rules.tryAcquire(0x101, kC, 0, 0.5F).granted);
    check(rules.holderCount(0x101, 0) == 1);
}

void slotExpiryIsLazy()
{
    AiRules rules;
    const auto first = rules.tryAcquire(kTarget, kA, 1000, 0.0F);
    check(first.granted);
    check(first.expiresAt == 1000 + rules.settings().slotWindowMinMs);
    check(rules.tryAcquire(kTarget, kB, 1000, 0.0F).granted);
    check(!rules.tryAcquire(kTarget, kC, 1000, 0.0F).granted);

    const TimeMs after = 1000 + rules.settings().slotWindowMinMs + 1;
    check(rules.holderCount(kTarget, after) == 0);
    check(rules.tryAcquire(kTarget, kC, after, 0.0F).granted);
}

void windowSampleInterpolates()
{
    AiRules rules;
    const auto atMin = rules.tryAcquire(kTarget, kA, 0, 0.0F);
    check(atMin.expiresAt == rules.settings().slotWindowMinMs);
    const auto atMax = rules.tryAcquire(0x101, kA, 0, 1.0F);
    check(atMax.expiresAt == rules.settings().slotWindowMaxMs);
    const auto mid = rules.tryAcquire(0x102, kA, 0, 0.5F);
    check(mid.expiresAt == (rules.settings().slotWindowMinMs + rules.settings().slotWindowMaxMs) / 2);
}

void reacquireKeepsOriginalExpiry()
{
    AiRules rules;
    const auto first = rules.tryAcquire(kTarget, kA, 1000, 0.0F);
    const auto again = rules.tryAcquire(kTarget, kA, 2000, 1.0F);
    check(again.granted && again.expiresAt == first.expiresAt);
    check(rules.holderCount(kTarget, 2000) == 1);
}

void releaseFreesSlotEarly()
{
    AiRules rules;
    check(rules.tryAcquire(kTarget, kA, 0, 0.5F).granted);
    check(rules.tryAcquire(kTarget, kB, 0, 0.5F).granted);
    rules.release(kTarget, kA);
    check(rules.holderCount(kTarget, 0) == 1);
    check(rules.tryAcquire(kTarget, kC, 0, 0.5F).granted);
    // Releasing an unknown pair is a harmless no-op.
    rules.release(0x999, kA);
    rules.release(kTarget, 0x999);
}

void retaliationBypassesOnce()
{
    AiRules rules;
    check(rules.tryAcquire(kTarget, kA, 0, 0.5F).granted);
    check(rules.tryAcquire(kTarget, kB, 0, 0.5F).granted);
    check(!rules.tryAcquire(kTarget, kC, 0, 0.5F).granted);

    rules.onTargetDamagedAttacker(kTarget, kC, 100);
    check(rules.retaliationActive(kTarget, kC, 100));
    const auto answer = rules.tryAcquire(kTarget, kC, 100, 0.5F);
    check(answer.granted && answer.retaliating);
    // Consumed: after the answer there is no second bypass.
    rules.release(kTarget, kC);
    check(!rules.retaliationActive(kTarget, kC, 100));
    check(!rules.tryAcquire(kTarget, kC, 100, 0.5F).granted);
}

void retaliationWindowExpires()
{
    AiRules rules;
    check(rules.tryAcquire(kTarget, kA, 0, 0.5F).granted);
    check(rules.tryAcquire(kTarget, kB, 0, 0.5F).granted);
    rules.onTargetDamagedAttacker(kTarget, kC, 0);
    const TimeMs after = rules.settings().retaliationMs + 1;
    check(!rules.retaliationActive(kTarget, kC, after));
    check(!rules.tryAcquire(kTarget, kC, after, 0.5F).granted);
}

void zeroRetaliationDisables()
{
    AiSettings settings;
    settings.retaliationMs = 0;
    AiRules rules{settings};
    rules.onTargetDamagedAttacker(kTarget, kA, 0);
    check(!rules.retaliationActive(kTarget, kA, 0));
}

void invalidSlotInputsRejected()
{
    AiRules rules;
    check(rules.tryAcquire(0, kA, 0, 0.5F).reason == SlotRejectReason::InvalidInput);
    check(rules.tryAcquire(kTarget, 0, 0, 0.5F).reason == SlotRejectReason::InvalidInput);
    check(rules.holderCount(0, 0) == 0);
    check(!rules.holdsSlot(kTarget, kA, 0));
}

void bashCooldownEnforced()
{
    AiRules rules;
    const auto first = rules.admitBash(kA, 1000);
    check(first.allowed);
    check(first.nextAllowedAt == 1000 + rules.settings().bashCooldownMs);

    const auto denied = rules.admitBash(kA, 1001);
    check(!denied.allowed && denied.reason == BashRejectReason::OnCooldown);
    check(rules.bashCooldownRemaining(kA, 1001) == rules.settings().bashCooldownMs - 1);

    check(rules.admitBash(kA, 1000 + rules.settings().bashCooldownMs).allowed);
    // Cooldowns are per actor.
    check(rules.admitBash(kB, 1001).allowed);
}

void cancelBashRestoresEligibility()
{
    AiRules rules;
    check(rules.admitBash(kA, 1000).allowed);
    rules.cancelBash(kA, 1000);
    check(rules.bashCooldownRemaining(kA, 1000) == 0);
    check(rules.admitBash(kA, 1001).allowed);

    // A stale cancel must not erase a later attempt.
    check(rules.admitBash(kB, 5000).allowed);
    rules.cancelBash(kB, 4000);
    check(rules.bashCooldownRemaining(kB, 5000) == rules.settings().bashCooldownMs);
}

void bashRejectsInvalidInput()
{
    AiRules rules;
    check(rules.admitBash(0, 0).reason == BashRejectReason::InvalidInput);
}

void bashRollsRespectChances()
{
    AiSettings settings;
    settings.bashReactionChance = 0.35F;
    settings.bashProactiveChance = 0.0F;
    AiRules rules{settings};

    check(rules.rollBash(true, 0.0F));
    check(rules.rollBash(true, 0.34F));
    check(!rules.rollBash(true, 0.35F));
    check(!rules.rollBash(true, 0.99F));
    // Proactive chance 0: never bash a passive target.
    check(!rules.rollBash(false, 0.0F));
    check(!rules.rollBash(false, 0.99F));

    AiSettings always;
    always.bashReactionChance = 1.0F;
    always.bashProactiveChance = 1.0F;
    AiRules eager{always};
    check(eager.rollBash(true, 0.99F));
    check(eager.rollBash(false, 0.99F));
}

void turtleOpenings()
{
    AiSettings settings;
    settings.maxCircleMs = 10000;
    AiRules rules{settings};
    const TimeMs started = 1000;

    // No opening: passive target, fresh circle.
    check(!rules.isOpening(false, false, false, false, started, started + 500));
    // Each target-state opening on its own.
    check(rules.isOpening(true, false, false, false, started, started + 500));
    check(rules.isOpening(false, true, false, false, started, started + 500));
    check(rules.isOpening(false, false, true, false, started, started + 500));
    // Retaliation window opens it regardless of target state.
    check(rules.isOpening(false, false, false, true, started, started + 500));
    // Timeout boundary: strictly before maxCircleMs is closed, at/after is open.
    check(!rules.isOpening(false, false, false, false, started, started + 9999));
    check(rules.isOpening(false, false, false, false, started, started + 10000));
    check(rules.isOpening(false, false, false, false, started, started + 60000));
    // A circle that started later measures from its own start.
    check(!rules.isOpening(false, false, false, false, started + 5000, started + 10000));
}

void resetActorClearsOnlyThatActor()
{
    AiRules rules;
    check(rules.admitBash(kA, 0).allowed);
    check(rules.tryAcquire(kTarget, kA, 0, 0.5F).granted);
    rules.onTargetDamagedAttacker(kTarget, kA, 0);

    rules.resetActor(kA);
    check(rules.bashCooldownRemaining(kA, 0) == 0);
    check(rules.holderCount(kTarget, 0) == 0);
    check(!rules.retaliationActive(kTarget, kA, 0));
    check(rules.admitBash(kA, 0).allowed);
}

void resetSessionClearsEverything()
{
    AiRules rules;
    check(rules.admitBash(kA, 0).allowed);
    check(rules.tryAcquire(kTarget, kA, 0, 0.5F).granted);
    rules.resetSession();
    check(rules.targetCount() == 0);
    check(rules.bashCooldownRemaining(kA, 0) == 0);
}

void setSettingsValidatesAndClears()
{
    AiRules rules;
    check(rules.tryAcquire(kTarget, kA, 0, 0.5F).granted);

    AiSettings invalid = rules.settings();
    invalid.maxAttackers = 0;
    check(throwsInvalid(invalid));
    invalid.maxAttackers = 6;
    check(throwsInvalid(invalid));

    invalid = rules.settings();
    invalid.slotWindowMaxMs = invalid.slotWindowMinMs - 1;
    check(throwsInvalid(invalid));

    invalid = rules.settings();
    invalid.bashReactionChance = 1.5F;
    check(throwsInvalid(invalid));
    invalid.bashReactionChance = -0.1F;
    check(throwsInvalid(invalid));
    invalid.bashReactionChance = std::numeric_limits<float>::quiet_NaN();
    check(throwsInvalid(invalid));

    invalid = rules.settings();
    invalid.bashStaminaCost = -1.0F;
    check(throwsInvalid(invalid));

    invalid = rules.settings();
    invalid.maxCircleMs = 0;
    check(throwsInvalid(invalid));

    // Failed validation leaves prior settings and state untouched.
    check(rules.holderCount(kTarget, 0) == 1);

    AiSettings valid;
    valid.maxAttackers = 3;
    rules.setSettings(valid);
    check(rules.settings().maxAttackers == 3);
    check(rules.holderCount(kTarget, 0) == 0);
}

}  // namespace

int main()
{
    slotRespectsMaxAttackers();
    slotsArePerTarget();
    slotExpiryIsLazy();
    windowSampleInterpolates();
    reacquireKeepsOriginalExpiry();
    releaseFreesSlotEarly();
    retaliationBypassesOnce();
    retaliationWindowExpires();
    zeroRetaliationDisables();
    invalidSlotInputsRejected();
    bashCooldownEnforced();
    cancelBashRestoresEligibility();
    bashRejectsInvalidInput();
    bashRollsRespectChances();
    turtleOpenings();
    resetActorClearsOnlyThatActor();
    resetSessionClearsEverything();
    setSettingsValidatesAndClears();
    std::cout << checks << " checks passed\n";
    return EXIT_SUCCESS;
}
