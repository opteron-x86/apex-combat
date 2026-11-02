#pragma once

#include "CombatRules.h"

#include <unordered_map>

namespace Apex::Core {

// Combat director settings: attack-slot pacing plus bash discipline.
// All times share the monotonic TimeMs clock used by CombatRules/PoiseRules.
struct AiSettings {
    // Maximum simultaneous melee attackers per target. 1..5.
    std::uint32_t maxAttackers{2};
    // A granted slot expires after a randomized interval in [slotWindowMinMs,
    // slotWindowMaxMs]; expiry is lazy (checked on acquire and on tick).
    TimeMs slotWindowMinMs{4000};
    TimeMs slotWindowMaxMs{8000};
    // When the target damages a circling attacker, that attacker gains a
    // retaliation window during which it bypasses the slot count. The slot is
    // short and non-renewing. 0 disables retaliation.
    TimeMs retaliationMs{3000};
    // Minimum interval between one NPC's bashes. Enforced in admission.
    TimeMs bashCooldownMs{6000};
    // Chance that a blocking shield NPC bashes in reaction to a target attack
    // beginning in range. Rolled once per target attack, not per frame.
    float bashReactionChance{0.35F};
    // Chance of a bash against a passive target. Default 0: no unpunishable
    // openers against a player who is just standing there.
    float bashProactiveChance{0.0F};
    // Deprecated configuration compatibility field. Native Skyrim owns bash
    // stamina costs; a nonzero legacy value is ignored by the runtime.
    float bashStaminaCost{0.0F};
    // A circling shield NPC that has seen no opening for this long commits
    // anyway, so turtles cannot stall a fight forever.
    TimeMs maxCircleMs{10000};
};

enum class SlotRejectReason {
    None,
    InvalidInput,
    // No free slot and no live retaliation window for this attacker.
    NoSlot,
};

struct SlotResult {
    bool granted{false};
    bool retaliating{false};
    SlotRejectReason reason{SlotRejectReason::None};
    // Valid when granted: the slot expires at this time unless released first.
    TimeMs expiresAt{0};
};

enum class BashRejectReason {
    None,
    InvalidInput,
    OnCooldown,
};

struct BashResult {
    bool allowed{false};
    BashRejectReason reason{BashRejectReason::None};
    // Valid when allowed: the NPC may not bash again before this time.
    TimeMs nextAllowedAt{0};
};

// Single-threaded combat-director state. Call from the game thread or
// serialize externally (same contract as PoiseRules). Randomness is injected
// by the caller so tests are deterministic: the runtime draws a uniform
// sample in [0,1) and passes it where a roll or a window draw is needed.
class AiRules {
public:
    explicit AiRules(AiSettings settings = {});

    const AiSettings& settings() const noexcept;
    // Validates first, then replaces settings and clears all state.
    void setSettings(AiSettings settings);

    // --- Attack slots -------------------------------------------------------

    // Attempt to grant `attacker` an attack slot against `target`.
    // `windowSample` in [0,1) selects the slot duration between the min and
    // max window; values are clamped. A current holder re-acquiring keeps its
    // original expiry (slots rotate, they do not refresh).
    SlotResult tryAcquire(ActorId target, ActorId attacker, TimeMs now, float windowSample);
    void release(ActorId target, ActorId attacker) noexcept;
    // Actor left this encounter: clear its slot and retaliation entitlement,
    // preserving its bash cooldown and all unrelated encounters.
    void leaveEncounter(ActorId target, ActorId attacker) noexcept;
    // Opens the retaliation window for `attacker` against `target`; call when
    // the target damages a non-holder. No-op while retaliationMs == 0.
    void onTargetDamagedAttacker(ActorId target, ActorId attacker, TimeMs now);
    // Number of live (unexpired) slot holders for a target.
    std::uint32_t holderCount(ActorId target, TimeMs now);
    bool holdsSlot(ActorId target, ActorId attacker, TimeMs now);
    bool retaliationActive(ActorId target, ActorId attacker, TimeMs now);

    // --- Bash discipline ----------------------------------------------------

    // Admission check only; records the bash on success so the cooldown
    // starts at commit time (the runtime calls this when the bash begins,
    // matching how attack admission owns its stage token).
    BashResult admitBash(ActorId actor, TimeMs now);
    // Cancels a recorded bash whose commit never landed (rejected/superseded
    // begin), restoring eligibility exactly as before the attempt.
    void cancelBash(ActorId actor, TimeMs recordedAt) noexcept;
    TimeMs bashCooldownRemaining(ActorId actor, TimeMs now) const noexcept;

    // Rolls one bash decision. `sample` in [0,1) is clamped; returns true when
    // the roll passes for the given situation. Pure function of settings.
    bool rollBash(bool targetIsAttacking, float sample) const noexcept;

    // --- Shield-turtle openings ----------------------------------------------

    // A circling blocker commits to an attack when any opening holds:
    // the target is mid-recovery, staggered, or exhausted; the circler's
    // retaliation window is live; or the circle has outlasted maxCircleMs.
    // Pure function — the runtime gathers the inputs per tick.
    bool isOpening(bool targetRecovering, bool targetStaggered, bool targetExhausted,
        bool retaliating, TimeMs circleStartedAt, TimeMs now) const noexcept;

    // --- Housekeeping -------------------------------------------------------

    // Removes the actor as both a target and a participant.
    void resetActor(ActorId actor) noexcept;
    void resetSession() noexcept;
    std::size_t targetCount() const noexcept;

private:
    struct Holder {
        TimeMs expiresAt{0};
    };
    struct TargetState {
        std::unordered_map<ActorId, Holder> holders;
        std::unordered_map<ActorId, TimeMs> retaliationUntil;
    };

    static void validate(const AiSettings& settings);
    void evictExpired(TargetState& state, TimeMs now) noexcept;

    AiSettings settings_;
    std::unordered_map<ActorId, TargetState> targets_;
    std::unordered_map<ActorId, TimeMs> lastBash_;
};

}  // namespace Apex::Core
