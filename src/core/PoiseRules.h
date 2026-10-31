#pragma once

#include "CombatRules.h"

#include <unordered_map>

namespace Apex::Core {

enum class PoiseKind { Dagger, Sword, Axe, Mace, TwoHanded, Unarmed };
enum class PoiseSource { Hit, BlockedHit, PerfectParry };

struct PoiseSettings {
    float maxPoise{100.0F};
    float daggerDamage{20.0F};
    float swordDamage{25.0F};
    float axeDamage{30.0F};
    float maceDamage{35.0F};
    float twoHandedDamage{40.0F};
    float unarmedDamage{15.0F};
    float powerMultiplier{1.5F};
    // Fraction of the listed damage that passes through an ordinary held block.
    float blockedMultiplier{0.5F};
    // Flat damage dealt to the attacker by a perfect parry; kind is ignored.
    float perfectParryDamage{50.0F};
    // Poise regenerates at recoveryPerSecond only after this delay since the
    // last damage application.
    TimeMs recoveryDelayMs{3000};
    float recoveryPerSecond{20.0F};
    // After a break the actor is staggered for this long; further poise damage
    // is ignored inside the window and poise refills to max when it expires.
    TimeMs breakRecoveryMs{1500};
};

enum class PoiseRejectReason { None, InvalidInput, Staggered };

struct PoiseResult {
    bool applied{false};
    bool broke{false};
    PoiseRejectReason reason{PoiseRejectReason::None};
    float poiseAfter{0.0F};
    TimeMs staggeredUntil{0};
};

// Single-threaded poise pool per actor. Call from the game thread or serialize
// externally. All times share a monotonic clock. An unseen actor is treated as
// full poise; regen and break recovery are computed lazily at each call.
class PoiseRules {
public:
    explicit PoiseRules(PoiseSettings settings = {});

    const PoiseSettings& settings() const noexcept;
    // Validates first, then replaces settings and clears actor state.
    void setSettings(PoiseSettings settings);

    PoiseResult applyDamage(ActorId actor, PoiseKind kind, bool power,
                            PoiseSource source, TimeMs now);
    // Current poise with lazy regen and break recovery applied.
    float poise(ActorId actor, TimeMs now);
    bool isStaggered(ActorId actor, TimeMs now);

    void resetActor(ActorId actor);
    void resetSession() noexcept;
    std::size_t actorCount() const noexcept;

private:
    struct ActorState {
        float poise{0.0F};
        bool seen{false};
        TimeMs lastDamage{0};
        bool broken{false};
        TimeMs staggeredUntil{0};
    };

    static void validate(const PoiseSettings& settings);
    float observe(ActorState& state, TimeMs now);
    PoiseSettings settings_;
    std::unordered_map<ActorId, ActorState> actors_;
};

}  // namespace Apex::Core
