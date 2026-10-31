#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <REX/W32/KERNEL32.h>
#include <Windows.h>
#include "Runtime.h"
#include "core/CombatRules.h"
#include "core/Configuration.h"
#include "core/ParryContacts.h"
#include "precision/PrecisionAPI.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
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
std::recursive_mutex stateMutex;
std::atomic_bool active{false};
bool initialized{false};
bool precisionReady{false};

struct ActorState {
    EventToken stage{0};
    EventToken hit{0};
    bool committed{false};
    bool denied{false};
    bool nextStage{true};
    TimeMs lastSeen{0};
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

bool Ranged(RE::Actor* actor) {
    auto* form = actor->GetEquippedObject(false);
    auto* weapon = form ? form->As<RE::TESObjectWEAP>() : nullptr;
    if (!weapon) return false;
    auto type = weapon->GetWeaponType();
    return type == RE::WEAPON_TYPE::kBow || type == RE::WEAPON_TYPE::kCrossbow || type == RE::WEAPON_TYPE::kStaff;
}

AttackKind Kind(RE::Actor* actor, bool powerHint = false) {
    if (powerHint || actor->IsPowerAttacking()) return AttackKind::Power;
    auto* entry = actor->GetAttackingWeapon();
    auto* weapon = entry && entry->object ? entry->object->As<RE::TESObjectWEAP>() : nullptr;
    if (!weapon) {
        auto* form = actor->GetEquippedObject(false);
        weapon = form ? form->As<RE::TESObjectWEAP>() : nullptr;
    }
    if (!weapon || weapon->GetWeaponType() == RE::WEAPON_TYPE::kHandToHandMelee) return AttackKind::Unarmed;
    if (weapon->GetWeaponType() == RE::WEAPON_TYPE::kTwoHandSword || weapon->GetWeaponType() == RE::WEAPON_TYPE::kTwoHandAxe)
        return AttackKind::LightTwoHanded;
    return AttackKind::LightOneHanded;
}

bool AttackRequest(std::string_view event) {
    if (event == "attackstop" || event == "attackend" || event == "attackrelease") return false;
    return event.starts_with("attackstart") || event.starts_with("attackpowerstart") ||
        event.starts_with("bfcoattackstart") || event.starts_with("bfco_powerattackstart") ||
        event.starts_with("mco_attackstart") || event.starts_with("mco_powerattackstart");
}

bool Begin(RE::Actor* actor, bool power, TimeMs now) {
    std::scoped_lock lock(stateMutex);
    Prune(now);
    auto& state = State(actor, now);
    // Power attack costs remain owned by the game. Positive stamina is required.
    auto result = rules.beginAttack(actor->GetFormID(), ++state.stage, Kind(actor, power), Stamina(actor), now);
    state.committed = false;
    state.denied = !result.accepted;
    state.nextStage = false;
    Trace(actor, result.accepted ? "admit" : "reject", state.stage, result.nominalCost, static_cast<int>(result.reason));
    return result.accepted;
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
    if (result.staminaDebit > 0) {
        actor->AsActorValueOwner()->DamageActorValue(RE::ActorValue::kStamina, result.staminaDebit);
        auto* process = actor->GetActorRuntimeData().currentProcess;
        if (process) process->UpdateRegenDelay(RE::ActorValue::kStamina, config.regenDelaySeconds);
    }
    rules.observeStamina(actor->GetFormID(), Stamina(actor), now);
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

    if (!ManageAttack(actor) || Ranged(actor)) return true;
    if (event == "attackwinstart" || event == "mco_winopen") {
        std::scoped_lock lock(stateMutex);
        auto& state = State(actor, now);
        if (state.committed) state.nextStage = true;
    } else if (event == "prehitframe" || event == "weaponswing" || event == "weaponleftswing") {
        if (!Commit(actor, now)) return false;
    } else if (event == "attackstop" || event == "staggerstart" || event == "recoilstart") {
        std::scoped_lock lock(stateMutex);
        auto& state = State(actor, now);
        rules.cancelAttack(actor->GetFormID(), state.stage, now);
        state.nextStage = true;
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
        if (request && (actor->IsStaggered() || !Begin(actor, event.find("power") != std::string::npos, now))) {
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
        !WithinBlockCone(defender, attacker)) return output;
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
        if (!result.perfect) return output;
        if (strike && !attackerState.contacts.remember(strike, defender->GetFormID())) {
            // A duplicate contact for the same strike/defender cannot refund twice.
            output.bIgnoreHit = true;
            Trace(attacker, "duplicate-parry-contact", strike, 0);
            return output;
        }
        output.bIgnoreHit = true;
        if (result.staminaRestore > 0) defender->AsActorValueOwner()->RestoreActorValue(
            RE::ActorValue::kStamina, result.staminaRestore);
        Trace(defender, "perfect-parry", defenderState.hit, result.staminaRestore);
    }
    // Feedback is sent after releasing the rules lock: graph callbacks can re-enter.
    defender->NotifyAnimationGraph("Apex_PerfectParry");
    attacker->NotifyAnimationGraph("recoilStart");
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
    if (!config.enabled) { SKSE::log::info("Disabled in configuration."); return true; }
    GraphHooks<false>::Install();
    GraphHooks<true>::Install();
    PatchAttackIdles();
    if (auto* scripts = RE::ScriptEventSourceHolder::GetSingleton()) scripts->AddEventSink<RE::TESObjectLoadedEvent>(&lifecycle);
    if (auto* ui = RE::UI::GetSingleton()) ui->AddEventSink<RE::MenuOpenCloseEvent>(&lifecycle);
    auto* api = static_cast<PRECISION_API::IVPrecision1*>(PRECISION_API::RequestPluginAPI(PRECISION_API::InterfaceVersion::V1));
    if (api) precisionReady = api->AddPreHitCallback(SKSE::GetPluginHandle(), PreHit) == PRECISION_API::APIResult::OK;
    SKSE::log::info("Capabilities: graph stamina=true; attack IDLE gates={}; Precision melee parry={}; VR={}",
        config.patchAttackIdles, precisionReady, REL::Module::IsVR());
    if (!precisionReady) SKSE::log::warn("Perfect blocking requires a compatible Precision API. Stamina features remain available.");
    initialized = true;
    return true;
}

void ResetSession() {
    active = false;
    std::scoped_lock lock(stateMutex);
    rules.resetSession();
    actors.clear();
}

void StartSession() {
    ResetSession();
    active = initialized && config.enabled;
    SKSE::log::info("Combat session ready={}", active.load());
}
}
