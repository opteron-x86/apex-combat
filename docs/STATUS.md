# Apex Combat — Living Status Document

This document is the handoff record for continuing work on Apex Combat.
Update it at the end of every session: mark completed work, record new
decisions with their evidence, and rewrite "Next steps" so a fresh session
can resume without re-deriving context.

Last updated: 2026-10-31 (session: bow pullback drain fix shipped)

## Project

Skyrim SE/AE/VR SKSE plugin (`ApexCombat`) implementing combat stamina
gating, BFCO/MCO attack admission, and perfect-block parries. Milestone
0.1 (issues #1–#5) is complete, packaged, and pushed. Current work is
stamina features from in-game testing, leading toward milestone 0.2
(issue #6: poise + For Honor counterattacks).

- Repo: `https://github.com/opteron-x86/apex-combat`
- Branch: `feat/stamina-parry-foundation` @ `f0ab372` (pushed, tree clean
  at time of last session end)
- Base: `b11dc99`
- Do NOT commit unless explicitly asked; the user has authorized commits
  so far (commit + push after each feature round).

## Environment / commands

- Build presets: `native-tests` (ASan/UBSan test suite),
  `windows-release`, `linux-cross`; user preset `local-cross`
  (output `build/windows-local/ApexCombat.dll`).
- CommonLib pinned: `build/deps/CommonLibSSE-NG` @
  `12d61f3c39f7d4ba1b1ab543df2bca0e613dc747`
- Cross env: `XWIN_SYSROOT=build/toolchain/xwin`,
  `APEX_WINDOWS_PREFIX=build/toolchain/windows-libs`
- Tests: `tools/build.sh native-tests` (CTest: combat_rules,
  configuration, parry_contacts + 7 tools unit tests)
- Packaging: `tools/package.py` → `dist/Apex-Combat-0.1.0.zip`
  (DLL, INI, README, licenses, build-info, SHA256SUMS) + symbols/source
  zips. Flag `dirty: true` appears if tree has uncommitted changes.
- Deploy (MO2, preview-only so far): plan at
  `/tmp/opencode/apex-deploy-plan.json` targets `KINMUNE CS` profile,
  mods dir `/mnt/mods/Amaranth/mods`, mod name "Apex Combat".
  Apply: `python3 tools/deploy.py --plan /tmp/opencode/apex-deploy-plan.json
  --backup-dir /tmp/opencode/apex-backup --apply`
  Rollback: `--rollback <snapshot> --backup-dir /tmp/opencode/apex-backup`
  NOT yet applied — user has not decided.
- GitHub works: `gh` authenticated as `opteron-x86` via `GH_TOKEN`
  (exported before launching opencode). Pushes over SSH
  (`~/.ssh/opteron-repo` ED25519 key loaded in agent).

## Verified API facts (do not re-research)

All verified against `build/deps/CommonLibSSE-NG/include/` at the pinned
commit:

1. **Bow state**: `actor->AsActorState()->GetAttackState()` returns
   `ATTACK_STATE_ENUM` (`RE/A/ActorState.h:156`), values include
   `kBowDraw=8`, `kBowAttached=9`, `kBowDrawn=10`, `kBowReleasing=11`
   (`ActorState.h:23-37`). Empirical (user's setup, 2026-10-31): the
   pullback animation runs mostly as `kBowAttached`, not `kBowDraw` —
   `kBowDraw` is only a brief moment at draw start. Plain struct read —
   VR-safe.
2. **Per-frame hook**: `Actor::Update(float)` is vtable slot `0xAD`
   (`RE/A/Actor.h:401`), marked `SKYRIM_REL_VR_VIRTUAL` (slot exists in
   VR at the same index). `PlayerCharacter` does NOT override
   `Update(float)` (`RE/P/PlayerCharacter.h` has no Update(float)), so
   hooking `VTABLE_Character[0]` slot 0xAD (NPCs) and
   `VTABLE_PlayerCharacter[0]` slot 0xAD (player) covers both.
   `VTABLE_Character`/`VTABLE_PlayerCharacter` VariantIDs include VR
   (`RE/Offsets_VTABLE.h:2215,2231`). CommonLib helper:
   `REL::Relocation<std::uintptr_t>{...}.write_vfunc(0xAD, fn)`
   (`REL/Relocation.h:782`) — same pattern as existing GraphHooks
   (`src/runtime/Runtime.cpp:247-250`). Repo precedent: vtable-vfunc
   writes only, no `REL::RelocationID`.
3. **Weapon types** (`RE/T/TESObjectWEAP.h:44`): `kHandToHandMelee=0,
   kOneHandSword=1, kOneHandDagger=2, kOneHandAxe=3, kOneHandMace=4,
   kTwoHandSword=5, kTwoHandAxe=6, kBow=7, kStaff=8, kCrossbow=9`.
4. **Regen delay**: `actor->GetActorRuntimeData().currentProcess->
   UpdateRegenDelay(RE::ActorValue::kStamina, seconds)` — existing
   precedent at `src/runtime/Runtime.cpp:164`.
5. **Stamina restore**: `ActorValueOwner::RestoreActorValue(ActorValue,
   float)` — 2-arg form (pinned CommonLib).
6. CommonLib-NG `AGENTS.md` (in build/deps) describes THAT repo's rules;
   ignore its contribution workflow — we only consume the library.

## User requirements (in-game testing feedback, authoritative)

1. **Exhaustion**: at 0 stamina, a short delay before stamina recovery
   may begin (`exhaustionRecoveryDelaySeconds`, default ~1.0).
2. **Post-action regen delay**: short delay (options discussed:
   0.33/0.5/0.64 s) after executing an attack OR a dodge before stamina
   recovers. Attack delay exists (`regenDelaySeconds` 0.5, applied on
   commit); **dodge delay is missing** (add `dodgeRegenDelaySeconds`).
3. **Bow draw drain**: drain stamina while holding a bow draw (design
   docs: Gravitas spec 5/sec bow draw; combat sneaking 1/sec).
4. **Dual wielding** costs more than a single weapon (currently flat 20;
   should scale higher).
5. **Per-weapon-type costs**: daggers cheaper, distinct costs for other
   types (sword/axe/mace/two-handed/unarmed).

Constraint from design docs: DMCO owns dodge *cost* (25) during the
prototype — we add only a dodge regen delay, never a dodge charge.

## Agreed design (settled, ready to implement)

### Core (`src/core/CombatRules.h/.cpp`)
- Extend `AttackKind` with `Dagger`, `OneHandedAxe`, `OneHandedMace`,
  `DualWield` (additive; existing values/tests unchanged).
- `Settings` additions: `daggerCost{10.0F}`, `axeCost{25.0F}`,
  `maceCost{25.0F}`, `dualWieldMultiplier{1.6F}`. Existing
  `lightOneHandedCost` remains the sword cost; `lightTwoHandedCost`
  remains two-handed (legacy INI keys stay valid).
- `beginAttack` gains a `customCost` parameter (default 0) used when
  `kind == DualWield`; runtime computes
  `(leftCost + rightCost) * 0.5F * dualWieldMultiplier` from
  `rules.settings()` and per-hand weapon classes. Invalid customCost
  for DualWield → `RejectReason::InvalidInput`. Validate new fields in
  `validate()` (`validCost` for costs; multiplier finite and >= 1.0F).

### Configuration (`src/core/Configuration.h/.cpp`)
New keys (section `stamina`), all float ranges 0..10000 for costs,
0..10 for delay-seconds, multiplier 1..10:
- `stamina.daggercost`, `stamina.axecost`, `stamina.macecost`
- `stamina.dualwieldmultiplier`
- `stamina.dodgeregendelayseconds` (Configuration field
  `dodgeRegenDelaySeconds{0.5F}`)
- `stamina.exhaustionrecoverdelayseconds` (field
  `exhaustionRecoveryDelaySeconds{1.0F}`)
- `stamina.bowdrawdrainpersecond` (field
  `bowDrawDrainPerSecond{5.0F}`)
Update `knownKey()` and `apply()`; remember first-occurrence-wins and
diagnostic behavior are already generic.

### Runtime (`src/runtime/Runtime.cpp`)
- Rework `Kind()` into full per-weapon classification:
  - both hands `TESObjectWEAP` melee → `DualWield` (+ computed cost)
  - single weapon by type: Dagger/Sword/Axe/Mace/TwoHanded/Unarmed
  - ranged (bow/crossbow/staff) already excluded via `Ranged()`
- **Exhaustion delay**: track a per-actor `exhausted` bool in the
  Runtime `ActorState`; wherever stamina observation reports an
  entered-exhaustion transition (Begin result, `rules.observeStamina`
  return in Commit, successfulBlock result), apply
  `UpdateRegenDelay(kStamina, config.exhaustionRecoveryDelaySeconds)`
  on the false→true flip only.
- **Dodge delay**: in `IncomingEvent`/graph processing, match dodge
  events (pattern like "dodge" + "start", e.g. DMCO `*dodge*start*`)
  BEFORE the `ManageAttack(actor) || Ranged(actor)` early return, and
  apply `UpdateRegenDelay(kStamina, config.dodgeRegenDelaySeconds)`.
  No dodge charge.
- **Bow drain**: new `UpdateHooks` struct installing
   `write_vfunc(0xAD, ...)` on `VTABLE_Character[0]` and
   `VTABLE_PlayerCharacter[0]` (store original pointer, call it first).
   In the hook: if `StableGameplay() && EligibleActor(actor)` and
   `GetAttackState()` is `kBowDraw`, `kBowAttached`, or `kBowDrawn`:
  drain `config.bowDrawDrainPerSecond * delta` via
  `DamageActorValue(kStamina, ...)` and re-apply
  `UpdateRegenDelay(kStamina, config.regenDelaySeconds)` each frame so
  regen stays paused while drawing (after release, the last 0.5 s gate
  applies naturally). Hook runs on the game thread; guard with
  `stateMutex` where touching shared state. Install in
  `InitializeRuntime()` alongside GraphHooks; log in Capabilities line.

### Tests
- `tests/CoreTests.cpp`: costs for Dagger/OneHandedAxe/OneHandedMace;
  DualWield accepted with valid customCost (nominalCost == customCost,
  debit applied); DualWield with invalid customCost → InvalidInput;
  DualWield without customCost → InvalidInput; multiplier validation
  (multiplier < 1.0 → invalid settings).
- `tests/ConfigurationTests.cpp`: parse of all new keys; invalid values
  retain defaults; unknown keys still diagnosed.

### Docs
- `README.md` config table: add all new keys with defaults.

## Completed so far (do not redo)

- Milestone 0.1 features: configurable stamina costs, BFCO/MCO attack
  admission (`beginAttack`/`commitAttack` stage tokens), perfect-block
  recovery (Precision PreHit + parry window/rearm), ParryContacts
  dedup ledger (`suppress-parried-contact`, `duplicate-parry-contact`),
  runtime builds + archives (`dist/Apex-Combat-0.1.0.zip`), MO2 deploy
  preview. All in commit `2430775`, pushed.
- `RestoreActorValue` 2-arg fix; README.md + THIRD_PARTY_NOTICES.md
  written; gh auth + SSH push verified working.
- **Stamina feature round (commit `29dcb5f`, pushed 2026-10-31)** —
  implemented exactly per the design section above; no deviations
  except where noted:
  - Core: `AttackKind::Dagger/OneHandedAxe/OneHandedMace/DualWield`;
    `Settings` cost/multiplier fields; `beginAttack(..., float
    customCost = 0.0F)` (DualWield uses customCost; invalid/NaN/inf/
    <= 0 → InvalidInput); unmapped weapon kinds fall back to
    `LightOneHanded`.
  - Configuration: 7 new `[Stamina]` keys registered in `knownKey()`
    and `apply()` (ranges: costs 0..10000, multiplier 1..10, delays
    0..10, `bowdrawdrainpersecond` 0..10000).
  - Runtime: full per-weapon `Kind()` classification (DualWield =
    averaged per-hand cost × `dualWieldMultiplier`); exhaustion
    `UpdateRegenDelay` on false→true flips (Begin, Commit
    `observeStamina`, PreHit successfulBlock); dodge event delay
    before the `ManageAttack || Ranged` early return;
    `UpdateHooks<Player>` vtable slot 0xAD hook on `Character` +
    `PlayerCharacter` draining `bowDrawDrainPerSecond × delta` while
    `GetAttackState()` ∈ {kBowDraw, kBowAttached, kBowDrawn}
    (kBowAttached added in follow-up commit `f0ab372`, see below);
    capabilities log extended with `bow draw drain={}`.
  - INI + README config table updated with all new keys.
  - Tests: `perWeaponCostsAndDualWield` in CoreTests; extended
    defaults/full/limits/invalid coverage in ConfigurationTests.
  - Validation: `tools/build.sh native-tests` all green (3 CTest
    suites + 7 tools tests, ASan/UBSan); `local-cross` rebuilt
    (`build/windows-local/ApexCombat.dll`); `tools/audit-dll.py`
    clean (x64, 3 SKSE exports, expected imports); `tools/package.py`
    re-packaged `dist/` (runtime layouts SE/AE/VR).
  - Bug fixed during testing: `knownKey`/`apply` had a typo
    `stamina.exhaustionrecoverdelayseconds` (missing "y" in
    "recovery"); corrected to
    `stamina.exhaustionrecoverydelayseconds` — this key was the cause
    of the initial configuration test failure (isolated with a
    standalone repro printing the diagnostics).
- **In-game validation round 1 (user feedback, 2026-10-31)**: per-weapon
  costs, dual-wield multiplier, other attacks, and dodge all confirmed
  working in-game. Bow behavior needed a fix (below).
- **Bow pullback drain fix (commit `f0ab372`, pushed 2026-10-31)** —
  user reported: no drain during the draw pullback animation, only a
  ~1-stamina drop at draw start, then 5/sec at full draw. Root cause:
  the pullback runs as `kBowAttached` (state 9), which the drain
  condition didn't cover; the 1-point drop was our drain during the
  brief `kBowDraw` frames at draw start. Fix: added `kBowAttached` to
  the `UpdateHooks` drain condition — now drains continuously at
  `bowDrawDrainPerSecond` from draw start through the pullback and the
  full-draw hold. Release states (`kBowReleasing`/`kBowReleased`)
  remain excluded. Note: drain still uses the single
  `stamina.bowdrawdrainpersecond` value for both pullback and full
  draw (user's wording implied one rate; if they want a separate
  pullback rate, add a new INI key).
  - Validation: `tools/build.sh native-tests` all green; `local-cross`
    rebuilt; `tools/audit-dll.py` clean; re-packaged `dist/` (dll sha256
    `849120e7...`, differs from the `29dcb5f` build `6c21e5e7...`).

## Next steps (execute in order)

1. Wait for user in-game validation of the bow pullback drain (commit
   `f0ab372` build) and the remaining stamina behaviors.
2. Deploy the fresh build to MO2 when the user decides (deploy plan
   and rollback are documented above; preview only so far).
3. Milestone 0.2 (issue #6): poise + For Honor counterattacks.

## Open items / parked

- MO2 deployment apply is pending a user decision (preview-only so far).
- Milestone 0.2 (issue #6): poise + For Honor counterattacks — next
  after this stamina round.
- Player-only vs NPC scope for bow drain: default drains both player
  and NPCs; if user wants player-only, gate with `IsPlayerRef()`.
- Separate pullback-vs-full-draw bow drain rates: current build uses
  one rate (`stamina.bowdrawdrainpersecond`) for the whole draw; add a
  second key only if the user asks.
- Watch for idle-hold drain: `kBowAttached` is assumed to be a
  transient pullback state; if the user reports stamina draining while
  idling with a nocked bow, gate the `kBowAttached` branch behind a
  seen-draw-start latch.
- In-game validation of the bow pullback fix still required from the
  user after build+deploy.
