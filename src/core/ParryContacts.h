#pragma once

#include "CombatRules.h"
#include <unordered_set>

namespace Apex::Core {

// Contacts are identified by the attacker's monotonically increasing committed
// strike generation and the defender. This is intentionally not a time cooldown:
// the next combo strike may be parried immediately, even against the same target.
// Keep one ledger per attacker and reset it with that actor's strike sequence.
class ParryContacts {
public:
    bool contains(EventToken strike, ActorId defender) const noexcept
    {
        return strike != 0 && strike == strike_ && defenders_.contains(defender);
    }

    // False means invalid, stale, or already recorded. Call only for an accepted
    // perfect block; normal contacts must never create immunity for later hits.
    bool remember(EventToken strike, ActorId defender)
    {
        if (strike == 0 || defender == 0 || strike < strike_) return false;
        if (strike > strike_) {
            strike_ = strike;
            defenders_.clear();
        }
        return defenders_.insert(defender).second;
    }

private:
    EventToken strike_{0};
    std::unordered_set<ActorId> defenders_;
};

} // namespace Apex::Core
