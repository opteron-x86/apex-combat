#include "CombatRules.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace Apex::Core {
namespace {
bool within(TimeMs now, TimeMs start, TimeMs duration) noexcept
{
    // Unsigned subtraction also handles signed timestamps spanning zero without overflow.
    return now >= start &&
           static_cast<std::uint64_t>(now) - static_cast<std::uint64_t>(start) <=
               static_cast<std::uint64_t>(duration);
}

bool validCost(float value) noexcept
{
    return std::isfinite(value) && value >= 0.0F;
}
}  // namespace

CombatRules::CombatRules(Settings settings) : settings_(settings)
{
    validate(settings_);
}

const Settings& CombatRules::settings() const noexcept { return settings_; }

void CombatRules::validate(const Settings& settings)
{
    if (!validCost(settings.lightOneHandedCost) || !validCost(settings.lightTwoHandedCost) ||
        !validCost(settings.unarmedCost) || !validCost(settings.perfectBlockRestore) ||
        settings.attackCommitTimeoutMs <= 0 || settings.perfectBlockWindowMs <= 0 ||
        settings.parryRearmDelayMs < 0 ||
        (settings.affordability != Affordability::PositiveStamina &&
         settings.affordability != Affordability::FullCost)) {
        throw std::invalid_argument("Invalid combat rule settings");
    }
}

void CombatRules::setSettings(Settings settings)
{
    validate(settings);
    settings_ = settings;
    resetSession();
}

bool CombatRules::updateClock(ActorState& state, TimeMs now)
{
    const bool regressed = state.hasTime && now < state.lastTime;
    state.hasTime = true;
    state.lastTime = now;
    if (regressed) {
        state.stagePending = false;
        state.blockArmed = false;
        state.hasBlockStart = true;
        state.blockStart = now;
    }
    return !regressed;
}

ExhaustionState CombatRules::updateStamina(ActorState& state, float stamina)
{
    const bool before = state.exhausted;
    state.exhausted = stamina <= 0.0F;
    return {state.exhausted, !before && state.exhausted, before && !state.exhausted};
}

RejectReason CombatRules::affordabilityFailure(float stamina, float cost) const noexcept
{
    if (stamina <= 0.0F) {
        return RejectReason::NoStamina;
    }
    if (settings_.affordability == Affordability::FullCost && stamina < cost) {
        return RejectReason::InsufficientStamina;
    }
    return RejectReason::None;
}

AttackResult CombatRules::beginAttack(ActorId actor, EventToken stage, AttackKind kind,
                                      float stamina, TimeMs now, float vanillaPowerCost)
{
    AttackResult result;
    if (actor == 0 || stage == 0 || !std::isfinite(stamina)) {
        result.reason = RejectReason::InvalidInput;
        return result;
    }
    float cost = 0.0F;
    switch (kind) {
    case AttackKind::LightOneHanded: cost = settings_.lightOneHandedCost; break;
    case AttackKind::LightTwoHanded: cost = settings_.lightTwoHandedCost; break;
    case AttackKind::Unarmed: cost = settings_.unarmedCost; break;
    case AttackKind::Power:
        if (!validCost(vanillaPowerCost)) {
            result.reason = RejectReason::InvalidInput;
            return result;
        }
        cost = vanillaPowerCost;
        break;
    default:
        result.reason = RejectReason::InvalidInput;
        return result;
    }

    auto& state = actors_[actor];
    result.exhausted = updateStamina(state, stamina).exhausted;
    if (!updateClock(state, now)) {
        result.reason = RejectReason::ClockRegressed;
        return result;
    }
    if (stage <= state.highestStage) {
        result.duplicate = true;
        if (stage == state.highestStage && state.stagePending) {
            if (!within(now, state.stageStart, settings_.attackCommitTimeoutMs)) {
                state.stagePending = false;
                result.reason = RejectReason::StageExpired;
            } else {
                result.accepted = true;
                result.nominalCost = state.stageCost;
                result.engineOwnsCost = state.engineOwnsCost;
            }
        } else {
            result.reason = RejectReason::DuplicateToken;
        }
        return result;
    }

    // A new stage supersedes an uncommitted request. There is no reservation to refund.
    state.highestStage = stage;
    state.stagePending = false;
    result.nominalCost = cost;
    result.engineOwnsCost = kind == AttackKind::Power && !settings_.chargePowerAttacks;
    result.reason = affordabilityFailure(stamina, cost);
    if (result.reason != RejectReason::None) {
        return result;
    }
    state.stagePending = true;
    state.stageStart = now;
    state.stageCost = cost;
    state.engineOwnsCost = result.engineOwnsCost;
    result.accepted = true;
    return result;
}

AttackResult CombatRules::commitAttack(ActorId actor, EventToken stage,
                                       float stamina, TimeMs now)
{
    AttackResult result;
    if (actor == 0 || stage == 0 || !std::isfinite(stamina)) {
        result.reason = RejectReason::InvalidInput;
        return result;
    }
    auto found = actors_.find(actor);
    if (found == actors_.end()) {
        result.reason = RejectReason::MissingStage;
        return result;
    }
    auto& state = found->second;
    result.exhausted = updateStamina(state, stamina).exhausted;
    if (!updateClock(state, now)) {
        result.reason = RejectReason::ClockRegressed;
        return result;
    }
    if (!state.stagePending || stage != state.highestStage) {
        result.duplicate = stage <= state.highestStage;
        result.reason = result.duplicate ? RejectReason::DuplicateToken : RejectReason::MissingStage;
        return result;
    }
    state.stagePending = false;
    result.nominalCost = state.stageCost;
    result.engineOwnsCost = state.engineOwnsCost;
    if (!within(now, state.stageStart, settings_.attackCommitTimeoutMs)) {
        result.reason = RejectReason::StageExpired;
        return result;
    }
    result.reason = affordabilityFailure(stamina, state.stageCost);
    if (result.reason != RejectReason::None) {
        return result;
    }
    result.accepted = true;
    result.staminaDebit = state.engineOwnsCost ? 0.0F : std::min(stamina, state.stageCost);
    result.exhausted = updateStamina(state, stamina - result.staminaDebit).exhausted;
    return result;
}

bool CombatRules::cancelAttack(ActorId actor, EventToken stage, TimeMs now)
{
    const auto found = actors_.find(actor);
    if (found == actors_.end()) {
        return false;
    }
    auto& state = found->second;
    if (!updateClock(state, now) || stage == 0 || !state.stagePending || stage != state.highestStage) {
        return false;
    }
    state.stagePending = false;
    return true;
}

ExhaustionState CombatRules::observeStamina(ActorId actor, float stamina, TimeMs now)
{
    if (actor == 0 || !std::isfinite(stamina)) {
        return {isExhausted(actor), false, false};
    }
    auto& state = actors_[actor];
    updateClock(state, now);
    return updateStamina(state, stamina);
}

bool CombatRules::isExhausted(ActorId actor) const noexcept
{
    const auto found = actors_.find(actor);
    return found != actors_.end() && found->second.exhausted;
}

bool CombatRules::setBlocking(ActorId actor, bool blocking, TimeMs now)
{
    if (actor == 0) {
        return false;
    }
    auto& state = actors_[actor];
    if (!updateClock(state, now)) {
        state.blocking = blocking;
        return false;
    }
    if (blocking == state.blocking) {
        return false;
    }
    state.blocking = blocking;
    state.blockArmed = false;
    if (!blocking) {
        return false;
    }
    const bool rearmed = !state.hasBlockStart || settings_.parryRearmDelayMs == 0 ||
                         !within(now, state.blockStart, settings_.parryRearmDelayMs - 1);
    state.hasBlockStart = true;
    state.blockStart = now;
    state.blockArmed = rearmed;
    return rearmed;
}

ParryResult CombatRules::successfulBlock(ActorId actor, EventToken hit, float stamina,
                                         float maxStamina, TimeMs now)
{
    ParryResult result;
    if (actor == 0 || hit == 0 || !std::isfinite(stamina) ||
        !std::isfinite(maxStamina) || maxStamina <= 0.0F) {
        result.reason = ParryReason::InvalidInput;
        return result;
    }
    auto& state = actors_[actor];
    result.exhausted = updateStamina(state, stamina).exhausted;
    if (!updateClock(state, now)) {
        result.reason = ParryReason::ClockRegressed;
        return result;
    }
    if (hit <= state.highestHit) {
        result.reason = ParryReason::DuplicateToken;
        return result;
    }
    state.highestHit = hit;
    if (!state.blocking || !state.blockArmed ||
        !within(now, state.blockStart, settings_.perfectBlockWindowMs)) {
        state.blockArmed = false;
        return result;
    }
    state.blockArmed = false;
    result.perfect = true;
    result.reason = ParryReason::Perfect;
    const float current = std::clamp(stamina, 0.0F, maxStamina);
    result.staminaRestore = std::min(settings_.perfectBlockRestore, maxStamina - current);
    result.exhausted = updateStamina(state, stamina + result.staminaRestore).exhausted;
    return result;
}

void CombatRules::resetActor(ActorId actor) { actors_.erase(actor); }
void CombatRules::resetSession() noexcept { actors_.clear(); }
std::size_t CombatRules::actorCount() const noexcept { return actors_.size(); }

}  // namespace Apex::Core
