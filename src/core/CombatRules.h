#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace Apex::Core {

using ActorId = std::uint32_t;
using EventToken = std::uint64_t;
using TimeMs = std::int64_t;

enum class AttackKind { LightOneHanded, LightTwoHanded, Unarmed, Power, Dagger, OneHandedAxe, OneHandedMace, DualWield, Bash };
enum class Affordability { PositiveStamina, FullCost };

struct Settings {
    float lightOneHandedCost{20.0F};
    float lightTwoHandedCost{33.0F};
    float unarmedCost{10.0F};
    float daggerCost{10.0F};
    float axeCost{25.0F};
    float maceCost{25.0F};
    // Multiplier applied to the averaged per-hand cost of a dual-wield attack.
    float dualWieldMultiplier{1.6F};
    Affordability affordability{Affordability::PositiveStamina};
    // When false, power admission uses the supplied cost, but the engine owns its debit.
    bool chargePowerAttacks{false};
    TimeMs attackCommitTimeoutMs{1000};
    TimeMs perfectBlockWindowMs{150};
    float perfectBlockRestore{25.0F};
    // Minimum interval between block starts; repeated starts inside it do not arm a window.
    // Set to zero to allow every fresh block edge to arm.
    TimeMs parryRearmDelayMs{200};
    // A perfect parry grants the defender a counterattack window during which their next
    // power attack costs no stamina. The reward fires once and expires with the window.
    bool counterattackEnabled{true};
    TimeMs counterattackWindowMs{1500};
};

enum class RejectReason {
    None,
    InvalidInput,
    NoStamina,
    InsufficientStamina,
    DuplicateToken,
    MissingStage,
    StageExpired,
    ClockRegressed
};

struct AttackResult {
    bool accepted{false};
    bool duplicate{false};
    float nominalCost{0.0F};
    // Always zero from beginAttack. Only the first successful commit may return a debit.
    // The runtime must apply this amount before processing another attack for this actor.
    float staminaDebit{0.0F};
    bool engineOwnsCost{false};
    bool exhausted{false};
    // True when this attack consumed a pending perfect-parry counterattack reward. The
    // runtime refunds the engine's power-attack stamina deduction for this commit.
    bool freeCounterattack{false};
    RejectReason reason{RejectReason::None};
};

struct ExhaustionState {
    bool exhausted{false};
    bool entered{false};
    bool recovered{false};
};

enum class ParryReason {
    Perfect,
    NormalBlock,
    DuplicateToken,
    InvalidInput,
    ClockRegressed
};

struct ParryResult {
    bool perfect{false};
    float staminaRestore{0.0F};
    bool exhausted{false};
    ParryReason reason{ParryReason::NormalBlock};
};

// Single-threaded rules state. Call from the game thread or serialize externally.
// Stage and hit tokens must be nonzero and strictly increasing per actor, separately
// for each event type. Reuse a token for duplicate notifications of the same event.
// resetActor/resetSession start new token sequences. All times share a monotonic clock.
class CombatRules {
public:
    explicit CombatRules(Settings settings = {});

    const Settings& settings() const noexcept;
    // Validates first, then replaces settings and clears actor state.
    void setSettings(Settings settings);

    // The cost multiplier is captured at admission and applies only to Apex-owned costs.
    // It must be finite and within [0, 1]; the returned debit is already discounted.
    AttackResult beginAttack(ActorId actor, EventToken stage, AttackKind kind,
                             float stamina, TimeMs now, float vanillaPowerCost = 0.0F,
                             float customCost = 0.0F, float costMultiplier = 1.0F);
    AttackResult commitAttack(ActorId actor, EventToken stage, float stamina, TimeMs now);
    // Cancellation invalidates a pending stage. Committed attacks are never refunded.
    bool cancelAttack(ActorId actor, EventToken stage, TimeMs now);
    ExhaustionState observeStamina(ActorId actor, float stamina, TimeMs now);
    bool isExhausted(ActorId actor) const noexcept;

    // Returns true only when this call arms a new perfect-block window.
    bool setBlocking(ActorId actor, bool blocking, TimeMs now);
    // Call only after the runtime confirms an eligible incoming hit was blocked.
    // An ordinary held block or a duplicate hit returns no restoration.
    ParryResult successfulBlock(ActorId actor, EventToken hit, float stamina,
                                float maxStamina, TimeMs now);
    // Arms the perfect-parry counterattack window for this actor. Returns true only when a
    // new window is armed (not on a duplicate grant inside an already-active window).
    bool grantCounterattack(ActorId actor, TimeMs now);
    // True while this actor holds an unconsumed, unexpired counterattack reward.
    bool counterattackReady(ActorId actor, TimeMs now) noexcept;
    // True while the actor's counterattack window is open, consumed reward or
    // not. The window closes at expiry or when settings reset actor state.
    bool counterattackActive(ActorId actor, TimeMs now) noexcept;

    void resetActor(ActorId actor);
    void resetSession() noexcept;
    std::size_t actorCount() const noexcept;

private:
    struct ActorState {
        bool hasTime{false};
        TimeMs lastTime{0};
        bool exhausted{false};
        EventToken highestStage{0};
        bool stagePending{false};
        TimeMs stageStart{0};
        float stageCost{0.0F};
        bool engineOwnsCost{false};
        bool freeCounterattack{false};
        bool counterattackArmed{false};
        TimeMs counterattackUntil{0};
        EventToken highestHit{0};
        bool blocking{false};
        bool blockArmed{false};
        bool hasBlockStart{false};
        TimeMs blockStart{0};
    };

    static void validate(const Settings& settings);
    static bool updateClock(ActorState& state, TimeMs now);
    static ExhaustionState updateStamina(ActorState& state, float stamina);
    // Re-arms a counterattack reward whose rewarded swing did not commit (superseded,
    // rejected, cancelled, expired). Also clears the per-stage flag so a later commit
    // cannot refund on the old swing's behalf. Expired windows are not resurrected.
    static void rearmFreeCounterattack(ActorState& state, TimeMs now, bool spent) noexcept;
    RejectReason affordabilityFailure(float stamina, float cost) const noexcept;
    Settings settings_;
    std::unordered_map<ActorId, ActorState> actors_;
};

}  // namespace Apex::Core
