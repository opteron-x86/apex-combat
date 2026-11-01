#include "AiRules.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Apex::Core {

AiRules::AiRules(AiSettings settings) : settings_(settings) { validate(settings_); }

const AiSettings& AiRules::settings() const noexcept { return settings_; }

void AiRules::setSettings(AiSettings settings) {
    validate(settings);
    settings_ = settings;
    resetSession();
}

void AiRules::validate(const AiSettings& settings) {
    const auto finite = [](float value) { return std::isfinite(value); };
    if (settings.maxAttackers < 1 || settings.maxAttackers > 5) {
        throw std::invalid_argument("maxAttackers must be within 1..5");
    }
    if (settings.slotWindowMinMs < 0 || settings.slotWindowMaxMs < settings.slotWindowMinMs) {
        throw std::invalid_argument("slot window must satisfy 0 <= min <= max");
    }
    if (settings.retaliationMs < 0) {
        throw std::invalid_argument("retaliationMs must be non-negative");
    }
    if (settings.bashCooldownMs < 0) {
        throw std::invalid_argument("bashCooldownMs must be non-negative");
    }
    if (!finite(settings.bashReactionChance) || settings.bashReactionChance < 0.0F ||
        settings.bashReactionChance > 1.0F) {
        throw std::invalid_argument("bashReactionChance must be within 0..1");
    }
    if (!finite(settings.bashProactiveChance) || settings.bashProactiveChance < 0.0F ||
        settings.bashProactiveChance > 1.0F) {
        throw std::invalid_argument("bashProactiveChance must be within 0..1");
    }
    if (!finite(settings.bashStaminaCost) || settings.bashStaminaCost < 0.0F) {
        throw std::invalid_argument("bashStaminaCost must be finite and non-negative");
    }
    if (settings.maxCircleMs <= 0) {
        throw std::invalid_argument("maxCircleMs must be positive");
    }
}

void AiRules::evictExpired(TargetState& state, TimeMs now) noexcept {
    for (auto it = state.holders.begin(); it != state.holders.end();) {
        if (it->second.expiresAt <= now) {
            it = state.holders.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = state.retaliationUntil.begin(); it != state.retaliationUntil.end();) {
        if (it->second <= now) {
            it = state.retaliationUntil.erase(it);
        } else {
            ++it;
        }
    }
}

SlotResult AiRules::tryAcquire(ActorId target, ActorId attacker, TimeMs now, float windowSample) {
    if (target == 0 || attacker == 0) {
        return {false, false, SlotRejectReason::InvalidInput, 0};
    }
    auto& state = targets_[target];
    evictExpired(state, now);

    const auto held = state.holders.find(attacker);
    if (held != state.holders.end()) {
        return {true, false, SlotRejectReason::None, held->second.expiresAt};
    }

    const auto retaliation = state.retaliationUntil.find(attacker);
    const bool retaliating = retaliation != state.retaliationUntil.end() && retaliation->second > now;

    if (state.holders.size() >= settings_.maxAttackers && !retaliating) {
        return {false, false, SlotRejectReason::NoSlot, 0};
    }

    const float sample = std::clamp(windowSample, 0.0F, 1.0F);
    const auto span = settings_.slotWindowMaxMs - settings_.slotWindowMinMs;
    const auto window = settings_.slotWindowMinMs + static_cast<TimeMs>(span * sample);
    const TimeMs expiresAt = now + std::max<TimeMs>(window, 1);
    state.holders.emplace(attacker, Holder{expiresAt});
    if (retaliating) {
        // Retaliation slots answer once; the window does not renew.
        state.retaliationUntil.erase(attacker);
    }
    return {true, retaliating, SlotRejectReason::None, expiresAt};
}

void AiRules::release(ActorId target, ActorId attacker) noexcept {
    const auto it = targets_.find(target);
    if (it == targets_.end()) {
        return;
    }
    it->second.holders.erase(attacker);
}

void AiRules::onTargetDamagedAttacker(ActorId target, ActorId attacker, TimeMs now) {
    if (target == 0 || attacker == 0 || settings_.retaliationMs == 0) {
        return;
    }
    auto& state = targets_[target];
    state.retaliationUntil[attacker] = now + settings_.retaliationMs;
}

std::uint32_t AiRules::holderCount(ActorId target, TimeMs now) {
    const auto it = targets_.find(target);
    if (it == targets_.end()) {
        return 0;
    }
    evictExpired(it->second, now);
    return static_cast<std::uint32_t>(it->second.holders.size());
}

bool AiRules::holdsSlot(ActorId target, ActorId attacker, TimeMs now) {
    const auto it = targets_.find(target);
    if (it == targets_.end()) {
        return false;
    }
    evictExpired(it->second, now);
    return it->second.holders.find(attacker) != it->second.holders.end();
}

bool AiRules::retaliationActive(ActorId target, ActorId attacker, TimeMs now) {
    const auto it = targets_.find(target);
    if (it == targets_.end()) {
        return false;
    }
    const auto r = it->second.retaliationUntil.find(attacker);
    return r != it->second.retaliationUntil.end() && r->second > now;
}

BashResult AiRules::admitBash(ActorId actor, TimeMs now) {
    if (actor == 0) {
        return {false, BashRejectReason::InvalidInput, 0};
    }
    const auto it = lastBash_.find(actor);
    if (it != lastBash_.end()) {
        const TimeMs nextAllowed = it->second + settings_.bashCooldownMs;
        if (nextAllowed > now) {
            return {false, BashRejectReason::OnCooldown, nextAllowed};
        }
    }
    lastBash_[actor] = now;
    return {true, BashRejectReason::None, now + settings_.bashCooldownMs};
}

void AiRules::cancelBash(ActorId actor, TimeMs recordedAt) noexcept {
    const auto it = lastBash_.find(actor);
    // Only cancel the record this attempt created; a later bash may already
    // have overwritten it.
    if (it != lastBash_.end() && it->second == recordedAt) {
        lastBash_.erase(it);
    }
}

TimeMs AiRules::bashCooldownRemaining(ActorId actor, TimeMs now) const noexcept {
    const auto it = lastBash_.find(actor);
    if (it == lastBash_.end()) {
        return 0;
    }
    const TimeMs remaining = (it->second + settings_.bashCooldownMs) - now;
    return std::max<TimeMs>(remaining, 0);
}

bool AiRules::rollBash(bool targetIsAttacking, float sample) const noexcept {
    const float chance = targetIsAttacking ? settings_.bashReactionChance : settings_.bashProactiveChance;
    if (chance <= 0.0F) {
        return false;
    }
    if (chance >= 1.0F) {
        return true;
    }
    const float clamped = std::clamp(sample, 0.0F, 1.0F);
    return clamped < chance;
}

bool AiRules::isOpening(bool targetRecovering, bool targetStaggered, bool targetExhausted,
    bool retaliating, TimeMs circleStartedAt, TimeMs now) const noexcept {
    if (targetRecovering || targetStaggered || targetExhausted || retaliating) return true;
    return now - circleStartedAt >= settings_.maxCircleMs;
}

void AiRules::resetActor(ActorId actor) noexcept {
    lastBash_.erase(actor);
    for (auto& [target, state] : targets_) {
        state.holders.erase(actor);
        state.retaliationUntil.erase(actor);
    }
}

void AiRules::resetSession() noexcept {
    targets_.clear();
    lastBash_.clear();
}

std::size_t AiRules::targetCount() const noexcept { return targets_.size(); }

}  // namespace Apex::Core
