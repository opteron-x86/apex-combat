#include "core/NativePowerCost.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <source_location>

using namespace Apex::Core;
namespace {
int checks = 0;
void check(bool condition, const std::source_location at = std::source_location::current())
{
    ++checks;
    if (!condition) {
        std::cerr << at.file_name() << ':' << at.line() << ": check failed\n";
        std::exit(EXIT_FAILURE);
    }
}

void reservationIsOneStage()
{
    NativePowerReservation reward;
    check(!reward.matches(1, 0));
    reward.reserve(7, 100, 1000);
    check(!reward.matches(0, 100));
    check(!reward.matches(6, 100));
    check(!reward.matches(8, 100));
    check(!reward.matches(7, 99));
    check(reward.matches(7, 100));
    check(reward.matches(7, 1100));
    check(!reward.matches(7, 1101));
    // Repeated affordability and debit queries do not spend the reservation.
    check(reward.matches(7, 101));
    check(reward.matches(7, 101));
    check(reward.commit(7, 1100));
    // A committed long animation remains free until its lifecycle ends.
    check(reward.matches(7, 100000));
    check(!reward.matches(8, 100000));
    reward.clear();
    check(!reward.matches(7, 100001));
}

void invalidAndSupersededReservations()
{
    NativePowerReservation reward;
    reward.reserve(0, 1, 1000);
    check(!reward.matches(0, 1));
    reward.reserve(1, 1, 0);
    check(!reward.matches(1, 1));
    reward.reserve(1, 1, -1);
    check(!reward.matches(1, 1));
    reward.reserve(1, 1, 1000);
    check(!reward.commit(2, 2));
    check(!reward.matches(1, 2));
    reward.reserve(2, 100, 1000);
    check(!reward.commit(2, 99));
    check(!reward.matches(2, 100));
    reward.reserve(3, 100, 1000);
    check(!reward.commit(3, 1101));
    check(!reward.matches(3, 1101));
    reward.reserve(4, 1102, 1000);
    reward.reserve(5, 1103, 1000);
    check(!reward.matches(4, 1103));
    check(reward.matches(5, 1103));
    // Unsigned elapsed calculation supports timestamps across zero without overflow.
    reward.reserve(6, std::numeric_limits<TimeMs>::min(), 1000);
    check(reward.matches(6, std::numeric_limits<TimeMs>::min() + 1000));
    check(!reward.matches(6, std::numeric_limits<TimeMs>::max()));
}

void chainedVisitorsAppendOnce()
{
    int actorA = 0, actorB = 0, visitorA = 0, visitorB = 0;
    {
        const NativePowerVisitScope outer(&actorA, &visitorA);
        check(!outer.reentered());
        {
            const NativePowerVisitScope recursive(&actorA, &visitorA);
            check(recursive.reentered());
            const NativePowerVisitScope nestedQuery(&actorA, &visitorB);
            check(!nestedQuery.reentered());
            const NativePowerVisitScope otherActor(&actorB, &visitorA);
            check(!otherActor.reentered());
        }
        const NativePowerVisitScope afterNested(&actorA, &visitorB);
        check(!afterNested.reentered());
    }
    const NativePowerVisitScope laterQuery(&actorA, &visitorA);
    check(!laterQuery.reentered());
}

void scalarPolicy()
{
    check(nativePowerMultiplier(1.0F, false) == 1.0F);
    check(nativePowerMultiplier(0.75F, false) == 0.75F);
    check(nativePowerMultiplier(0.0F, false) == 0.0F);
    check(nativePowerMultiplier(-0.1F, false) == 1.0F);
    check(nativePowerMultiplier(1.1F, false) == 1.0F);
    check(nativePowerMultiplier(std::numeric_limits<float>::infinity(), false) == 1.0F);
    check(nativePowerMultiplier(std::numeric_limits<float>::quiet_NaN(), false) == 1.0F);
    check(nativePowerMultiplier(0.75F, true) == 0.0F);
    check(nativePowerMultiplier(1.0F, true) == 0.0F);
    // The modifier scales the existing native result once; it owns no stamina debit.
    check(80.0F * nativePowerMultiplier(0.75F, false) == 60.0F);
    check(80.0F * nativePowerMultiplier(0.0F, false) == 0.0F);
}

void coreReservationLifecycle()
{
    CombatRules rules;
    NativePowerReservation reward;
    check(rules.grantCounterattack(1, 0));
    // The unconsumed reward alone must not grant free unmanaged native queries.
    check(rules.counterattackReady(1, 0));
    check(!reward.matches(1, 0));
    const auto first = rules.beginAttack(1, 1, AttackKind::Power, 0, 10);
    check(first.accepted && first.freeCounterattack && first.engineOwnsCost);
    reward.reserve(1, 10, rules.settings().attackCommitTimeoutMs);
    check(nativePowerMultiplier(1.0F, reward.matches(1, 10)) == 0.0F);
    check(rules.cancelAttack(1, 1, 11));
    reward.clear();
    check(!reward.matches(1, 11));
    check(rules.counterattackReady(1, 11));
    const auto retry = rules.beginAttack(1, 2, AttackKind::Power, 0, 12);
    check(retry.accepted && retry.freeCounterattack);
    reward.reserve(2, 12, rules.settings().attackCommitTimeoutMs);
    const auto committed = rules.commitAttack(1, 2, 0, 13);
    check(committed.accepted && committed.staminaDebit == 0 && committed.freeCounterattack);
    check(reward.commit(2, 13));
    check(reward.matches(2, 10000));
    check(!rules.counterattackReady(1, 14));
    // A new normal power or a bash clears runtime reservation before admission.
    reward.clear();
    const auto next = rules.beginAttack(1, 3, AttackKind::Power, 20, 14);
    check(next.accepted && !next.freeCounterattack);
    check(nativePowerMultiplier(0.75F, reward.matches(3, 14)) == 0.75F);
    check(rules.grantCounterattack(2, 20));
    const auto bash = rules.beginAttack(2, 1, AttackKind::Bash, 20, 21);
    check(bash.accepted && bash.engineOwnsCost && !bash.freeCounterattack);
    check(rules.counterattackReady(2, 21));
    // Expiry/failed commit cannot leave a zero-cost native reservation behind.
    reward.reserve(4, 100, 5);
    check(!reward.commit(4, 106));
    check(nativePowerMultiplier(1.0F, reward.matches(4, 106)) == 1.0F);
}
}
int main()
{
    reservationIsOneStage();
    invalidAndSupersededReservations();
    chainedVisitorsAppendOnce();
    scalarPolicy();
    coreReservationLifecycle();
    std::cout << "native power-cost tests passed (" << checks << " checks)\n";
}
