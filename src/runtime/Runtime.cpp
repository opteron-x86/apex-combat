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
#include "core/PotionRules.h"
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
#include <vector>

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
bool precisionPostHitReady{false};
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
    TimeMs freePowerDeadline{0};
    TimeMs lastSeen{0};
    // Director: a recorded bash whose commit never landed must be cancelled
    // (rejected/superseded begin), restoring the cooldown. bashStage is the
    // admission token of the bash's own engine-cost stage; dispatch failure cancels exactly
    // this token and never an interleaved attack's stage.
    bool bashPending{false};
    TimeMs bashRecordedAt{0};
    EventToken bashStage{0};
    // Director shield-turtle: while circling, this NPC holds block and
    // strafes around its target until an opening (recovery, stagger,
    // exhaustion, retaliation) or the max-circle timeout.
    bool turtleCircling{false};
    TimeMs turtleCircleSince{0};
    bool turtleClockwise{false};
    float turtleDirection{0.0F};  // actor-local degrees of the last strafe write
    TimeMs directorLastTick{0};
    RE::FormID directorTarget{0};
    bool turtleRaisedBlock{false};
    // Only copied engine-effect data is read by graph/Precision callbacks.
    bool potionActive{false};
    float potionPercent{0.0F};
    std::uint64_t potionRequest{0};
    int pendingPotionTier{Core::kTierNone};
    bool potionCastAttempted{false};
    float potionConfirmationSeconds{0.0F};
    Core::ParryContacts contacts;
};
std::unordered_map<RE::FormID, ActorState> actors;
TimeMs lastPrune{0};

void TurtleStop(RE::Actor* actor, ActorState& state);
void StopDirector(RE::Actor* actor, ActorState& state);
void StopAllDirectors();

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
            TurtleStop(RE::TESForm::LookupByID<RE::Actor>(it->first), it->second);
            rules.resetActor(it->first);
            poiseRules.resetActor(static_cast<Core::ActorId>(it->first));
            aiRules.resetActor(static_cast<Core::ActorId>(it->first));
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

// Engine effect access stays on Actor::Update or the SKSE main-thread task.
// Combat callbacks can run elsewhere and read only this protected snapshot.
float ActiveCostMultiplier(RE::Actor* actor, TimeMs) {
    std::scoped_lock lock(stateMutex);
    const auto found = actors.find(actor->GetFormID());
    if (!config.potionSettings.enabled || found == actors.end() || !found->second.potionActive) return 1.0F;
    return Core::costMultiplier(found->second.potionPercent);
}

struct PotionForms {
    RE::BGSKeyword* keyword{nullptr};
    RE::EffectSetting* receipt{nullptr};
    RE::EffectSetting* buff{nullptr};
    RE::SpellItem* spell{nullptr};
};
PotionForms potionForms[6]{};
bool potionFormsReady{false};
std::uint64_t potionSession{0};
std::uint64_t nextPotionRequest{0};

// Skyrim retains keyword EDIDs. Other forms may return an empty EDID without
// an EDID-preservation plugin; their exact local IDs and structural links are
// still checked, and a retained but unexpected EDID is rejected.
bool PotionEditorIDMatches(RE::TESForm* form, const std::string& expected, bool required = false) {
    const char* id = form ? form->GetFormEditorID() : nullptr;
    return form && (id && *id ? expected == id : !required);
}

int ResolvePotionForms() {
    auto* data = RE::TESDataHandler::GetSingleton();
    int found = 0;
    constexpr auto plugin = "ApexCombat - Potions.esp";
    for (int tier = 0; tier < 6; ++tier) {
        auto& forms = potionForms[tier];
        if (!data) continue;
        forms.keyword = data->LookupForm<RE::BGSKeyword>(0x800 + tier, plugin);
        forms.receipt = data->LookupForm<RE::EffectSetting>(0x806 + tier, plugin);
        forms.buff = data->LookupForm<RE::EffectSetting>(0x80C + tier, plugin);
        forms.spell = data->LookupForm<RE::SpellItem>(0x812 + tier, plugin);
        const std::string suffix = Core::kTierNames[tier];
        const bool valid =
            PotionEditorIDMatches(forms.keyword, std::string(Core::kPotionKeywordPrefix) + suffix, true) &&
            PotionEditorIDMatches(forms.receipt, "ApexPotionDiscount" + suffix) &&
            PotionEditorIDMatches(forms.buff, "ApexPotionBuff" + suffix) &&
            PotionEditorIDMatches(forms.spell, "ApexPotionBuffSpell" + suffix) &&
            forms.receipt->GetArchetype() == RE::EffectSetting::Archetype::kScript &&
            forms.buff->GetArchetype() == RE::EffectSetting::Archetype::kScript &&
            forms.spell->GetSpellType() == RE::MagicSystem::SpellType::kSpell &&
            forms.spell->GetCastingType() == RE::MagicSystem::CastingType::kFireAndForget &&
            forms.spell->GetDelivery() == RE::MagicSystem::Delivery::kSelf &&
            forms.spell->effects.size() == 1 && forms.spell->effects[0] &&
            forms.spell->effects[0]->baseEffect == forms.buff;
        if (valid) ++found;
        else forms = {};
    }
    // An old keyword-only patch must not quietly enable a timer without a HUD.
    potionFormsReady = found == 6;
    if (!potionFormsReady || !config.potionSettings.enabled) return found;
    for (int tier = 0; tier < 6; ++tier) {
        const auto discount = Core::discountForTier(config.potionSettings, tier);
        auto& item = potionForms[tier].spell->effects[0]->effectItem;
        item.magnitude = discount.percent;
        item.duration = discount.seconds;
    }
    std::vector<RE::AlchemyItem*> potions;
    {
        const auto [forms, formLock] = RE::TESForm::GetAllForms();
        const RE::BSReadLockGuard guard{formLock};
        if (forms) {
            for (const auto& [id, form] : *forms)
                if (form) if (auto* potion = form->As<RE::AlchemyItem>()) potions.push_back(potion);
        }
    }
    std::size_t synchronized = 0;
    for (auto* potion : potions) {
        for (auto* effect : potion->effects) {
            if (!effect) continue;
            for (int tier = 0; tier < 6; ++tier) {
                if (effect->baseEffect != potionForms[tier].receipt) continue;
                const auto discount = Core::discountForTier(config.potionSettings, tier);
                effect->effectItem.magnitude = discount.percent;
                effect->effectItem.duration = discount.seconds;
                ++synchronized;
                break;
            }
        }
    }
    SKSE::log::info("Potion spell and inventory descriptions synchronized to INI; receipt effects={}", synchronized);
    return found;
}

bool PotionGameplaySession() {
    auto* ui = RE::UI::GetSingleton();
    // Drinking through paused inventory/favorites menus is valid.
    return active.load() && ui && !ui->IsMenuOpen(RE::MainMenu::MENU_NAME) &&
        !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
}

void ClearPendingPotion(ActorState& state) {
    state.pendingPotionTier = Core::kTierNone;
    state.potionCastAttempted = false;
    state.potionConfirmationSeconds = 0.0F;
}

// Main-thread only. VisitActiveEffects uses the engine-owned walk on SE/AE/VR.
// Dispel flags exact owned receipts in the visitor; no pointers escape it.
void SynchronizePotionEffect(RE::Actor* actor, float unpausedDelta = 0.0F) {
    if (!potionFormsReady || !config.potionSettings.enabled || !actor) return;
    bool present = false;
    float percent = 0.0F;
    float newestElapsed = 0.0F;
    int liveTier = Core::kTierNone;
    actor->AsMagicTarget()->VisitActiveEffects([&](RE::ActiveEffect* effect) {
        if (!effect || effect->flags.any(RE::ActiveEffect::Flag::kDispelled))
            return RE::BSContainer::ForEachResult::kContinue;
        auto* base = effect->GetBaseObject();
        for (int tier = 0; tier < 6; ++tier) {
            const auto& forms = potionForms[tier];
            if (base == forms.receipt) {
                effect->Dispel(true);
                break;
            }
            if (base != forms.buff || effect->spell != forms.spell) continue;
            if (effect->flags.any(RE::ActiveEffect::Flag::kInactive) ||
                !std::isfinite(effect->magnitude) || !std::isfinite(effect->duration) ||
                !std::isfinite(effect->elapsedSeconds) || effect->magnitude < 0.0F ||
                effect->duration <= 0.0F || effect->elapsedSeconds < 0.0F ||
                effect->elapsedSeconds >= effect->duration) break;
            // Replacement normally leaves one live effect. Never add multiple
            // magnitudes if another plugin has supplied duplicate instances.
            if (!present || effect->elapsedSeconds < newestElapsed) {
                present = true;
                percent = std::clamp(effect->magnitude, 0.0F, 100.0F);
                newestElapsed = effect->elapsedSeconds;
                liveTier = tier;
            }
            break;
        }
        return RE::BSContainer::ForEachResult::kContinue;
    });
    std::scoped_lock lock(stateMutex);
    auto found = actors.find(actor->GetFormID());
    if (!present && found == actors.end()) return;
    auto& state = State(actor, Now());
    state.potionActive = present;
    state.potionPercent = percent;
    if (!state.potionCastAttempted) return;
    if (present && liveTier == state.pendingPotionTier) {
        Trace(actor, "potion-buff-active", state.potionRequest, percent, liveTier);
        ClearPendingPotion(state);
    } else if (std::isfinite(unpausedDelta) && unpausedDelta > 0.0F) {
        state.potionConfirmationSeconds += unpausedDelta;
        if (state.potionConfirmationSeconds >= 2.0F) {
            SKSE::log::warn("Potion buff did not become active: actor={:08X}, tier={}, request={}",
                actor->GetFormID(), state.pendingPotionTier, state.potionRequest);
            ClearPendingPotion(state);
        }
    }
}

// TESEquip reserves only a request. It never grants a discount itself. The
// latest token wins even when several doses are drunk before the task runs.
void QueuePotionReplacement(RE::Actor* actor, int tier) {
    if (!potionFormsReady || !config.potionSettings.enabled) return;
    auto* tasks = SKSE::GetTaskInterface();
    if (!tasks) {
        SKSE::log::warn("Potion replacement unavailable: SKSE task interface missing.");
        return;
    }
    const auto handle = actor->GetHandle();
    const auto id = actor->GetFormID();
    std::uint64_t request;
    std::uint64_t session;
    {
        std::scoped_lock lock(stateMutex);
        auto& state = State(actor, Now());
        request = ++nextPotionRequest;
        session = potionSession;
        state.potionRequest = request;
        state.pendingPotionTier = tier;
        state.potionCastAttempted = false;
        state.potionConfirmationSeconds = 0.0F;
    }
    tasks->AddTask([handle, id, tier, request, session]() {
        {
            std::scoped_lock lock(stateMutex);
            const auto found = actors.find(id);
            if (session != potionSession || found == actors.end() ||
                found->second.potionRequest != request) return;
        }
        const auto resolved = handle.get();
        auto* actor = resolved.get();
        auto* caster = actor && !actor->IsDead() && actor->Is3DLoaded()
            ? actor->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant) : nullptr;
        auto* spell = potionForms[tier].spell;
        if (!PotionGameplaySession() || !caster || !spell) {
            std::scoped_lock lock(stateMutex);
            const auto found = actors.find(id);
            if (session == potionSession && found != actors.end() && found->second.potionRequest == request)
                ClearPendingPotion(found->second);
            SKSE::log::warn("Potion replacement could not cast: actor={:08X}, tier={}, request={}", id, tier, request);
            return;
        }
        // Validate the replacement before removing an existing buff. Only the
        // exact dedicated spells are replaced; unrelated potion effects stay.
        actor->AsMagicTarget()->VisitActiveEffects([](RE::ActiveEffect* effect) {
            if (effect && !effect->flags.any(RE::ActiveEffect::Flag::kDispelled)) {
                for (const auto& forms : potionForms) {
                    if (effect->GetBaseObject() == forms.buff && effect->spell == forms.spell) {
                        effect->Dispel(true);
                        break;
                    }
                }
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });
        {
            std::scoped_lock lock(stateMutex);
            const auto found = actors.find(id);
            if (session != potionSession || found == actors.end() || found->second.potionRequest != request) return;
            found->second.potionActive = false;
            found->second.potionPercent = 0.0F;
            found->second.potionCastAttempted = true;
        }
        // Zero-percent tiers still replace the previous buff and get a timer.
        // The configured effect magnitude/duration are also the tooltip data.
        caster->CastSpellImmediate(spell, true, actor, 1.0F, false, 0.0F, actor);
        SynchronizePotionEffect(actor);
    });
}

// --- Sprint-drain discount ---------------------------------------------------
// The engine scales fSprintStaminaDrainPerSec per frame; while a discount is
// active we scale that setting down and restore the captured baseline when it
// ends. The restore happens lazily from the per-frame hook and from session
// resets, so a reload cannot strand a modified global.
float sprintDrainBaseline{-1.0F};
bool sprintDrainScaled{false};

RE::Setting* SprintDrainSetting() {
    auto* collection = RE::GameSettingCollection::GetSingleton();
    return collection ? collection->GetSetting("fSprintStaminaDrainPerSec") : nullptr;
}

void UpdateSprintDrainDiscount(bool discountActive, float multiplier) {
    auto* setting = SprintDrainSetting();
    if (!setting) return;
    if (discountActive) {
        if (!sprintDrainScaled) {
            sprintDrainBaseline = setting->GetFloat();
            sprintDrainScaled = true;
        }
        setting->SetFloat(sprintDrainBaseline * multiplier);
        return;
    }
    if (sprintDrainScaled) {
        setting->SetFloat(sprintDrainBaseline);
        sprintDrainScaled = false;
        sprintDrainBaseline = -1.0F;
    }
}

void RestoreSprintDrain() { UpdateSprintDrainDiscount(false, 1.0F); }


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
    if (!config.director || !EligibleActor(actor) || actor->IsPlayerRef() || !actor->IsInCombat()) return nullptr;
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
    auto result = rules.beginAttack(actor->GetFormID(), ++state.stage, kind, baseline, now, 0.0F, customCost,
        ActiveCostMultiplier(actor, now));
    state.freePowerPending = false;
    state.freePowerStage = 0;
    state.committed = false;
    state.denied = !result.accepted;
    state.nextStage = false;
    if (result.freeCounterattack) {
        state.freePowerPending = true;
        state.freePowerBaseline = baseline;
        state.freePowerStage = state.stage;
        state.freePowerDeadline = now + config.rules.attackCommitTimeoutMs;
    }
    ObserveExhaustion(actor, result.exhausted, now);
    Trace(actor, result.accepted ? "admit" : "reject", state.stage, result.nominalCost, static_cast<int>(result.reason));
    return result.accepted;
}

// A bash keeps a real admission/commit token, but Skyrim owns its normal or
// power-bash stamina debit. It never spends a perfect-parry power reward.
bool BeginBash(RE::Actor* actor, TimeMs now) {
    std::scoped_lock lock(stateMutex);
    Prune(now);
    auto& state = State(actor, now);
    CancelPendingBash(actor, state);
    const auto actorId = actor->GetFormID();
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
        state.bashPending = true;
        state.bashRecordedAt = now;
    }
    const float baseline = Stamina(actor);
    state.bashStage = ++state.stage;
    const auto result = rules.beginAttack(actorId, state.bashStage, AttackKind::Bash, baseline, now);
    state.freePowerPending = false;
    state.freePowerStage = 0;
    state.committed = false;
    state.denied = !result.accepted;
    state.nextStage = false;
    if (!result.accepted) {
        CancelPendingBash(actor, state);
        state.bashStage = 0;
    }
    ObserveExhaustion(actor, result.exhausted, now);
    Trace(actor, result.accepted ? "bash-admit-engine-cost" : "bash-reject", state.stage, 0,
        static_cast<int>(result.reason));
    return result.accepted;
}

// Refunds the engine's power-attack drain for an armed free counterattack, once
// the drop is observed. Only the stage that spent the reward can settle, so
// light attacks and later stages cannot trigger the refund; stage zero means no
// consuming begin has bound the refund yet. Caller must hold stateMutex.
// Balance-based settlement cannot distinguish concurrent external stamina drains.
// Keep its stage and time bounds explicit until native debit attribution is available.
void SettleFreeCounterattack(RE::Actor* actor, ActorState& state, TimeMs now) {
    if (!state.freePowerPending) return;
    if (state.freePowerStage == 0 || state.stage != state.freePowerStage || state.denied ||
        now > state.freePowerDeadline) {
        state.freePowerPending = false;
        state.freePowerStage = 0;
        return;
    }
    if (!state.committed || !actor->IsPowerAttacking()) return;
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
    // The bash committed; its cooldown record now stands on its own.
    state.bashPending = false;
    state.bashRecordedAt = 0;
    state.bashStage = 0;
    if (result.staminaDebit > 0) {
        // Admission already applied the discount before affordability and clamping.
        const float debit = result.staminaDebit;
        if (debit > 0) {
            actor->AsActorValueOwner()->DamageActorValue(RE::ActorValue::kStamina, debit);
            auto* process = actor->GetActorRuntimeData().currentProcess;
            if (process) process->UpdateRegenDelay(RE::ActorValue::kStamina, config.regenDelaySeconds);
        }
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
    // Matches "*dodge*start*" style animation events (e.g. DMCO). Apex owns
    // the dodge cost (dodge frameworks must charge 0); the potion discount
    // scales it, and a discounted dodge never blocks admission.
    if (event.find("dodge") != std::string_view::npos && event.find("start") != std::string_view::npos) {
        auto* process = actor->GetActorRuntimeData().currentProcess;
        if (process) {
            process->UpdateRegenDelay(RE::ActorValue::kStamina, config.dodgeRegenDelaySeconds);
            if (config.dodgeStaminaCost > 0.0F) {
                const float cost = config.dodgeStaminaCost * ActiveCostMultiplier(actor, now);
                if (cost > 0) {
                    actor->AsActorValueOwner()->DamageActorValue(RE::ActorValue::kStamina, cost);
                    Trace(actor, "dodge-cost", 0, cost);
                }
            } else {
                Trace(actor, "dodge-delay", 0, config.dodgeRegenDelaySeconds);
            }
        }
        return true;
    }

    if (!ManageAttack(actor)) return true;
    if (Ranged(actor)) {
        std::scoped_lock lock(stateMutex);
        const auto found = actors.find(actor->GetFormID());
        // Weapon bashes use the same native-cost stage even when the equipped
        // weapon is ranged. Other bow/staff attack events remain unmanaged.
        if (found == actors.end() || found->second.bashStage == 0) return true;
    }
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
        if (state.bashStage != 0) {
            // The interrupted stage is the bash's; cancel it by its token.
            rules.cancelAttack(actor->GetFormID(), state.bashStage, now);
            state.bashStage = 0;
        } else {
            rules.cancelAttack(actor->GetFormID(), state.stage, now);
        }
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

// Defined below with the shield-turtle driver; the graph hook calls it.
void BashReactionRoll(RE::Actor* player, TimeMs now);

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
                    auto& state = State(actor, now);
                    if (state.directorTarget != slotTarget->GetFormID()) {
                        StopDirector(actor, state);
                        state.directorTarget = slotTarget->GetFormID();
                    }
                    return aiRules.tryAcquire(slotTarget->GetFormID(), actor->GetFormID(), now, RandomUnit());
                }();
                if (!slot.granted) {
                    Trace(actor, "slot-denied", 0, 0, static_cast<int>(slot.reason));
                    return false;
                }
                slotGranted = true;
                Trace(actor, slot.retaliating ? "slot-retaliate" : "slot-granted", 0,
                    static_cast<float>(slot.expiresAt - now));
            }
        }
        if (bash && (actor->IsStaggered() || !BeginBash(actor, now))) {
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
        // Bash-as-reaction: nearby blocking shield NPCs roll against the
        // player's attack begin. Rolled once per begin, win or lose.
        if (request && actor->IsPlayerRef()) BashReactionRoll(actor, now);
        const bool result = notify(holder, name);
        if (result && (event == "blockstart" || event == "sbf_blockstart")) BlockEvent(actor, true, now);
        if (event == "blockstop" || event == "sbf_blockstop" || event == "staggerstart") BlockEvent(actor, false, now);
        if (request && !result) {
            std::scoped_lock lock(stateMutex);
            auto& state = State(actor, now);
            rules.cancelAttack(actor->GetFormID(), state.stage, now);
            state.freePowerPending = false;
            state.freePowerStage = 0;
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
            if (state.bashStage != 0) {
                rules.cancelAttack(actor->GetFormID(), state.bashStage, now);
                state.bashStage = 0;
            }
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

// Defined below with the shield-turtle driver; the Update hook calls it.
void DirectorTick(RE::Actor* actor, TimeMs now);

// Scale the queried regeneration rate without changing saved base values or buffs.
struct RegenHook {
    static inline REL::Relocation<float (*)(const RE::ActorValueOwner*, RE::ActorValue)> getValue;
    static float GetValue(const RE::ActorValueOwner* owner, RE::ActorValue value) {
        const float result = getValue(owner, value);
        return active.load() && value == RE::ActorValue::kStaminaRateMult
            ? result * config.staminaRegenMult : result;
    }
    static void Install() {
        REL::Relocation<std::uintptr_t> table{RE::VTABLE_PlayerCharacter[5]};
        getValue = table.write_vfunc(0x01, GetValue);
    }
};

// Per-frame Actor::Update hook: drains stamina while a bow
// is drawn and keeps regen paused for the whole hold. Runs on the game thread;
// it reads no shared runtime state, so no lock is needed.
template<bool Player>
struct UpdateHooks {
    static inline REL::Relocation<void (*)(RE::Character*, float)> update;

    static void Hook(RE::Character* character, float delta) {
        update(character, delta);
        auto* actor = character ? static_cast<RE::Actor*>(character) : nullptr;
        if (!actor || !PotionGameplaySession()) return;
        const bool gameplay = StableGameplay();
        SynchronizePotionEffect(actor, gameplay ? delta : 0.0F);
        if (delta <= 0.0F || !gameplay) return;
        if (!EligibleActor(actor)) {
            // Cleanup must run before the eligibility return: dead actors and
            // actors losing their 3D must not retain a Director slot or strafe.
            std::scoped_lock lock(stateMutex);
            if (const auto found = actors.find(actor->GetFormID()); found != actors.end())
                StopDirector(actor, found->second);
            aiRules.resetActor(actor->GetFormID());
            return;
        }
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
        if constexpr (!Player) DirectorTick(actor, Now());
        if constexpr (Player) {
            const float multiplier = ActiveCostMultiplier(actor, Now());
            UpdateSprintDrainDiscount(multiplier < 1.0F, multiplier);
        }
        if (config.bowDrawDrainPerSecond <= 0.0F) return;
        auto* actorState = actor->AsActorState();
        const auto attack = actorState ? actorState->GetAttackState() : RE::ATTACK_STATE_ENUM::kNone;
        // kBowDraw/kBowAttached cover the pullback animation; kBowDrawn is the
        // full-draw hold. Release states deliberately do not drain.
        if (attack != RE::ATTACK_STATE_ENUM::kBowDraw &&
            attack != RE::ATTACK_STATE_ENUM::kBowAttached &&
            attack != RE::ATTACK_STATE_ENUM::kBowDrawn) return;
        const float drain = config.bowDrawDrainPerSecond * delta * ActiveCostMultiplier(actor, Now());
        if (drain <= 0.0F) return;
        actor->AsActorValueOwner()->DamageActorValue(RE::ActorValue::kStamina, drain);
        auto* process = actor->GetActorRuntimeData().currentProcess;
        if (process) process->UpdateRegenDelay(RE::ActorValue::kStamina, config.regenDelaySeconds);
        Trace(actor, "bow-draw", 0, drain);
    }

    static void Install() {
        REL::Relocation<std::uintptr_t> table{Player ? RE::VTABLE_PlayerCharacter[0] : RE::VTABLE_Character[0]};
        // CommonLib Actor::Update uses 0xAD on SE/AE and 0xAF on VR.
        // Slot 0xAD is Actor::Resurrect on VR and has a different signature.
        const std::size_t slot = REL::Module::IsVR() ? 0xAF : 0xAD;
        update = table.write_vfunc(slot, Hook);
    }
};

// Potion discounts are observed through TESEquipEvent and the tier keywords
// supplied by the data patch; no magic-effect application hook is installed.

// --- Shield-turtle driver ---------------------------------------------------
// While an NPC circles, it holds block and strafes around its target until an
// opening (target recovery/stagger/exhaustion, live retaliation) or the
// max-circle timeout. The commit is a vanilla attackstart: the ordinary
// admission + slot path gates it. Movement is graph-var driven (the mechanism
// ECAI proved); a failed write just retries next tick.

void TurtleStop(RE::Actor* actor, ActorState& state) {
    const bool ownedLocomotion = state.turtleCircling;
    const bool raisedBlock = state.turtleRaisedBlock;
    state.turtleCircling = false;
    state.turtleRaisedBlock = false;
    state.turtleCircleSince = 0;
    state.turtleDirection = 0.0F;
    state.turtleClockwise = false;
    if (!ownedLocomotion || !actor || !actor->Is3DLoaded()) return;
    // Clear every locomotion value this driver writes, and only after this
    // actor actually entered circling. Unmanaged movement remains untouched.
    actor->SetGraphVariableFloat("Speed", 0.0F);
    actor->SetGraphVariableFloat("Direction", 0.0F);
    actor->SetGraphVariableFloat("movementDirection", 0.0F);
    actor->SetGraphVariableFloat("movementSpeed", 0.0F);
    actor->SetGraphVariableFloat("InputDirection", 0.0F);
    actor->SetGraphVariableFloat("InputMagnitude", 0.0F);
    if (raisedBlock && !actor->IsDead() && actor->IsBlocking())
        actor->NotifyAnimationGraph("blockStop");
}

// Caller holds stateMutex. Leaving an encounter preserves bash cooldown;
// actor unload/death separately resets all of that actor's AI ledger state.
void StopDirector(RE::Actor* actor, ActorState& state) {
    const auto target = state.directorTarget;
    state.directorTarget = 0;
    state.directorLastTick = 0;
    TurtleStop(actor, state);
    if (actor && target) aiRules.leaveEncounter(target, actor->GetFormID());
}

void StopAllDirectors() {
    for (auto& [id, state] : actors)
        TurtleStop(RE::TESForm::LookupByID<RE::Actor>(id), state);
}

// Returns true while circling continues this tick. Caller must hold stateMutex.
bool TurtleTick(RE::Actor* actor, RE::Actor* target, ActorState& state, TimeMs now) {
    const auto targetStateIt = actors.find(target->GetFormID());
    const auto* targetState = targetStateIt != actors.end() ? &targetStateIt->second : nullptr;
    // The target is "recovering" while it has an uncommitted attack stage —
    // the swing is out and the stamina debit hasn't landed yet.
    const bool recovering = targetState && !targetState->committed &&
        (targetState->stage != 0 || targetState->bashStage != 0);
    const bool exhausted = targetState ? targetState->exhausted : false;
    const bool retaliating = aiRules.retaliationActive(target->GetFormID(), actor->GetFormID(), now);
    const bool opening = aiRules.isOpening(recovering, target->IsStaggered(), exhausted,
        retaliating, state.turtleCircleSince, now);
    if (opening) {
        TurtleStop(actor, state);
        // A vanilla attack begin: admission, stamina cost, exhaustion gate,
        // and the slot ledger all still gate the actual swing.
        actor->NotifyAnimationGraph("attackStart");
        Trace(actor, "turtle-commit", state.stage, 0);
        return false;
    }
    // Keep circling: strafe perpendicular to the target bearing, curving
    // toward the preferred radius band.
    const auto toTarget = target->GetPosition() - actor->GetPosition();
    const float distance = std::sqrt(toTarget.x * toTarget.x + toTarget.y * toTarget.y);
    const float sign = state.turtleClockwise ? 1.0F : -1.0F;
    RE::NiPoint3 strafe{-toTarget.y * sign, toTarget.x * sign, 0.0F};
    if (distance > config.circleRadiusMax) strafe += toTarget * 0.4F;
    else if (distance < config.circleRadiusMin) strafe -= toTarget * 0.4F;
    const auto* actor3D = actor->Get3D();
    if (!actor3D) return true;  // keep state; retry when the 3D loads
    const auto local = actor3D->world.rotate.Transpose() * strafe;
    const float direction = std::atan2(local.x, local.y) * 57.2957795F;
    state.turtleDirection = direction;
    actor->SetGraphVariableFloat("Speed", 90.0F);  // walk pace; block caps it further
    actor->SetGraphVariableFloat("Direction", direction);
    actor->SetGraphVariableFloat("movementDirection", direction);
    actor->SetGraphVariableFloat("movementSpeed", 0.5F);
    actor->SetGraphVariableFloat("InputDirection", direction);
    actor->SetGraphVariableFloat("InputMagnitude", 0.5F);
    // Shield bearers circle behind a raised shield; re-assert block when it
    // drops. Shieldless circlers stay ready instead of tapping block.
    const auto* left = actor->GetEquippedObject(true);
    if (left && left->IsArmor() && !actor->IsBlocking()) {
        if (actor->NotifyAnimationGraph("blockStart")) state.turtleRaisedBlock = true;
    }
    return true;
}

// Bash-as-reaction: when the player's attack begins near blocking shield
// NPCs that target the player, each rolls bashReactionChance once per attack.
// The bash is dispatched as a vanilla bashStart, so the cooldown, stamina
// cost, SCAR conditions, and exhaustion gate all apply unchanged. Callers
// hold no lock; the roll is taken under stateMutex, the dispatch outside it.
void BashReactionRoll(RE::Actor* player, TimeMs now) {
    if (!config.director || !EligibleActor(player)) return;
    std::vector<RE::Actor*> candidates;
    {
        std::scoped_lock lock(stateMutex);
        for (auto& [id, state] : actors) {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
            if (!EligibleActor(actor) || actor->IsPlayerRef() || !actor->IsBlocking()) continue;
            if (DirectorTarget(actor) != player) continue;
            const float dist = actor->GetPosition().GetDistance(player->GetPosition());
            if (dist > config.circleRadiusMax * 1.5F) continue;
            if (state.bashPending) continue;
            if (!aiRules.rollBash(true, RandomUnit())) continue;
            candidates.push_back(actor);
        }
    }
    for (auto* actor : candidates) {
        Trace(actor, "bash-reaction", 0, 0);
        actor->NotifyAnimationGraph("bashStart");
    }
}

// Distance-banded per-NPC tick from the Actor::Update hook: 100 ms near the
// player, 200 ms mid, skipped beyond 2000 units. Caller holds no lock.
void DirectorTick(RE::Actor* actor, TimeMs now) {
    auto* target = DirectorTarget(actor);
    const auto player = RE::PlayerCharacter::GetSingleton();
    const float playerDist = player ? actor->GetPosition().GetDistance(player->GetPosition()) : 0.0F;
    const bool usable = target && player && !Ranged(actor) && playerDist <= 2000.0F &&
        actor->GetPosition().GetDistance(target->GetPosition()) <= config.circleRadiusMax * 1.5F;
    std::scoped_lock lock(stateMutex);
    if (!usable) {
        if (const auto found = actors.find(actor->GetFormID()); found != actors.end())
            StopDirector(actor, found->second);
        return;
    }
    auto& state = State(actor, now);
    const auto targetId = target->GetFormID();
    if (state.directorTarget != targetId) {
        StopDirector(actor, state);
        state.directorTarget = targetId;
    }
    const TimeMs band = playerDist > 1000.0F ? 200 : 100;
    if (now - state.directorLastTick < band) return;
    state.directorLastTick = now;
    if (aiRules.holdsSlot(targetId, actor->GetFormID(), now) || actor->IsStaggered()) {
        if (state.turtleCircling) TurtleStop(actor, state);
        return;
    }
    if (!state.turtleCircling) {
        state.turtleCircling = true;
        state.turtleCircleSince = now;
        state.turtleClockwise = RandomUnit() < 0.5F;
        Trace(actor, "turtle-enter", state.stage, 0);
    }
    TurtleTick(actor, target, state, now);
}

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
    if (attacker->GetGraphVariableBool("isUnblockableHit", result) && result) return true;
    return false;
}

// Behavior contracts used by Combat Framework and MaxsuIFrame. Annotation-only
// iframe implementations need their own API; missing graph variables do not
// prove that a contact is vulnerable.
bool KnownInvulnerable(RE::Actor* actor) {
    bool combatFrameworkIframe = false;
    if (actor->GetGraphVariableBool("hasIframeCMF", combatFrameworkIframe) && combatFrameworkIframe)
        return true;
    bool iframeActive = false;
    bool inIframe = false;
    if (actor->GetGraphVariableBool("bIframeActive", iframeActive) && iframeActive &&
        actor->GetGraphVariableBool("bInIframe", inIframe) && inIframe) return true;
    // CommonLib's IsGhost relocation has SE/AE IDs only.
    return !REL::Module::IsVR() && actor->IsGhost();
}

Core::PoiseKind PoiseKindFromWeapon(RE::TESObjectWEAP* weapon) {
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

Core::PoiseKind PoiseKindFromWeapon(RE::Actor* attacker) {
    auto* entry = attacker->GetAttackingWeapon();
    return PoiseKindFromWeapon(entry && entry->object ? entry->object->As<RE::TESObjectWEAP>() : nullptr);
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

// HitData captures the weapon, power and block flags before damage processing
// can stop or replace the attacker's animation. Locks stateMutex recursively;
// graph feedback must be dispatched after returning.
Core::PoiseResult ApplyDefenderPoise(RE::Actor* attacker, RE::Actor* defender,
    const RE::HitData& hitData, TimeMs now) {
    if (!config.poise) return {};
    std::scoped_lock lock(stateMutex);
    const auto attackerId = attacker->GetFormID();
    const float multiplier = rules.counterattackActive(attackerId, now)
        ? poiseRules.settings().counterattackPoiseMultiplier
        : 1.0F;
    const bool blocked = hitData.flags.any(RE::HitData::Flag::kBlocked);
    const auto result = poiseRules.applyDamage(defender->GetFormID(), PoiseKindFromWeapon(hitData.weapon),
        hitData.flags.any(RE::HitData::Flag::kPowerAttack),
        blocked ? Core::PoiseSource::BlockedHit : Core::PoiseSource::Hit, now, multiplier);
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
        if (ManageAttack(attacker) && (!Ranged(attacker) || state.bashStage != 0)) {
            if (state.denied || (!state.committed && !Commit(attacker, now))) {
                output.bIgnoreHit = true;
                Trace(attacker, "suppress-rejected-hit", state.stage, 0);
                return output;
            }
        }
        strike = state.committed ? state.stage : Core::EventToken{0};
        // Immunity belongs to the parried strike, independent of whether the
        // defender is still blocking when its other colliders arrive.
        if (strike && state.contacts.contains(strike, defender->GetFormID())) {
            output.bIgnoreHit = true;
            Trace(attacker, "suppress-parried-contact", strike, 0);
            return output;
        }
    }
    if (KnownInvulnerable(defender) || !config.perfectBlock ||
        (!defender->IsPlayerRef() && !config.npcPerfectBlock) ||
        !defender->IsBlocking() || defender->IsStaggered() || Unblockable(attacker) ||
        !WithinBlockCone(defender, attacker)) return output;

    Core::PoiseResult attackerPoise;
    {
        std::scoped_lock lock(stateMutex);
        auto& attackerState = State(attacker, now);
        auto& defenderState = State(defender, now);
        const auto result = rules.successfulBlock(defender->GetFormID(), ++defenderState.hit,
            Stamina(defender), MaximumStamina(defender), now);
        ObserveExhaustion(defender, result.exhausted, now);
        // Ordinary blocks have no parry reward, recoil or pre-hit poise damage.
        if (!result.perfect) return output;
        if (strike && !attackerState.contacts.remember(strike, defender->GetFormID())) {
            output.bIgnoreHit = true;
            Trace(attacker, "duplicate-parry-contact", strike, 0);
            return output;
        }
        output.bIgnoreHit = true;
        if (result.staminaRestore > 0) defender->AsActorValueOwner()->RestoreActorValue(
            RE::ActorValue::kStamina, result.staminaRestore);
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
    // Only a perfect parry reaches this feedback. Graph callbacks can re-enter,
    // so send them after releasing the rules lock.
    PlayParryFeedback(attacker, defender);
    defender->NotifyAnimationGraph("Apex_PerfectParry");
    attacker->NotifyAnimationGraph(attackerPoise.broke ? "staggerStart" : "recoilStart");
    FlashPoiseResult(attacker, attackerPoise);
    return output;
}

// Precision invokes this after all pre-hit callbacks allow a contact and its
// damage routine runs. HitData is a pre-damage snapshot, not a report of health
// actually lost; downstream damage hooks may still change the outcome.
void PostHit(const PRECISION_API::PrecisionHitData& hit, const RE::HitData& hitData) {
    if (!StableGameplay() || (!config.poise && !config.director)) return;
    auto* attacker = hit.attacker;
    auto* defender = hit.target ? hit.target->As<RE::Actor>() : nullptr;
    if (!EligibleActor(attacker) || !EligibleActor(defender) || attacker == defender ||
        KnownInvulnerable(defender)) return;
    const auto now = Now();
    const bool blocked = hitData.flags.any(RE::HitData::Flag::kBlocked);
    if (!blocked && config.director && !defender->IsPlayerRef() &&
        (attacker->IsPlayerRef() || !config.directorPlayerOnly)) {
        std::scoped_lock lock(stateMutex);
        aiRules.onTargetDamagedAttacker(attacker->GetFormID(), defender->GetFormID(), now);
    }
    const auto poise = ApplyDefenderPoise(attacker, defender, hitData, now);
    if (poise.broke) defender->NotifyAnimationGraph("staggerStart");
    FlashPoiseResult(defender, poise);
}

bool AddStaminaCondition(RE::TESIdleForm* idle) {
    const auto* head = idle->conditions.head;
    if (head && head->data.functionData.function == RE::FUNCTION_DATA::FunctionID::kGetActorValue &&
        head->data.functionData.params[0] == reinterpret_cast<void*>(static_cast<std::uintptr_t>(RE::ActorValue::kStamina)) &&
        head->data.object == RE::CONDITIONITEMOBJECT::kSelf &&
        head->data.flags.opCode == RE::CONDITION_ITEM_DATA::OpCode::kGreaterThan &&
        !head->data.flags.isOR && !head->data.flags.global && !head->data.flags.usesAliases &&
        !head->data.flags.usePackData && !head->data.flags.swapTarget && head->data.comparisonValue.f == 0.0F) {
        return false;
    }
    auto* condition = new RE::TESConditionItem();
    condition->data.functionData.function = RE::FUNCTION_DATA::FunctionID::kGetActorValue;
    condition->data.functionData.params[0] = reinterpret_cast<void*>(static_cast<std::uintptr_t>(RE::ActorValue::kStamina));
    condition->data.comparisonValue.f = 0.0F;
    condition->data.flags.opCode = RE::CONDITION_ITEM_DATA::OpCode::kGreaterThan;
    condition->data.object = RE::CONDITIONITEMOBJECT::kSelf;
    // Prepend an AND condition without changing the existing condition chain.
    condition->next = idle->conditions.head;
    idle->conditions.head = condition;
    return true;
}

void PatchAttackIdles() {
    if (!config.patchAttackIdles || !config.playerAttacks || !config.npcAttacks) return;
    std::vector<RE::TESIdleForm*> idles;
    {
        const auto [forms, lock] = RE::TESForm::GetAllForms();
        const RE::BSReadLockGuard guard{lock};
        if (!forms) {
            SKSE::log::warn("Attack IDLE gates unavailable: loaded form map is absent.");
            return;
        }
        // Some form types are not populated in TESDataHandler's type arrays.
        // Use the same loaded-form map as LookupByID and release its lock
        // before modifying any condition lists.
        for (const auto& [formID, form] : *forms) {
            if (form) {
                if (auto* idle = form->As<RE::TESIdleForm>()) idles.push_back(idle);
            }
        }
    }
    std::size_t candidates = 0;
    std::size_t added = 0;
    std::size_t existing = 0;
    std::size_t skippedCounterattack = 0;
    std::size_t rootsFound = 0;
    for (auto* idle : idles) {
        const char* eventName = idle->animEventName.c_str();
        const char* editorID = idle->GetFormEditorID();
        const auto event = Lower(eventName ? eventName : "");
        const auto editor = Lower(editorID ? editorID : "");
        const bool root = idle->GetFormID() == 0x13211 || idle->GetFormID() == 0x13216;
        const bool releaseRoot = editor == "bfco_m1releaseroot_byskse";
        const bool normalEditor = editor.starts_with("adxp_npcnormalattack");
        const bool powerEditor = editor.starts_with("adxp_npcpowerattack");
        if (root) ++rootsFound;
        if (!(root || releaseRoot || normalEditor || powerEditor || AttackRequest(event))) continue;
        ++candidates;
        // Shared roots and power/release paths can admit an earned free
        // counterattack at zero stamina. A permanent AV gate cannot represent
        // that per-actor reward, so leave those paths to graph admission.
        const bool ordinaryLightEvent = event == "attackstart" || event == "attackstartright" || event == "attackstartleft";
        const bool lightOnly = !root && !releaseRoot && !powerEditor &&
            event.find("power") == std::string::npos &&
            (normalEditor || (ordinaryLightEvent && !idle->childIdles));
        if (rules.settings().counterattackEnabled && !lightOnly) {
            ++skippedCounterattack;
            continue;
        }
        if (AddStaminaCondition(idle)) ++added;
        else ++existing;
    }
    SKSE::log::info("Attack IDLE gates: loaded={}, candidates={}, added={}, existing={}, counterattack-skipped={}, vanilla-roots={}/2",
        idles.size(), candidates, added, existing, skippedCounterattack, rootsFound);
    if (rootsFound != 2 || candidates == 0)
        SKSE::log::warn("Attack IDLE enumeration is incomplete; verify the loaded form map before relying on direct IDLE gates.");
    if (skippedCounterattack != 0)
        SKSE::log::warn("Direct IDLE gates cover light-only paths while counterattacks are enabled; shared and power paths rely on graph admission.");
}

class LifecycleEvents final : public RE::BSTEventSink<RE::TESObjectLoadedEvent>,
                             public RE::BSTEventSink<RE::MenuOpenCloseEvent>,
                             public RE::BSTEventSink<RE::TESEquipEvent> {
    RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent* event, RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override {
        if (event && !event->loaded) {
            std::scoped_lock lock(stateMutex);
            const auto found = actors.find(event->formID);
            if (found != actors.end() && found->second.turtleCircling) {
                // Zero the locomotion writes before the state dies with the
                // actor's graph; a reloaded actor starts from a clean slate.
                if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(event->formID))
                    TurtleStop(actor, found->second);
            }
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
                StopAllDirectors();
                rules.resetSession();
                poiseRules.resetSession();
                aiRules.resetSession();
                ++potionSession;
                actors.clear();
                RestoreSprintDrain();
            }
            else {
                std::scoped_lock lock(stateMutex);
                for (auto& [id, state] : actors) { rules.setBlocking(id, false, Now()); }
            }
        }
        return RE::BSEventNotifyControl::kContinue;
    }
    // The latest stamped potion reserves a replacement, including in paused
    // inventory menus. Gameplay state comes only from its live dedicated spell.
    RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* event, RE::BSTEventSource<RE::TESEquipEvent>*) override {
        if (!event || !event->equipped || !active.load() || !config.potionSettings.enabled)
            return RE::BSEventNotifyControl::kContinue;
        auto* actor = event->actor ? event->actor->As<RE::Actor>() : nullptr;
        auto* alch = event->baseObject ? RE::TESForm::LookupByID<RE::AlchemyItem>(event->baseObject) : nullptr;
        if (!actor || actor->IsDead() || !alch || alch->IsFood() || alch->IsPoison())
            return RE::BSEventNotifyControl::kContinue;
        for (int tier = Core::kTierIndexMinor; tier <= Core::kTierIndexUltimate; ++tier) {
            const auto* keyword = potionForms[tier].keyword;
            if (keyword && alch->HasKeyword(keyword)) {
                QueuePotionReplacement(actor, tier);
                break;
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
    if (config.aiSettings.bashStaminaCost != 0.0F)
        SKSE::log::warn("director.bashStaminaCost is deprecated and ignored; Skyrim owns bash stamina costs.");
    if (!config.enabled) { SKSE::log::info("Disabled in configuration."); return true; }
    GraphHooks<false>::Install();
    GraphHooks<true>::Install();
    UpdateHooks<false>::Install();
    UpdateHooks<true>::Install();
    RegenHook::Install();
    PatchAttackIdles();
    if (auto* scripts = RE::ScriptEventSourceHolder::GetSingleton()) {
        scripts->AddEventSink<RE::TESObjectLoadedEvent>(&lifecycle);
        scripts->AddEventSink<RE::TESEquipEvent>(&lifecycle);
    }
    if (auto* ui = RE::UI::GetSingleton()) ui->AddEventSink<RE::MenuOpenCloseEvent>(&lifecycle);
    const int potionBuffTiers = ResolvePotionForms();
    auto* api = static_cast<PRECISION_API::IVPrecision1*>(PRECISION_API::RequestPluginAPI(PRECISION_API::InterfaceVersion::V1));
    if (api) {
        precisionReady = api->AddPreHitCallback(SKSE::GetPluginHandle(), PreHit) == PRECISION_API::APIResult::OK;
        precisionPostHitReady = api->AddPostHitCallback(SKSE::GetPluginHandle(), PostHit) == PRECISION_API::APIResult::OK;
    }
    SKSE::log::info("Precision accepted-hit poise and retaliation callbacks={}", precisionPostHitReady);
    if ((config.poise || config.director) && !precisionPostHitReady)
        SKSE::log::warn("Accepted-hit poise and retaliation require a compatible Precision post-hit API.");
    if (config.poise && config.poiseHud) ConnectTrueHud();
    if (config.perfectBlock) {
        parrySoundWeaponDesc = ResolveConfiguredSound(config.parrySoundWeapon, "weapon parry");
        parrySoundShieldDesc = ResolveConfiguredSound(config.parrySoundShield, "shield parry");
        parrySoundVolume = config.parrySoundVolume;
        parryVisual = config.parryVisual;
    }
    SKSE::log::info("Capabilities: graph stamina=true; attack IDLE gates={}; Precision melee parry={}; "
        "bow draw drain={}; poise={}; TrueHUD poise bar={}; parry sfx={}; parry vfx={}; director={}; "
        "stamina regen mult={}; dodge cost={}; potion discount={}; buff tiers={}; VR={}",
        config.patchAttackIdles, precisionReady, config.bowDrawDrainPerSecond, config.poise,
        trueHudReady, parrySoundWeaponDesc != nullptr || parrySoundShieldDesc != nullptr,
        parryVisual, config.director, config.staminaRegenMult, config.dodgeStaminaCost,
        config.potionSettings.enabled && potionFormsReady, potionBuffTiers, REL::Module::IsVR());
    if (config.potionSettings.enabled && !potionFormsReady)
        SKSE::log::warn("Potion discount disabled: all six keyword/receipt/buff/spell bundles are required. "
            "Regenerate and enable ApexCombat - Potions.esp with tools/PotionPatcher.");
    if (!precisionReady) SKSE::log::warn("Perfect blocking requires a compatible Precision API. Stamina features remain available.");
    if (config.poise && config.poiseHud && !trueHudReady)
        SKSE::log::warn("Poise is enabled but the TrueHUD poise bar is unavailable. Poise rules still apply.");
    initialized = true;
    return true;
}

void ResetSession() {
    active = false;
    std::scoped_lock lock(stateMutex);
    StopAllDirectors();
    rules.resetSession();
    poiseRules.resetSession();
    aiRules.resetSession();
    ++potionSession;
    actors.clear();
    // A session change while a discount is active must not strand the scaled
    // sprint-drain game setting.
    RestoreSprintDrain();
}

void StartSession() {
    ResetSession();
    active = initialized && config.enabled;
    SKSE::log::info("Combat session ready={}", active.load());
}
}
