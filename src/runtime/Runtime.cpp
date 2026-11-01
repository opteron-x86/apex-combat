#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <REX/W32/KERNEL32.h>
#include <Windows.h>
#include "Runtime.h"
#include "core/AiRules.h"
#include "core/CombatRules.h"
#include "core/Configuration.h"
#include "core/ParryContacts.h"
#include "core/PoiseRules.h"
#include "precision/PrecisionAPI.h"
#include "truehud/TrueHUDAPI.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Apex {
namespace {
using Core::AttackKind;
using Core::EventToken;
using Core::TimeMs;
Configuration config;
Core::CombatRules rules;
Core::PoiseRules poiseRules;
Core::AiRules aiRules;
std::recursive_mutex stateMutex;
std::atomic_bool active{false};
bool initialized{false};
bool precisionReady{false};
TRUEHUD_API::IVTrueHUD1* trueHud{nullptr};
bool trueHudReady{false};
RE::BGSSoundDescriptorForm* parrySoundWeaponDesc{nullptr};
RE::BGSSoundDescriptorForm* parrySoundShieldDesc{nullptr};
float parrySoundVolume{1.0f};
bool parryVisual{true};

struct ActorState {
    EventToken stage{0};
    EventToken hit{0};
    bool committed{false};
    bool denied{false};
    bool nextStage{true};
    bool exhausted{false};
    bool freePowerPending{false};
    float freePowerBaseline{0.0F};
    EventToken freePowerStage{0};
    TimeMs lastSeen{0};
    // Director: a recorded bash whose commit never landed must be cancelled
    // (rejected/superseded begin), restoring the cooldown.
    bool bashPending{false};
    TimeMs bashRecordedAt{0};
    Core::ParryContacts contacts;
};
std::unordered_map<RE::FormID, ActorState> actors;
TimeMs lastPrune{0};

TimeMs Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string Lower(std::string_view value) {
    std::string result(value);
    for (char& c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return result;
}

bool StableGameplay() {
    if (!active.load()) return false;
    auto* ui = RE::UI::GetSingleton();
    return ui && !ui->GameIsPaused() && !ui->IsMenuOpen(RE::MainMenu::MENU_NAME) &&
        !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
}

bool EligibleActor(RE::Actor* actor) {
    return actor && !actor->IsDead() && !actor->IsInKillMove() && actor->Is3DLoaded();
}

bool ManageAttack(RE::Actor* actor) {
    return EligibleActor(actor) && (actor->IsPlayerRef() ? config.playerAttacks : config.npcAttacks);
}

float Stamina(RE::Actor* actor) {
    return actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
}

float MaximumStamina(RE::Actor* actor) {
    auto* owner = actor->AsActorValueOwner();
    const float damage = actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kStamina);
    return std::max(0.0F, owner->GetActorValue(RE::ActorValue::kStamina) - damage);
}

void Trace(RE::Actor* actor, std::string_view action, EventToken token, float amount, int reason = 0) {
    if (config.trace) SKSE::log::info("actor={:08X} action={} token={} amount={} stamina={} reason={}",
        actor->GetFormID(), action, token, amount, Stamina(actor), reason);
}

void Prune(TimeMs now) {
    if (now - lastPrune < 30000) return;
    for (auto it = actors.begin(); it != actors.end();) {
        if (now - it->second.lastSeen > 120000) {
            rules.resetActor(it->first);
            poiseRules.resetActor(static_cast<Core::ActorId>(it->first));
            it = actors.erase(it);
        } else ++it;
    }
    lastPrune = now;
}

ActorState& State(RE::Actor* actor, TimeMs now) {
    auto& state = actors[actor->GetFormID()];
    state.lastSeen = now;
    return state;
}

// Applies the exhaustion recovery delay exactly on a false-true flip of the
// observed exhaustion state. Caller must hold stateMutex.
void ObserveExhaustion(RE::Actor* actor, bool exhausted, TimeMs now) {
    auto& state = State(actor, now);
    const bool entered = exhausted && !state.exhausted;
    state.exhausted = exhausted;
    if (entered) {
        auto* process = actor->GetActorRuntimeData().currentProcess;
        if (process) process->UpdateRegenDelay(RE::ActorValue::kStamina, config.exhaustionRecoveryDelaySeconds);
        Trace(actor, "exhaustion-delay", 0, config.exhaustionRecoveryDelaySeconds);
    }
}

bool RangedWeapon(RE::TESObjectWEAP* weapon) {
    if (!weapon) return false;
    auto type = weapon->GetWeaponType();
    return type == RE::WEAPON_TYPE::kBow || type == RE::WEAPON_TYPE::kCrossbow || type == RE::WEAPON_TYPE::kStaff;
}

bool Ranged(RE::Actor* actor) {
    auto* form = actor->GetEquippedObject(false);
    return RangedWeapon(form ? form->As<RE::TESObjectWEAP>() : nullptr);
}

// Cost of one swing with a weapon of this type, from the current settings.
float WeaponTypeCost(RE::WEAPON_TYPE type) {
    const auto& settings = rules.settings();
    switch (type) {
    case RE::WEAPON_TYPE::kOneHandDagger: return settings.daggerCost;
    case RE::WEAPON_TYPE::kOneHandAxe: return settings.axeCost;
    case RE::WEAPON_TYPE::kOneHandMace: return settings.maceCost;
    case RE::WEAPON_TYPE::kTwoHandSword:
    case RE::WEAPON_TYPE::kTwoHandAxe: return settings.lightTwoHandedCost;
    case RE::WEAPON_TYPE::kHandToHandMelee: return settings.unarmedCost;
    default: return settings.lightOneHandedCost;
    }
}

bool MeleeWeapon(RE::TESObjectWEAP* weapon) { return weapon && !RangedWeapon(weapon); }

AttackKind Kind(RE::Actor* actor, bool powerHint, float& customCost) {
    if (powerHint || actor->IsPowerAttacking()) return AttackKind::Power;
    auto* rightForm = actor->GetEquippedObject(false);
    auto* right = rightForm ? rightForm->As<RE::TESObjectWEAP>() : nullptr;
    auto* leftForm = actor->GetEquippedObject(true);
    auto* left = leftForm ? leftForm->As<RE::TESObjectWEAP>() : nullptr;
    if (MeleeWeapon(right) && MeleeWeapon(left)) {
        customCost = (WeaponTypeCost(right->GetWeaponType()) + WeaponTypeCost(left->GetWeaponType())) *
                     0.5F * rules.settings().dualWieldMultiplier;
        return AttackKind::DualWield;
    }
    auto* entry = actor->GetAttackingWeapon();
    auto* weapon = entry && entry->object ? entry->object->As<RE::TESObjectWEAP>() : nullptr;
    if (!MeleeWeapon(weapon)) weapon = MeleeWeapon(right) ? right : (MeleeWeapon(left) ? left : nullptr);
    if (!weapon) return AttackKind::Unarmed;
    auto type = weapon->GetWeaponType();
    if (type == RE::WEAPON_TYPE::kOneHandDagger) return AttackKind::Dagger;
    if (type == RE::WEAPON_TYPE::kOneHandAxe) return AttackKind::OneHandedAxe;
    if (type == RE::WEAPON_TYPE::kOneHandMace) return AttackKind::OneHandedMace;
    if (type == RE::WEAPON_TYPE::kTwoHandSword || type == RE::WEAPON_TYPE::kTwoHandAxe)
        return AttackKind::LightTwoHanded;
    if (type == RE::WEAPON_TYPE::kHandToHandMelee) return AttackKind::Unarmed;
    return AttackKind::LightOneHanded;  // Sword plus legacy fallback for unmapped types.
}

bool AttackRequest(std::string_view event) {
    if (event == "attackstop" || event == "attackend" || event == "attackrelease") return false;
    return event.starts_with("attackstart") || event.starts_with("attackpowerstart") ||
        event.starts_with("bfcoattackstart") || event.starts_with("bfco_powerattackstart") ||
        event.starts_with("mco_attackstart") || event.starts_with("mco_powerattackstart");
}

bool BashEvent(std::string_view event) {
    return event == "bashstart" || event == "bashpowerstart" || event == "mcobashstart" ||
        event == "bfco_bashstart";
}

// Uniform sample in [0,1) for director rolls and slot window draws.
float RandomUnit() {
    static std::mt19937 rng{std::random_device{}()};
    return std::generate_canonical<float, 24>(rng);
}

// The director gates NPC melee against the player; NPC-vs-NPC stays vanilla
// unless director.playeronly is off. Returns the attack target for `actor`,
// or nullptr when the director does not manage this attack.
RE::Actor* DirectorTarget(RE::Actor* actor) {
    if (!config.director || actor->IsPlayerRef() || !EligibleActor(actor)) return nullptr;
    auto* controller = actor->GetActorRuntimeData().combatController;
    auto target = controller ? controller->targetHandle.get() : RE::NiPointer<RE::Actor>{};
    auto* targetActor = target.get();
    if (!EligibleActor(targetActor)) return nullptr;
    if (config.directorPlayerOnly && !targetActor->IsPlayerRef()) return nullptr;
    return targetActor;
}

// Cancels a recorded bash whose commit never landed. Caller must hold
// stateMutex; `state` must belong to `actor`.
void CancelPendingBash(RE::Actor* actor, ActorState& state) {
    if (!state.bashPending) return;
    aiRules.cancelBash(actor->GetFormID(), state.bashRecordedAt);
    state.bashPending = false;
    state.bashRecordedAt = 0;
}

bool Begin(RE::Actor* actor, bool power, TimeMs now) {
    std::scoped_lock lock(stateMutex);
    Prune(now);
    auto& state = State(actor, now);
    CancelPendingBash(actor, state);
    float customCost = 0.0F;
    const auto kind = Kind(actor, power, customCost);
    const float baseline = Stamina(actor);
    // Power attack costs remain owned by the game. Positive stamina is required.
    auto result = rules.beginAttack(actor->GetFormID(), ++state.stage, kind, baseline, now, 0.0F, customCost);
    state.committed = false;
    state.denied = !result.accepted;
    state.nextStage = false;
    // A free counterattack refunds the engine's own power-attack drain for one
    // stage. The refund baseline is the stamina measured here, at the consuming
    // begin: the core re-arms the reward on every retry begin, so the binding is
    // refreshed per begin and the drop excludes stamina spent before this swing.
    // The refund fires at the first settle point that observes the drop.
    if (result.freeCounterattack) {
        state.freePowerPending = true;
        state.freePowerBaseline = baseline;
        state.freePowerStage = state.stage;
    }
    ObserveExhaustion(actor, result.exhausted, now);
    Trace(actor, result.accepted ? "admit" : "reject", state.stage, result.nominalCost, static_cast<int>(result.reason));
    return result.accepted;
}

// A bash begin: director cooldown gate first (NPCs), then the bash stamina
// cost rides the ordinary attack admission/commit path via customCost, so
// SCAR and the exhaustion gate still see a real debit.
bool BeginBash(RE::Actor* actor, TimeMs now) {
    std::scoped_lock lock(stateMutex);
    Prune(now);
    auto& state = State(actor, now);
    CancelPendingBash(actor, state);
    const auto actorId = actor->GetFormID();
    const float cost = config.aiSettings.bashStaminaCost;
    if (config.director && !actor->IsPlayerRef()) {
        const auto bash = aiRules.admitBash(actorId, now);
        if (!bash.allowed) {
            Trace(actor, "bash-denied-cooldown", state.stage,
                static_cast<float>(aiRules.bashCooldownRemaining(actorId, now)),
                static_cast<int>(bash.reason));
            state.denied = true;
            state.nextStage = true;
            return false;
        }
        if (cost > 0.0F && Stamina(actor) < cost) {
            aiRules.cancelBash(actorId, now);
            Trace(actor, "bash-denied-stamina", state.stage, cost);
            state.denied = true;
            state.nextStage = true;
            return false;
        }
        state.bashPending = true;
        state.bashRecordedAt = now;
    }
    if (cost <= 0.0F) {
        Trace(actor, "bash-admit", state.stage, 0.0F);
        return true;
    }
    const float baseline = Stamina(actor);
    // DualWield is the custom-cost kind; the nominal cost is the bash's.
    auto result = rules.beginAttack(actorId, ++state.stage, AttackKind::DualWield, baseline, now, 0.0F, cost);
    state.committed = false;
    state.denied = !result.accepted;
    state.nextStage = false;
    if (!result.accepted) CancelPendingBash(actor, state);
    ObserveExhaustion(actor, result.exhausted, now);
    Trace(actor, result.accepted ? "bash-admit" : "bash-reject", state.stage, result.nominalCost,
        static_cast<int>(result.reason));
    return result.accepted;
}

// Refunds the engine's power-attack drain for an armed free counterattack, once
// the drop is observed. Only the stage that spent the reward can settle, so
// light attacks and later stages cannot trigger the refund; stage zero means no
// consuming begin has bound the refund yet. Caller must hold stateMutex.
void SettleFreeCounterattack(RE::Actor* actor, ActorState& state, TimeMs now) {
    if (!state.freePowerPending || state.freePowerStage == 0 || state.stage != state.freePowerStage) return;
    const float drop = state.freePowerBaseline - Stamina(actor);
    if (drop <= 0.0F) return;
    state.freePowerPending = false;
    state.freePowerStage = 0;
    actor->AsActorValueOwner()->RestoreActorValue(RE::ActorValue::kStamina, drop);
    Trace(actor, "counterattack-refund", state.stage, drop);
}

bool Commit(RE::Actor* actor, TimeMs now) {
    std::scoped_lock lock(stateMutex);
    auto& state = State(actor, now);
    if (state.committed && !state.nextStage) return !state.denied;
    if (!state.stage || state.nextStage) {
        if (!Begin(actor, false, now)) return false;
    }
    if (state.denied) return false;
    const auto result = rules.commitAttack(actor->GetFormID(), state.stage, Stamina(actor), now);
    if (result.duplicate) return state.committed && !state.denied;
    state.denied = !result.accepted;
    if (!result.accepted) {
        Trace(actor, "reject-commit", state.stage, 0, static_cast<int>(result.reason));
        return false;
    }
    state.committed = true;
    state.nextStage = false;
    // The bash debit landed; the cooldown record now stands on its own.
    state.bashPending = false;
    state.bashRecordedAt = 0;
    if (result.staminaDebit > 0) {
        actor->AsActorValueOwner()->DamageActorValue(RE::ActorValue::kStamina, result.staminaDebit);
        auto* process = actor->GetActorRuntimeData().currentProcess;
        if (process) process->UpdateRegenDelay(RE::ActorValue::kStamina, config.regenDelaySeconds);
    }
    if (result.freeCounterattack) SettleFreeCounterattack(actor, state, now);
    ObserveExhaustion(actor, rules.observeStamina(actor->GetFormID(), Stamina(actor), now).exhausted, now);
    Trace(actor, result.engineOwnsCost ? "commit-engine-cost" : "commit", state.stage, result.staminaDebit);
    return true;
}

void StopRejectedAttack(RE::Actor* actor) {
    actor->NotifyAnimationGraph("attackStop");
    if (actor->IsPlayerRef()) RE::HUDMenu::FlashMeter(RE::ActorValue::kStamina);
}

void BlockEvent(RE::Actor* actor, bool blocking, TimeMs now) {
    if (!config.perfectBlock || !precisionReady) return;
    std::scoped_lock lock(stateMutex);
    State(actor, now);
    if (rules.setBlocking(actor->GetFormID(), blocking, now)) Trace(actor, "perfect-window", 0, 0);
}

bool IncomingEvent(RE::Actor* actor, std::string_view event) {
    const auto now = Now();
    if (event == "blockstartout" || event == "sbf_blockstart") BlockEvent(actor, true, now);
    else if (event == "blockstop" || event == "blockstopout" || event == "sbf_blockstop" ||
        event == "staggerstart" || event == "recoilstart" || event == "weaponsheathe") BlockEvent(actor, false, now);

    // A dodge starts its own regen delay regardless of admission scope.
    // Matches "*dodge*start*" style animation events (e.g. DMCO); no dodge charge.
    if (event.find("dodge") != std::string_view::npos && event.find("start") != std::string_view::npos) {
        auto* process = actor->GetActorRuntimeData().currentProcess;
        if (process) {
            process->UpdateRegenDelay(RE::ActorValue::kStamina, config.dodgeRegenDelaySeconds);
            Trace(actor, "dodge-delay", 0, config.dodgeRegenDelaySeconds);
        }
        return true;
    }

    if (!ManageAttack(actor) || Ranged(actor)) return true;
    if (event == "attackwinstart" || event == "mco_winopen") {
        std::scoped_lock lock(stateMutex);
        auto& state = State(actor, now);
        if (state.committed) state.nextStage = true;
        SettleFreeCounterattack(actor, state, now);
    } else if (event == "prehitframe" || event == "weaponswing" || event == "weaponleftswing") {
        if (!Commit(actor, now)) return false;
    } else if (event == "attackrelease") {
        // MCO release fires between commit and winstart; the engine's power-attack
        // debit can land here, so settle as soon as the drop is observable.
        std::scoped_lock lock(stateMutex);
        auto& state = State(actor, now);
        SettleFreeCounterattack(actor, state, now);
    } else if (event == "attackstop" || event == "staggerstart" || event == "recoilstart") {
        std::scoped_lock lock(stateMutex);
        auto& state = State(actor, now);
        CancelPendingBash(actor, state);
        rules.cancelAttack(actor->GetFormID(), state.stage, now);
        state.nextStage = true;
        SettleFreeCounterattack(actor, state, now);
    } else if (event == "attackend") {
        std::scoped_lock lock(stateMutex);
        auto& state = State(actor, now);
        SettleFreeCounterattack(actor, state, now);
        if (state.stage == state.freePowerStage) {
            state.freePowerPending = false;
            state.freePowerStage = 0;
        }
    }
    return true;
}

template<bool Player>
struct GraphHooks {
    using Holder = RE::IAnimationGraphManagerHolder;
    using Sink = RE::BSTEventSink<RE::BSAnimationGraphEvent>;
    using Source = RE::BSTEventSource<RE::BSAnimationGraphEvent>;
    static inline REL::Relocation<bool (*)(Holder*, const RE::BSFixedString&)> notify;
    static inline REL::Relocation<RE::BSEventNotifyControl (*)(Sink*, const RE::BSAnimationGraphEvent*, Source*)> process;

    static bool Notify(Holder* holder, const RE::BSFixedString& name) {
        auto* ref = static_cast<RE::TESObjectREFR*>(holder);
        auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
        if (!StableGameplay() || !EligibleActor(actor)) return notify(holder, name);
        const auto event = Lower(name.c_str());
        const auto now = Now();
        const bool request = ManageAttack(actor) && !Ranged(actor) && AttackRequest(event);
        const bool bash = ManageAttack(actor) && BashEvent(event);
        // Director: NPC melee attacks against the player need an attack slot.
        RE::Actor* slotTarget = nullptr;
        bool slotGranted = false;
        if (request && !actor->IsPlayerRef()) {
            slotTarget = DirectorTarget(actor);
            if (slotTarget) {
                const auto slot = [&] {
                    std::scoped_lock lock(stateMutex);
                    return aiRules.tryAcquire(slotTarget->GetFormID(), actor->GetFormID(), now, RandomUnit());
                }();
                if (!slot.granted) {
                    Trace(actor, "slot-denied", 0, 0, static_cast<int>(slot.reason));
                    return false;
                }
                slotGranted = true;
                if (slot.retaliating) Trace(actor, "slot-retaliate", 0, 0);
            }
        }
        if (bash && !BeginBash(actor, now)) {
            if (actor->IsPlayerRef()) RE::HUDMenu::FlashMeter(RE::ActorValue::kStamina);
            return false;
        }
        if (request && (actor->IsStaggered() || !Begin(actor, event.find("power") != std::string::npos, now))) {
            if (slotGranted) {
                std::scoped_lock lock(stateMutex);
                aiRules.release(slotTarget->GetFormID(), actor->GetFormID());
            }
            if (actor->IsPlayerRef()) RE::HUDMenu::FlashMeter(RE::ActorValue::kStamina);
            return false;
        }
        const bool result = notify(holder, name);
        if (result && (event == "blockstart" || event == "sbf_blockstart")) BlockEvent(actor, true, now);
        if (event == "blockstop" || event == "sbf_blockstop" || event == "staggerstart") BlockEvent(actor, false, now);
        if (request && !result) {
            std::scoped_lock lock(stateMutex);
            auto& state = State(actor, now);
            rules.cancelAttack(actor->GetFormID(), state.stage, now);
            state.nextStage = true;
            Trace(actor, "dispatch-failed", state.stage, 0);
        }
        // Director: the slot lives for its window regardless of how this swing
        // ended; expiry and rotation are handled lazily by the ledger. A bash
        // that failed to dispatch (rejected IDLE, supersede) restores the
        // cooldown exactly like a retry.
        if (bash && !result) {
            std::scoped_lock lock(stateMutex);
            auto& state = State(actor, now);
            CancelPendingBash(actor, state);
            rules.cancelAttack(actor->GetFormID(), state.stage, now);
            state.nextStage = true;
            Trace(actor, "bash-dispatch-failed", state.stage, 0);
        }
        return result;
    }

    static RE::BSEventNotifyControl Process(Sink* sink, const RE::BSAnimationGraphEvent* event, Source* source) {
        auto* actor = event && event->holder ? const_cast<RE::TESObjectREFR*>(event->holder)->As<RE::Actor>() : nullptr;
        if (StableGameplay() && EligibleActor(actor) && !IncomingEvent(actor, Lower(event->tag.c_str()))) {
            StopRejectedAttack(actor);
            return RE::BSEventNotifyControl::kContinue;
        }
        return process(sink, event, source);
    }

    static void Install() {
        REL::Relocation<std::uintptr_t> graphTable{Player ? RE::VTABLE_PlayerCharacter[3] : RE::VTABLE_Character[3]};
        REL::Relocation<std::uintptr_t> sinkTable{Player ? RE::VTABLE_PlayerCharacter[2] : RE::VTABLE_Character[2]};
        notify = graphTable.write_vfunc(0x01, Notify);
        process = sinkTable.write_vfunc(0x01, Process);
    }
};

// Per-frame Actor::Update hook (vtable slot 0xAD): drains stamina while a bow
// is drawn and keeps regen paused for the whole hold. Runs on the game thread;
// it reads no shared runtime state, so no lock is needed.
template<bool Player>
struct UpdateHooks {
    static inline REL::Relocation<void (*)(RE::Character*, float)> update;

    static void Hook(RE::Character* character, float delta) {
        update(character, delta);
        auto* actor = character ? static_cast<RE::Actor*>(character) : nullptr;
        if (delta <= 0.0F || !StableGameplay() || !EligibleActor(actor)) return;
        // Per-frame fallback for the free counterattack refund: the engine's
        // power-attack debit can land after every graph settle point. The map
        // is only mutated on the game thread, so the unlocked find is safe
        // here; the settle re-checks the pending state under the lock.
        {
            const auto found = actors.find(actor->GetFormID());
            if (found != actors.end() && found->second.freePowerPending) {
                std::scoped_lock lock(stateMutex);
                SettleFreeCounterattack(actor, found->second, Now());
            }
        }
        if (config.bowDrawDrainPerSecond <= 0.0F) return;
        auto* actorState = actor->AsActorState();
        const auto attack = actorState ? actorState->GetAttackState() : RE::ATTACK_STATE_ENUM::kNone;
        // kBowDraw/kBowAttached cover the pullback animation; kBowDrawn is the
        // full-draw hold. Release states deliberately do not drain.
        if (attack != RE::ATTACK_STATE_ENUM::kBowDraw &&
            attack != RE::ATTACK_STATE_ENUM::kBowAttached &&
            attack != RE::ATTACK_STATE_ENUM::kBowDrawn) return;
        const float drain = config.bowDrawDrainPerSecond * delta;
        actor->AsActorValueOwner()->DamageActorValue(RE::ActorValue::kStamina, drain);
        auto* process = actor->GetActorRuntimeData().currentProcess;
        if (process) process->UpdateRegenDelay(RE::ActorValue::kStamina, config.regenDelaySeconds);
        Trace(actor, "bow-draw", 0, drain);
    }

    static void Install() {
        REL::Relocation<std::uintptr_t> table{Player ? RE::VTABLE_PlayerCharacter[0] : RE::VTABLE_Character[0]};
        update = table.write_vfunc(0xAD, Hook);
    }
};

bool WithinBlockCone(RE::Actor* defender, RE::Actor* attacker) {
    const auto delta = attacker->GetPosition() - defender->GetPosition();
    const float distance = std::hypot(delta.x, delta.y);
    if (distance < 0.01F) return false;
    const float heading = defender->GetAngleZ();
    const float dot = (std::sin(heading) * delta.x + std::cos(heading) * delta.y) / distance;
    const float limit = std::cos(config.blockAngleDegrees * 0.5F * 0.017453292519943295F);
    return dot >= limit;
}

bool Unblockable(RE::Actor* attacker) {
    bool result = false;
    if (attacker->GetGraphVariableBool("bUnblockable", result) && result) return true;
    if (attacker->GetGraphVariableBool("isUnblockable", result) && result) return true;
    return false;
}

Core::PoiseKind PoiseKindFromWeapon(RE::Actor* attacker) {
    auto* entry = attacker->GetAttackingWeapon();
    auto* weapon = entry && entry->object ? entry->object->As<RE::TESObjectWEAP>() : nullptr;
    if (!weapon) return Core::PoiseKind::Unarmed;  // Creature and hand-to-hand contacts.
    switch (weapon->GetWeaponType()) {
    case RE::WEAPON_TYPE::kOneHandDagger: return Core::PoiseKind::Dagger;
    case RE::WEAPON_TYPE::kOneHandAxe: return Core::PoiseKind::Axe;
    case RE::WEAPON_TYPE::kOneHandMace: return Core::PoiseKind::Mace;
    case RE::WEAPON_TYPE::kTwoHandSword:
    case RE::WEAPON_TYPE::kTwoHandAxe: return Core::PoiseKind::TwoHanded;
    case RE::WEAPON_TYPE::kHandToHandMelee: return Core::PoiseKind::Unarmed;
    default: return Core::PoiseKind::Sword;
    }
}

// Publishes the poise pool through TrueHUD's special resource bar. The
// registered callbacks can run on the UI thread, so every read takes
// stateMutex (recursive) before touching the rules.
float PoiseResource(RE::Actor* actor, bool maximum) {
    if (!actor || !config.poise) return maximum ? poiseRules.settings().maxPoise : 0.0F;
    std::scoped_lock lock(stateMutex);
    if (maximum) return poiseRules.settings().maxPoise;
    return poiseRules.poise(actor->GetFormID(), Now());
}

void FlashSpecialBar(RE::Actor* actor, bool longFlash) {
    if (trueHudReady && actor) trueHud->FlashActorSpecialBar(SKSE::GetPluginHandle(), actor->GetHandle(), longFlash);
}

void FlashPoiseResult(RE::Actor* actor, const Core::PoiseResult& result) {
    if (result.broke || result.applied) FlashSpecialBar(actor, result.broke);
}

// Claims TrueHUD's special resource bar for the poise pool. Fails without
// side effects when TrueHUD is absent or another mod already owns the bars.
bool ConnectTrueHud() {
    auto* api = static_cast<TRUEHUD_API::IVTrueHUD1*>(
        TRUEHUD_API::RequestPluginAPI(TRUEHUD_API::InterfaceVersion::V1));
    if (!api) { SKSE::log::warn("Poise HUD enabled but the TrueHUD API is unavailable."); return false; }
    trueHud = api;
    const auto control = api->RequestSpecialResourceBarsControl(SKSE::GetPluginHandle());
    if (control != TRUEHUD_API::APIResult::OK) {
        SKSE::log::warn("Poise HUD could not take special bar control (result {}). "
            "Another poise mod may own the bars.", static_cast<int>(control));
        return false;
    }
    trueHudReady = api->RegisterSpecialResourceFunctions(SKSE::GetPluginHandle(),
        [](RE::Actor* actor) { return PoiseResource(actor, false); },
        [](RE::Actor* actor) { return PoiseResource(actor, true); },
        true, true) == TRUEHUD_API::APIResult::OK;
    if (!trueHudReady) SKSE::log::warn("Poise HUD special bar registration failed.");
    return trueHudReady;
}

// Resolves a "Plugin.esp|0xFormID" spec to a sound descriptor. Empty specs
// disable feedback silently; unresolvable specs warn once and return null.
RE::BGSSoundDescriptorForm* ResolveConfiguredSound(std::string_view spec, std::string_view label) {
    if (spec.empty()) return nullptr;
    const auto pipe = spec.find('|');
    if (pipe == std::string_view::npos || pipe == 0 || pipe + 1 >= spec.size()) {
        SKSE::log::warn("Invalid {} sound spec '{}'; expected Plugin.esp|0xFormID.", label, spec);
        return nullptr;
    }
    const auto plugin = spec.substr(0, pipe);
    const auto idText = spec.substr(pipe + 1);
    unsigned long raw = 0;
    try {
        raw = std::stoul(std::string(idText), nullptr, 16);
    } catch (...) {
        SKSE::log::warn("Invalid {} sound spec '{}'; FormID is not hex.", label, spec);
        return nullptr;
    }
    if (raw > 0xFFFFFF) {
        SKSE::log::warn("Invalid {} sound spec '{}'; FormID exceeds 0xFFFFFF.", label, spec);
        return nullptr;
    }
    auto* data = RE::TESDataHandler::GetSingleton();
    auto* form = data ? data->LookupForm(static_cast<RE::FormID>(raw), plugin) : nullptr;
    auto* descriptor = form ? form->As<RE::BGSSoundDescriptorForm>() : nullptr;
    if (!descriptor) {
        SKSE::log::warn("{} sound '{}' did not resolve to a sound record.", label, spec);
        return nullptr;
    }
    return descriptor;
}

// Maps the defender's blocking gear to its 3D attachment node, mirroring the
// engine's biped object slots. Returns null when no usable node is loaded.
RE::NiAVObject* BlockEffectNode(RE::Actor* defender, bool& shielded) {
    if (!defender) return nullptr;
    const auto& biped = defender->GetCurrentBiped();
    if (!biped) return nullptr;
    auto pick = [&](RE::BIPED_OBJECT slot) -> RE::NiAVObject* {
        if (slot == RE::BIPED_OBJECTS::kNone) return nullptr;
        auto& part = biped->objects[slot].partClone;
        return part ? part.get() : nullptr;
    };
    // A shield in the left hand always owns the block.
    if (auto* left = defender->GetEquippedObject(true); left && left->IsArmor()) {
        shielded = true;
        if (auto* node = pick(RE::BIPED_OBJECTS::kShield)) return node;
    }
    if (auto* right = defender->GetEquippedObject(false)) {
        if (auto* weapon = right->As<RE::TESObjectWEAP>()) {
            using WT = RE::WEAPON_TYPE;
            RE::BIPED_OBJECT slot = RE::BIPED_OBJECTS::kNone;
            switch (weapon->GetWeaponType()) {
                case WT::kOneHandSword:  slot = RE::BIPED_OBJECTS::kOneHandSword;  break;
                case WT::kOneHandAxe:   slot = RE::BIPED_OBJECTS::kOneHandAxe;   break;
                case WT::kOneHandMace:  slot = RE::BIPED_OBJECTS::kOneHandMace;  break;
                case WT::kOneHandDagger: slot = RE::BIPED_OBJECTS::kOneHandDagger; break;
                case WT::kTwoHandAxe:
                case WT::kTwoHandSword:
                case WT::kHandToHandMelee: slot = RE::BIPED_OBJECTS::kTwoHandMelee; break;
                default: break;
            }
            shielded = false;
            if (auto* node = pick(slot)) return node;
        }
    }
    // Dual-wield/spell-blade blocks land on the left weapon.
    if (auto* left = defender->GetEquippedObject(true)) {
        if (auto* weapon = left->As<RE::TESObjectWEAP>()) {
            shielded = false;
            using WT = RE::WEAPON_TYPE;
            switch (weapon->GetWeaponType()) {
                case WT::kOneHandSword:  if (auto* n = pick(RE::BIPED_OBJECTS::kOneHandSword)) return n;  break;
                case WT::kOneHandAxe:   if (auto* n = pick(RE::BIPED_OBJECTS::kOneHandAxe)) return n;   break;
                case WT::kOneHandMace:  if (auto* n = pick(RE::BIPED_OBJECTS::kOneHandMace)) return n;  break;
                case WT::kOneHandDagger: if (auto* n = pick(RE::BIPED_OBJECTS::kOneHandDagger)) return n; break;
                default: break;
            }
        }
    }
    return nullptr;
}

void PlayParryFeedback(RE::Actor* attacker, RE::Actor* defender) {
    if (!defender->IsPlayerRef() && !attacker->IsPlayerRef()) return;
    bool shielded = defender->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kShield) != nullptr;
    auto* node = BlockEffectNode(defender, shielded);
    const auto position = node ? node->worldBound.center : defender->GetPosition();

    // Positioned 3D playback: the clang originates from the blocking weapon or
    // shield, not the listener, so distance and occlusion behave naturally.
    auto* descriptor = shielded ? parrySoundShieldDesc : parrySoundWeaponDesc;
    if (descriptor) {
        if (auto* audio = RE::BSAudioManager::GetSingleton()) {
            RE::BSSoundHandle handle{};
            if (audio->GetSoundHandle(handle, descriptor, 0x16)) {
                handle.SetPosition(position);
                handle.SetVolume(parrySoundVolume);
                handle.Play();
            }
        }
    }

    if (!parryVisual || !node) return;
    auto* cell = defender->GetParentCell();
    if (!cell) return;
    // Engine-driven spark: attaching the FXMetalSparkImpact01 root to the
    // blocking weapon/shield node makes Skyrim fire its own vanilla metal-spark
    // impact at that node, exactly like a real block. Same recipe Elden Parry
    // uses; no custom spark/flare NIFs required.
    const auto& rotate = node->world.rotate;
    RE::BSTempEffectParticle::Spawn(cell, 0.0f,
        shielded ? "ApexCombat\\impactShieldRoot.nif" : "ApexCombat\\impactWeaponRoot.nif",
        rotate, position, 1.0f, 4U, node);
}

// Locks stateMutex itself (recursive). Returns the poise resolution; the
// stagger notification must be sent after lock release.
Core::PoiseResult ApplyDefenderPoise(RE::Actor* attacker, RE::Actor* defender, bool blocked, TimeMs now) {
    if (!config.poise) return {};
    std::scoped_lock lock(stateMutex);
    // Attacks inside an active counterattack window deal bonus poise damage.
    const auto attackerId = attacker->GetFormID();
    const float multiplier = rules.counterattackActive(attackerId, now)
        ? poiseRules.settings().counterattackPoiseMultiplier
        : 1.0F;
    const auto result = poiseRules.applyDamage(defender->GetFormID(), PoiseKindFromWeapon(attacker),
        attacker->IsPowerAttacking(), blocked ? Core::PoiseSource::BlockedHit : Core::PoiseSource::Hit,
        now, multiplier);
    Trace(defender, result.broke ? "poise-break" : "poise-damage", 0, result.poiseAfter,
        static_cast<int>(result.reason));
    return result;
}

PRECISION_API::PreHitCallbackReturn PreHit(const PRECISION_API::PrecisionHitData& hit) {
    PRECISION_API::PreHitCallbackReturn output;
    if (!StableGameplay()) return output;
    auto* attacker = hit.attacker;
    auto* defender = hit.target ? hit.target->As<RE::Actor>() : nullptr;
    if (!EligibleActor(attacker) || !EligibleActor(defender) || attacker == defender) return output;
    const auto now = Now();
    Core::EventToken strike = 0;
    {
        std::scoped_lock lock(stateMutex);
        auto& state = State(attacker, now);
        if (ManageAttack(attacker) && !Ranged(attacker)) {
            if (state.denied || (!state.committed && !Commit(attacker, now))) {
                output.bIgnoreHit = true;
                Trace(attacker, "suppress-rejected-hit", state.stage, 0);
                return output;
            }
        }
        // Only a committed, identified strike can own parry-contact immunity.
        strike = state.committed ? state.stage : Core::EventToken{0};
    }
    if (!config.perfectBlock || (!defender->IsPlayerRef() && !config.npcPerfectBlock) ||
        !defender->IsBlocking() || defender->IsStaggered() || Unblockable(attacker) ||
        !WithinBlockCone(defender, attacker)) {
        // Director: the target (defender) damaging a circling attacker opens
        // that attacker's retaliation window. Only the player's hits arm it
        // when playerOnly is on, matching DirectorTarget's scope.
        if (config.director && !defender->IsPlayerRef() &&
            (attacker->IsPlayerRef() || !config.directorPlayerOnly)) {
            std::scoped_lock lock(stateMutex);
            aiRules.onTargetDamagedAttacker(attacker->GetFormID(), defender->GetFormID(), now);
        }
        const auto poise = ApplyDefenderPoise(attacker, defender, false, now);
        if (poise.broke) defender->NotifyAnimationGraph("staggerStart");
        FlashPoiseResult(defender, poise);
        return output;
    }
    bool parried = false;
    Core::PoiseResult attackerPoise;
    Core::PoiseResult defenderPoise;
    {
        std::scoped_lock lock(stateMutex);
        auto& attackerState = State(attacker, now);
        auto& defenderState = State(defender, now);
        // Additional colliders from a strike that already parried this defender
        // are ignored; ordinary contacts never create immunity.
        if (strike && attackerState.contacts.contains(strike, defender->GetFormID())) {
            output.bIgnoreHit = true;
            Trace(attacker, "suppress-parried-contact", strike, 0);
            return output;
        }
        auto result = rules.successfulBlock(defender->GetFormID(), ++defenderState.hit,
            Stamina(defender), MaximumStamina(defender), now);
        ObserveExhaustion(defender, result.exhausted, now);
        if (!result.perfect) {
            // An ordinary block still chips poise at the blocked fraction.
            defenderPoise = ApplyDefenderPoise(attacker, defender, true, now);
        } else if (strike && !attackerState.contacts.remember(strike, defender->GetFormID())) {
            // A duplicate contact for the same strike/defender cannot refund twice.
            output.bIgnoreHit = true;
            Trace(attacker, "duplicate-parry-contact", strike, 0);
            return output;
        } else {
            output.bIgnoreHit = true;
            parried = true;
            if (result.staminaRestore > 0) defender->AsActorValueOwner()->RestoreActorValue(
                RE::ActorValue::kStamina, result.staminaRestore);
            // The reward refund anchors here: this is the last guaranteed
            // pre-drain observation before the counterattack power swing.
            if (rules.grantCounterattack(defender->GetFormID(), now)) {
                defenderState.freePowerPending = true;
                defenderState.freePowerBaseline = Stamina(defender);
                defenderState.freePowerStage = 0;
                Trace(defender, "counterattack-grant", defenderState.hit, 0);
            }
            Trace(defender, "perfect-parry", defenderState.hit, result.staminaRestore);
            if (config.poise) {
                attackerPoise = poiseRules.applyDamage(attacker->GetFormID(), PoiseKindFromWeapon(attacker),
                    attacker->IsPowerAttacking(), Core::PoiseSource::PerfectParry, now);
                Trace(attacker, attackerPoise.broke ? "poise-break" : "poise-parry", defenderState.hit,
                    attackerPoise.poiseAfter, static_cast<int>(attackerPoise.reason));
            }
        }
    }
    // Feedback is sent after releasing the rules lock: graph callbacks can re-enter.
    if (parried) PlayParryFeedback(attacker, defender);
    defender->NotifyAnimationGraph("Apex_PerfectParry");
    attacker->NotifyAnimationGraph(attackerPoise.broke ? "staggerStart" : "recoilStart");
    if (defenderPoise.broke) defender->NotifyAnimationGraph("staggerStart");
    FlashPoiseResult(attacker, attackerPoise);
    FlashPoiseResult(defender, defenderPoise);
    return output;
}

void AddStaminaCondition(RE::TESIdleForm* idle) {
    auto* condition = new RE::TESConditionItem();
    condition->data.functionData.function = RE::FUNCTION_DATA::FunctionID::kGetActorValue;
    condition->data.functionData.params[0] = reinterpret_cast<void*>(static_cast<std::uintptr_t>(RE::ActorValue::kStamina));
    condition->data.comparisonValue.f = 0.0F;
    condition->data.flags.opCode = RE::CONDITION_ITEM_DATA::OpCode::kGreaterThan;
    condition->data.object = RE::CONDITIONITEMOBJECT::kSelf;
    // Prepend an AND condition so an existing terminal OR chain cannot absorb it.
    condition->next = idle->conditions.head;
    idle->conditions.head = condition;
}

void PatchAttackIdles() {
    if (!config.patchAttackIdles || !config.playerAttacks || !config.npcAttacks) return;
    auto* data = RE::TESDataHandler::GetSingleton();
    std::size_t count = 0;
    for (auto* idle : data->GetFormArray<RE::TESIdleForm>()) {
        if (!idle) continue;
        const auto event = Lower(idle->animEventName.c_str());
        const auto editor = Lower(idle->GetFormEditorID());
        const bool root = idle->GetFormID() == 0x13211 || idle->GetFormID() == 0x13216;
        if (root || AttackRequest(event) || editor.starts_with("adxp_npcnormalattack") ||
            editor.starts_with("adxp_npcpowerattack") || editor == "bfco_m1releaseroot_byskse") {
            AddStaminaCondition(idle);
            ++count;
        }
    }
    SKSE::log::info("Stamina conditions added to {} attack IDLEs", count);
}

class LifecycleEvents final : public RE::BSTEventSink<RE::TESObjectLoadedEvent>, public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
    RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent* event, RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override {
        if (event && !event->loaded) {
            std::scoped_lock lock(stateMutex);
            rules.resetActor(event->formID);
            poiseRules.resetActor(static_cast<Core::ActorId>(event->formID));
            aiRules.resetActor(static_cast<Core::ActorId>(event->formID));
            actors.erase(event->formID);
        }
        return RE::BSEventNotifyControl::kContinue;
    }
    RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
        if (event && event->opening) {
            if (event->menuName == RE::MainMenu::MENU_NAME) ResetSession();
            else if (event->menuName == RE::LoadingMenu::MENU_NAME) {
                std::scoped_lock lock(stateMutex);
                rules.resetSession();
                poiseRules.resetSession();
                aiRules.resetSession();
                actors.clear();
            }
            else {
                std::scoped_lock lock(stateMutex);
                for (auto& [id, state] : actors) { rules.setBlocking(id, false, Now()); }
            }
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};
LifecycleEvents lifecycle;
}

bool InitializeRuntime() {
    if (initialized) return true;
    auto settings = readConfiguration("Data/SKSE/Plugins/ApexCombat.ini");
    config = settings.config;
    for (const auto& diagnostic : settings.diagnostics) SKSE::log::warn("{}", diagnostic);
    rules.setSettings(config.rules);
    if (config.poise) poiseRules.setSettings(config.poiseSettings);
    aiRules.setSettings(config.aiSettings);
    if (!config.enabled) { SKSE::log::info("Disabled in configuration."); return true; }
    GraphHooks<false>::Install();
    GraphHooks<true>::Install();
    UpdateHooks<false>::Install();
    UpdateHooks<true>::Install();
    PatchAttackIdles();
    if (auto* scripts = RE::ScriptEventSourceHolder::GetSingleton()) scripts->AddEventSink<RE::TESObjectLoadedEvent>(&lifecycle);
    if (auto* ui = RE::UI::GetSingleton()) ui->AddEventSink<RE::MenuOpenCloseEvent>(&lifecycle);
    auto* api = static_cast<PRECISION_API::IVPrecision1*>(PRECISION_API::RequestPluginAPI(PRECISION_API::InterfaceVersion::V1));
    if (api) precisionReady = api->AddPreHitCallback(SKSE::GetPluginHandle(), PreHit) == PRECISION_API::APIResult::OK;
    if (config.poise && config.poiseHud) ConnectTrueHud();
    if (config.perfectBlock) {
        parrySoundWeaponDesc = ResolveConfiguredSound(config.parrySoundWeapon, "weapon parry");
        parrySoundShieldDesc = ResolveConfiguredSound(config.parrySoundShield, "shield parry");
        parrySoundVolume = config.parrySoundVolume;
        parryVisual = config.parryVisual;
    }
    SKSE::log::info("Capabilities: graph stamina=true; attack IDLE gates={}; Precision melee parry={}; "
        "bow draw drain={}; poise={}; TrueHUD poise bar={}; parry sfx={}; parry vfx={}; director={}; VR={}",
        config.patchAttackIdles, precisionReady, config.bowDrawDrainPerSecond, config.poise,
        trueHudReady, parrySoundWeaponDesc != nullptr || parrySoundShieldDesc != nullptr,
        parryVisual, config.director, REL::Module::IsVR());
    if (!precisionReady) SKSE::log::warn("Perfect blocking requires a compatible Precision API. Stamina features remain available.");
    if (config.poise && config.poiseHud && !trueHudReady)
        SKSE::log::warn("Poise is enabled but the TrueHUD poise bar is unavailable. Poise rules still apply.");
    initialized = true;
    return true;
}

void ResetSession() {
    active = false;
    std::scoped_lock lock(stateMutex);
    rules.resetSession();
    poiseRules.resetSession();
    aiRules.resetSession();
    actors.clear();
}

void StartSession() {
    ResetSession();
    active = initialized && config.enabled;
    SKSE::log::info("Combat session ready={}", active.load());
}
}
