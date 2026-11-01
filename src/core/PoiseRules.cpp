#include "PoiseRules.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace Apex::Core {
namespace {

bool validDamage(float value) noexcept
{
    return std::isfinite(value) && value >= 0.0F;
}

float baseDamage(const PoiseSettings& settings, PoiseKind kind) noexcept
{
    switch (kind) {
    case PoiseKind::Dagger: return settings.daggerDamage;
    case PoiseKind::Axe: return settings.axeDamage;
    case PoiseKind::Mace: return settings.maceDamage;
    case PoiseKind::TwoHanded: return settings.twoHandedDamage;
    case PoiseKind::Unarmed: return settings.unarmedDamage;
    case PoiseKind::Sword: break;
    }
    return settings.swordDamage;
}

}  // namespace

PoiseRules::PoiseRules(PoiseSettings settings)
{
    validate(settings);
    settings_ = settings;
}

const PoiseSettings& PoiseRules::settings() const noexcept
{
    return settings_;
}

void PoiseRules::validate(const PoiseSettings& settings)
{
    if (!(settings.maxPoise > 0.0F) || !std::isfinite(settings.maxPoise) ||
        !validDamage(settings.daggerDamage) || !validDamage(settings.swordDamage) ||
        !validDamage(settings.axeDamage) || !validDamage(settings.maceDamage) ||
        !validDamage(settings.twoHandedDamage) || !validDamage(settings.unarmedDamage) ||
        !validDamage(settings.powerMultiplier) ||         !validDamage(settings.blockedMultiplier) || settings.counterattackPoiseMultiplier < 1.0F ||
        !validDamage(settings.perfectParryDamage) || !validDamage(settings.recoveryPerSecond) ||
        settings.recoveryDelayMs < 0 || settings.breakRecoveryMs < 0) {
        throw std::invalid_argument("Invalid poise settings");
    }
}

void PoiseRules::setSettings(PoiseSettings settings)
{
    validate(settings);
    settings_ = settings;
    resetSession();
}

float PoiseRules::observe(ActorState& state, TimeMs now)
{
    if (state.broken) {
        if (now >= state.staggeredUntil) {
            state.broken = false;
            state.staggeredUntil = 0;
            state.poise = settings_.maxPoise;
            state.lastDamage = now;
        }
    } else if (state.seen && state.poise < settings_.maxPoise &&
               now > state.lastDamage + settings_.recoveryDelayMs) {
        const auto elapsed = static_cast<float>(now - state.lastDamage - settings_.recoveryDelayMs);
        state.poise = std::min(settings_.maxPoise,
                               state.poise + settings_.recoveryPerSecond * elapsed / 1000.0F);
    }
    return state.poise;
}

PoiseResult PoiseRules::applyDamage(ActorId actor, PoiseKind kind, bool power,
                                    PoiseSource source, TimeMs now, float damageMultiplier)
{
    PoiseResult result;
    if (actor == 0 || now < 0 || kind < PoiseKind::Dagger || kind > PoiseKind::Unarmed ||
        source < PoiseSource::Hit || source > PoiseSource::PerfectParry ||
        !std::isfinite(damageMultiplier) || damageMultiplier < 0.0F) {
        result.reason = PoiseRejectReason::InvalidInput;
        return result;
    }
    auto& state = actors_[actor];
    if (!state.seen) {
        state.seen = true;
        state.poise = settings_.maxPoise;
    }
    observe(state, now);
    if (state.broken) {
        // StaggerMode semantics: a staggered actor takes no further poise damage
        // until its break recovery window elapses.
        result.reason = PoiseRejectReason::Staggered;
        result.poiseAfter = state.poise;
        result.staggeredUntil = state.staggeredUntil;
        return result;
    }
    float damage = baseDamage(settings_, kind) * (power ? settings_.powerMultiplier : 1.0F);
    damage *= damageMultiplier;
    if (source == PoiseSource::BlockedHit) damage *= settings_.blockedMultiplier;
    else if (source == PoiseSource::PerfectParry) damage = settings_.perfectParryDamage;
    state.poise -= damage;
    state.lastDamage = now;
    result.applied = true;
    if (state.poise <= 0.0F) {
        state.poise = 0.0F;
        state.broken = true;
        state.staggeredUntil = now + settings_.breakRecoveryMs;
        result.broke = true;
        result.staggeredUntil = state.staggeredUntil;
    }
    result.poiseAfter = state.poise;
    return result;
}

float PoiseRules::poise(ActorId actor, TimeMs now)
{
    if (actor == 0) return settings_.maxPoise;
    auto it = actors_.find(actor);
    if (it == actors_.end() || !it->second.seen) return settings_.maxPoise;
    return observe(it->second, now);
}

bool PoiseRules::isStaggered(ActorId actor, TimeMs now)
{
    auto it = actors_.find(actor);
    if (it == actors_.end() || !it->second.seen) return false;
    observe(it->second, now);
    return it->second.broken;
}

void PoiseRules::resetActor(ActorId actor)
{
    actors_.erase(actor);
}

void PoiseRules::resetSession() noexcept
{
    actors_.clear();
}

std::size_t PoiseRules::actorCount() const noexcept
{
    return actors_.size();
}

}  // namespace Apex::Core
