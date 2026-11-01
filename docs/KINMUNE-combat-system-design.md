# KINMUNE combat system design

Proposed replacement architecture for the BFCO and For Honor foundation, October 31, 2026. This is a design for implementation and testing; no replacement plugin has been built or installed.

A unified native combat-rules plugin is a reasonable direction for this installation. It would own action costs, exhaustion, timed blocking, stamina rewards, and poise, while BFCO and For Honor continue to supply attack behavior and movesets. The intended benefit is a consistent set of rules with one implementation of each mechanic.

**Keep the animation and collision foundation**

Keep BFCO, For Honor Reforged with its required base assets, Precision, the behavior-generation pipeline, and True Directional Movement. Retain supporting frameworks where required by those components. Retain SCAR as the NPC combo selector and DMCO as the dodge animation system initially. DMCO's existing native 25-stamina restriction already works at the configuration/source level; keep it as the sole dodge-cost owner during the prototype rather than charging the dodge again. TrueHUD can display the custom poise state.

The custom layer would take over Gravitas's combat rules, Chocolate Poise's posture calculations, and Parry for All/Sekiro's timed-block gameplay. Parry – Simple Weapon Parrying would also be retired or explicitly integrated, since its independent manipulation of blocking state would undermine a single parry system. These removals are staged migrations, not a recommendation to disable everything immediately in the current save.

For Honor Reforged is more than an animation pack: its abilities, perks, status effects, cooldowns, and follow-ups are part of the desired combat. The replacement must preserve their meaning or deliberately replace each affected mechanic. In particular, its counterattack parry and special knockdowns remain separate from tapping block for a perfect deflection.

**The combat contract**

| Event or state | Proposed rule |
| --- | --- |
| Start an attack | Validate stamina, stagger state, and action restrictions before the attack can produce damage. |
| Accepted attack stage | Deduct the cost once. A miss still costs stamina; several hitboxes in one swing do not charge several times. |
| Combo continuation | Each additional attack stage must pass the same admission check. |
| Exhausted actor | Cannot start an attack or dodge. An already committed swing finishes. Blocking remains available when otherwise actionable. |
| Ordinary block | Mitigate damage and spend stamina according to the blocked impact and equipment. Insufficient guard stamina can cause guard break. |
| Perfect block | Reject the eligible incoming hit, spend no ordinary block stamina, restore 25 stamina once, and apply configured poise damage to the attacker. |
| Poise reaches zero | Trigger a supported stagger/reaction on player or enemy. Replenish/reset poise at a defined recovery point. |
| Actor already staggered | Prevent repeated poise-break triggers during the reaction; use a short recovery allowance to avoid indefinite chains. |
| For Honor special counter/setup | Preserve its authored conditions for special knockdowns and follow-ups. |

Stamina and poise should remain distinct. Stamina controls whether an actor can act; poise controls interruption. Perfect blocking connects them by recovering the defender's stamina and pressuring the attacker's poise.

**Starting balance, subject to playtesting**

Use the familiar configured costs as a baseline: 20 for a one-handed light, 33 for a two-handed light, and 25 per dodge. Power attacks need one consistent calculation that preserves intended weapon/perk scaling and For Honor's special-move modifiers. Initially preserve the engine's existing power-attack debit and gate admission around it; replacing that debit requires explicit verification. Do not charge a generic light cost or a second full power cost on top of it.

I recommend full-cost admission as the default: an actor needs enough stamina to pay for the next action. This is stricter than merely blocking attacks at exactly zero, so expose the positive-stamina-only alternative as a setting. Holding attack must not evade either policy.

Start the perfect window at 150 ms, opening from an accepted new block action. Holding block must not repeatedly reopen it. Consume the successful opportunity so one held input does not automatically perfect-parry an entire combo. Repeated release/press inputs need an explicit anti-spam rule, with timings chosen through testing. The player can attempt the perfect block at zero stamina, provided they are not otherwise locked in an action or stagger.

For ordinary blocks, tune the stamina cost by blocked impact, shield/weapon type, and block skill. For perfect blocks, begin with a fixed 25-point reward; this provides one dodge without scaling excessively with a large stamina pool. Specify melee, arrow, and magic eligibility independently. Begin the prototype with melee; add projectile and spell rules only after their hit paths are verified.

Poise damage should reflect attack type, weapon class, armor, and target type without extreme weight scaling. Ordinary breaks produce a supported stagger, not an unconditional ragdoll. Bosses and large creatures can use explicit resistance/reaction profiles. Apply the same resource rules to NPCs where their action pipeline permits it, using documented exceptions instead of hidden unlimited resources.

**One hit-resolution sequence**

Each eligible impact should pass through a single ordered decision:

1. Reject invalid targets, duplicate callbacks for the same strike/target, and contacts excluded by active dodge invulnerability.
2. Check whether the defender is facing the hit and has an eligible perfect-block window.
3. If perfect, suppress damage and the ordinary block charge, consume the window, grant the reward once, then apply the attacker's poise consequence.
4. Otherwise resolve an ordinary block or an unblocked hit and its appropriate resource/poise effects.
5. Trigger at most one poise-break reaction from that resolution, then emit feedback and diagnostic data.

Weapon clashes and For Honor counterattack windows must enter this sequence through explicit adapters. They must not independently toggle blocking or apply a second reward behind its back.

**Implementation shape**

Use a CommonLibSSE-NG native plugin initially targeting the actual Skyrim 1.6.1170 environment. Pair it with a small data plugin for any required keywords, perks, spells, and deliberate record overrides, plus versioned settings. Keep dependencies and hooks explicit; framework APIs do not guarantee that every BFCO action is covered automatically.

The stamina controller needs per-actor action state: pending request, accepted stage, commitment, and recovery. Reserve the cost when admitting an action, confirm its start, and release the reservation if dispatch fails before commitment. Once the swing commits, ordinary player cancellation does not refund it. Load/reset paths must clear stale reservations and transient state.

Input hooks alone cannot enforce the rule: BFCO, BFCO_Functions, SCAR, and combo transitions have custom dispatch paths. Hit-frame-only charging is also inadequate because misses, multi-hit animations, synthetic events, and cancels can produce inconsistent costs. The prototype must identify the actual dispatch/transition points before claiming a hard lock.

Use Precision callbacks for supported collision paths, with separately verified handling for relevant vanilla/projectile paths. Keep deduplication tied to the actual strike and target so a legitimate hit on another target is not accidentally erased.

For Honor compatibility needs a dedicated adapter for its poise damage/recovery effects, counterattack success, unstable/guard-break states, knockdown cooldowns, and follow-up conditions. Preserve existing identifiers where practical and trace the winning plugin records rather than relying on names. Publish poise state through TrueHUD's supported API and expose concise debugging readouts for tests.

The installed For Honor Balance Patch has no hard Chocolate Poise master, but it does depend on its behavior: effects `0x5F5`, `0x89F`, and `0x8BD` request poise damage through `Variable01`; the Black Prior shield-flip effect uses `Variable03`. Blessed contributes additional attacking-vulnerability effects. The adapter should recognize the specific plugin/FormKey contracts, avoiding a global interpretation of every use of these generic actor values. Preserve OAR conditions, payload spells, and cooldown markers until their replacements are proven. Existing spell magnitudes are inputs to the old calculations, not final damage values to copy blindly.

**A bounded first prototype**

First prove stamina admission and tap-block rewards with one one-handed For Honor moveset, one humanoid SCAR enemy, and DMCO's basic dodge. Keep a controlled poise baseline during this first stage. Then add custom poise and the For Honor adapter to complete the loop:

- Light attack costs stamina once on both hit and miss.
- Holding a combo cannot create the next attack without permission.
- Power attack and dodge fail under the selected affordability rule.
- Tapping block successfully restores exactly 25 stamina, including at zero.
- A failed timing attempt gives an ordinary block, not a reward.
- Poise breaks stagger both actors without permanent stagger loops.
- For Honor's selected counter/follow-up still functions.

After this passes, expand to two-handed weapons, dual wielding, shields, unarmed, sprint/jump/dodge attacks, multi-hit moves, creatures, and projectiles. Exercise interruption, weapon switching, actor death/unload, menus, save/load, and input buffering. Check frame-rate sensitivity and repeat with several nearby actors.

**Migration**

Use a separate test profile and disposable saves. Turn off the old owner of a mechanic only when the replacement for that mechanic is active. Disable old stamina deductions when enabling the new cost controller; likewise remove competing timed-block interception and poise reactions as their replacements come online. Rebuild behaviors when behavior inputs change and verify the resulting files win.

Gravitas also changes other gameplay values. Review which of those should be retained instead of assuming that deleting its plugin affects only light-attack stamina. Reconcile the SCAR upgrade and For Honor's missing guard-break dependency as part of the new foundation. Preserve a working baseline for side-by-side tests.

The initial deliverable should be a playable, narrowly scoped prototype with observable costs and event traces. Broad compatibility comes after the basic loop is demonstrated in game. Static inspection can guide the implementation, but cannot establish that every authored animation obeys the new rules.

The [installation audit](/home/hackdirt/Documents/Codex/2026-10-31/i-x20/outputs/KINMUNE-CS-combat-audit.md) records the current conflicts and the incremental repair alternative.
