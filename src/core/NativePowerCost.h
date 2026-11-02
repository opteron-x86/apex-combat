#pragma once

#include "CombatRules.h"
#include <cmath>

namespace Apex::Core {

// A native cost query may occur several times for one attack. Only admission
// reserves the reward; queries never consume it or infer a reward from stamina.
class NativePowerReservation {
public:
    void clear() noexcept { stage_ = 0; committed_ = false; }

    void reserve(EventToken stage, TimeMs now, TimeMs commitTimeout) noexcept
    {
        clear();
        if (stage == 0 || commitTimeout <= 0) return;
        stage_ = stage;
        started_ = now;
        timeout_ = commitTimeout;
    }

    bool matches(EventToken stage, TimeMs now) const noexcept
    {
        if (stage_ == 0 || stage != stage_ || now < started_) return false;
        return committed_ || static_cast<std::uint64_t>(now) -
            static_cast<std::uint64_t>(started_) <= static_cast<std::uint64_t>(timeout_);
    }

    bool commit(EventToken stage, TimeMs now) noexcept
    {
        if (!matches(stage, now)) { clear(); return false; }
        committed_ = true;
        return true;
    }

private:
    EventToken stage_{0};
    TimeMs started_{0};
    TimeMs timeout_{0};
    bool committed_{false};
};

// A chained hook can re-enter the same visitor. Append once at the outermost
// matching call; nested calculations with their own visitor remain independent.
class NativePowerVisitScope {
public:
    NativePowerVisitScope(const void* actor, const void* visitor) noexcept
        : actor_(actor), visitor_(visitor), previous_(top_)
    {
        for (auto* frame = top_; frame; frame = frame->previous_)
            if (frame->actor_ == actor && frame->visitor_ == visitor) reentered_ = true;
        top_ = this;
    }
    ~NativePowerVisitScope() { top_ = previous_; }
    NativePowerVisitScope(const NativePowerVisitScope&) = delete;
    NativePowerVisitScope& operator=(const NativePowerVisitScope&) = delete;
    bool reentered() const noexcept { return reentered_; }
private:
    const void* actor_;
    const void* visitor_;
    NativePowerVisitScope* previous_;
    bool reentered_{false};
    static inline thread_local NativePowerVisitScope* top_{nullptr};
};

inline float nativePowerMultiplier(float potionMultiplier, bool reservedCounter) noexcept
{
    if (reservedCounter) return 0.0F;
    return std::isfinite(potionMultiplier) && potionMultiplier >= 0.0F && potionMultiplier <= 1.0F
        ? potionMultiplier : 1.0F;
}

} // namespace Apex::Core
