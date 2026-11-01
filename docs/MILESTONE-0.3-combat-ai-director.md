# Apex Combat — Milestone 0.3 Design: Combat AI Director

Status: agreed with user (2026-11-01), implementation in progress.
Scope decision: **full replacement** — Apex Combat owns NPC combat
behavior natively; Wait Your Turn Redux and Enhanced Combat AI get
retired when this ships. SCAR-AE (github.com/doodlum/SCAR-AE) may be
folded in later as a separate decision — not part of this milestone.

## Goals (user requirements, authoritative)

1. **Attack slots**: configurable maximum simultaneous melee attackers
   (default 2, range 1–5). Slot holders attack; everyone else circles.
2. **Circling**: non-holders orbit the target at melee-adjacent range
   instead of standing in a crowd. Apex owns the movement — no Combat
   Pathing Revolution dependency.
3. **Bash discipline** (the active pain point): NPCs can no longer
   spam shield bash. Bash becomes a *reaction* with a hard cooldown,
   and bash costs stamina for NPCs and the player alike.
4. **Shield-turtle behavior**: shield-bearing NPCs approach and circle
   **while blocking**, waiting for an opening. The player breaks the
   turtle by hitting the shield — blocked hits chip the NPC's poise
   (existing `PoiseRules` blocked fraction), and a poise break
   staggers them open. They have a chance to bash in reaction to the
   player attacking; they do not proactively bash a passive player.
5. **Retaliation window**: when the player hits a circling NPC, that
   NPC gets a short priority window (~3 s default) to answer — fights
   feel reactive, not robotic slot rotation.

## What the incumbents actually do (research, 2026-11-01)

Verified from installed configs and ECAI source
(github.com/tavvfiq/combat-ai-ng, cloned to /tmp/opencode/combat-ai-ng):

| | Gravitas | Enhanced Combat AI 1.9.1 | Wait Your Turn Redux |
|---|---|---|---|
| Attack slots | none | `MaxSimultaneousAttackers=2`, window 2–4 s, player-target only | `iMaxAttackers=2`, window 12–20 s, retaliation 3–6 s |
| Circling | none | denied attackers all pick identical `Flanking` (synchronized circling) via **CPR graph vars — no native movement** | native lightweight circling, its core feature |
| Bash spam fix | none (only 25/55 stamina costs) | 2 s bash cooldown + mistake chance | none |
| Sustained block | no | **no** — only momentary TimedBlock windows; block is left to vanilla AI | no |
| Decision quality | static combat-style records (Valravn/RAID blend) | full matrix: interrupt/evade/retreat/offense scoring, damage momentum, humanizer (reaction delay + mistakes) | none |

ECAI weaknesses we deliberately avoid (source-verified):

- **No native movement** — without CPR, strafe/flank/retreat silently
  no-op. Apex owns locomotion.
- **No attack-start/hit hooks** — everything is 100 ms polling +
  fixed "hit at 60% of swing" heuristic. Apex already has Precision
  PreHit + animation-graph event sinks; we detect commits precisely.
- **No sustained block action** — defense is bolted onto vanilla block.
  Our shield-turtle is a first-class state.
- **Synchronized flanking** — denied attackers all choose the same
  action. Our circlers get per-attacker orbit parameters.
- **One giant vtable thunk + try/catch** around recycled 0xFF FormIDs.
  We use the existing per-actor map + Prune/TESObjectLoaded hygiene.

Gravitas stays for its other roles (stamina cost ESP records it wins,
combat styles); its bash stamina costs *complement* our cooldown.
When Apex gates NPC admission for bash, we must not double-gate:
Gravitas's cost stays on the record, Apex adds the cooldown/reaction
layer. SCAR NPC Exhaustion System currently wins the SCAR attack
IDLE conditions — bash admission must tolerate SCAR gating (if SCAR
rejects, our state machine sees the rejection and re-arms, same
pattern as MCO/BFCO retries in the counterattack window).

## Architecture

New module `src/core/AiRules.h/.cpp` (pure logic, unit-testable, same
pattern as CombatRules/PoiseRules) + runtime wiring in
`src/runtime/Runtime.cpp`. All per-actor state lives in the existing
runtime actor map behind `stateMutex`; the per-frame driver is the
existing `UpdateHooks` vtable-0xAD hook (already proven for bow drain
and counterattack settle fallback).

### Core: slot ledger (`AiRules`)

Per **target** (the player, and optionally NPC targets):

```
SlotLedger {
  maxAttackers = 2            // config 1..5
  slotSecondsMin/Max = 4..8   // config; randomized per acquire
  retaliationSeconds = 3      // config 0..30
  holders: map<attackerId, {expiresAtMs}>
  retaliation: map<attackerId, {untilMs}>
}
```

- `tryAcquire(attacker, now)` — evicts expired holders, grants if
  `holders.size() < maxAttackers`, or if the attacker has a live
  retaliation window (retaliation bypasses the count, but its slot is
  short and non-renewing).
- `release(attacker)` — on attack commit end, stagger, death, unload,
  or target change.
- `onTargetDamagedBy(attacker)` — opens the retaliation window for
  that attacker (called from PreHit when the target damages a
  circler… inverted: when the *player* damages NPC X, X gets a
  retaliation window against the player).
- Slot expiry is lazy (checked on acquire and on the per-frame tick);
  no timers, no callbacks.

### Core: bash discipline

```
BashPolicy {
  cooldownSeconds = 6        // config 1..60 (ECAI's 2 s is too short
                             // per user; 6 s default feels deliberate)
  reactionChance = 0.35      // config 0..1 — chance a blocking shield
                             // NPC bashes in reaction to a player
                             // attack start in range
  proactiveChance = 0.0      // config 0..1 — bash vs passive player;
                             // default OFF (no unpunishable openers)
  staminaCost = 25           // config 0..1000 — NPC and player alike
}
```

- Cooldown is per-NPC (`lastBashAtMs`), enforced in admission.
- Reaction trigger: player attack *begin* observed (we already see
  begin events) while NPC is in shield-approach/circle state, in
  bash range (weapon reach via Precision if available, fallback 150
  units), facing the player. One chance roll per player attack, not
  per frame.
- **Stamina**: NPC bash admission requires `stamina >= cost` and
  debits on commit, reusing the existing `beginAttack`/`commitAttack`
  machinery — bash is treated as an `AttackKind` so exhaustion and
  regen delays apply automatically. Player bash: same cost through
  the same path (player bash already flows through attack events;
  verify in testing whether BFCO/vanilla routes bash through the same
  admission — if not, add a `bashStart` event hook for the player).
- Telemetry: `bash-denied-cooldown`, `bash-denied-stamina`,
  `bash-reaction` traces, same pattern as existing trace lines.

### Core: shield-turtle state machine

Per NPC with a shield (`GetWornArmor(kShield)` precedent exists in
the parry-SFX code) whose target is the player:

```
States: AdvanceBlocking -> CircleBlocking -> (opening) -> Commit
```

- **AdvanceBlocking**: outside engagement range → approach with
  block up (`blockStart` graph event; `blockStop` on state exit).
- **CircleBlocking**: inside engagement range but no attack slot →
  hold block, orbit at ring radius with per-NPC direction/orbit-speed
  so they don't synchronize.
- **Opening triggers** (any): player attack recovery (we see
  `attackStop`), player staggered/exhausted, retaliation window live,
  or a commit roll after `maxCircleSeconds` (config, default 10) so
  they can't turtle forever. On opening: `blockStop`, acquire slot,
  attack (or bash via reaction policy).
- **Breaking the turtle**: blocked hits chip NPC poise at the
  existing blocked fraction; poise break → `staggerStart` (existing
  PoiseRules). This is the intended punish loop.
- Block is **not free**: Gravitas's block regen penalty and our
  exhaustion rules apply; an exhausted NPC drops block (can't afford
  to keep guard up at 0 stamina — vanilla engine handles the drop).

### Runtime: movement (circling without CPR)

The rejected ECAI approach was delegating to CPR. Native approach,
in order of preference (decided at implementation time after
disassembly checks):

1. Set movement graph vars directly (`Speed`, `Direction`,
   `movementDirection` — the TDM-style vars ECAI also sets as
   fallback) while the NPC is in a movement state, letting the
   vanilla pathing/heading keep them at range.
2. If graph vars prove unreliable on NPCs (they're player-centric in
   some builds), fall back to `SetHeading` + package-free
   `Actor::SetLinearVelocity`-style nudges via the per-frame tick.

Orbit parameters per NPC: direction (coin flip on entering circle),
angular speed (slight randomization), radius band (min/max around
weapon reach). Recomputed on slot transitions to avoid synchronized
motion.

### Runtime: tick and LOD

- Reuse `UpdateHooks` per-frame hook; AI director ticks per NPC at
  100 ms near (< 1000 units), 200 ms mid, 500 ms far — ECAI's LOD
  numbers, which are proven fine.
- Player-only targeting by default (`director.playerOnly = true`,
  config) so NPC-vs-NPC brawls stay vanilla.
- Cleanup: `Prune`, unload, death, and combat-end all release slots
  and reset bash/circle state alongside the existing stamina/poise
  resets.

## Configuration (new section `[Director]`)

| key | default | range |
|---|---|---|
| `director.enabled` | false (staged migration) | bool |
| `director.playeronly` | true | bool |
| `director.maxattackers` | 2 | 1..5 |
| `director.slotwindowminseconds` | 4 | 0.5..30 |
| `director.slotwindowmaxseconds` | 8 | 0.5..30 |
| `director.retaliationseconds` | 3 | 0..30 |
| `director.bashcooldownseconds` | 6 | 1..60 |
| `director.bashreactionchance` | 0.35 | 0..1 |
| `director.bashproactivechance` | 0.0 | 0..1 |
| `director.bashstaminacost` | 25 | 0..1000 |
| `director.maxcircleseconds` | 10 | 1..120 |
| `director.circleradiusmin` | 200 | 50..2000 |
| `director.circleradiusmax` | 320 | 50..2000 |

Shipped INI: `enabled = false` until the user playtests, same staged
rule as poise.

## Rollout order (this session and following)

1. **Design doc** (this file).
2. **Bash discipline core + tests** — cooldown, stamina cost,
   reaction chance; smallest piece that fixes the active pain.
3. **Slot ledger core + tests**.
4. **Runtime admission wiring** — bash/slot gates at begin; traces.
5. **Shield-turtle state machine** — block approach/circle/opening.
6. **Circling movement**.
7. **In-game validation**, then retire WYR + ECAI from the profile.

## Test plan

- `tests/AiRulesTests.cpp` (new CTest suite `ai_rules`):
  slot acquire/expire/retaliation-bypass; max-attackers clamp;
  bash cooldown enforcement; reaction chance 0 and 1 bounds;
  stamina admission rejection; state-machine transitions
  (advance→circle→commit on each opening trigger; max-circle commit).
- ConfigurationTests: all new keys (defaults, full custom, limits,
  invalid → diagnostics count bump).
- CoreTests/PoiseTests: unchanged (bash cost flows through existing
  admission; poise blocked fraction already tested).
- In-game: shield bandit fight — expect no bash spam, blocking
  approach, poise-break stagger opening, retaliation after player
  hits a circler.
