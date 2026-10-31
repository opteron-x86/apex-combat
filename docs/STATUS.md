# Apex Combat — Living Status Document

This document is the handoff record for continuing work on Apex Combat.
Update it at the end of every session: mark completed work, record new
decisions with their evidence, and rewrite "Next steps" so a fresh session
can resume without re-deriving context.

Last updated: 2026-10-31 (session: stamina features round 2)

## Project

Skyrim SE/AE/VR SKSE plugin (`ApexCombat`) implementing combat stamina
gating, BFCO/MCO attack admission, and perfect-block parries. Milestone
0.1 (issues #1–#5) is complete, packaged, and pushed. Current work is
stamina features from in-game testing, leading toward milestone 0.2
(issue #6: poise + For Honor counterattacks).

- Repo: `https://github.com/opteron-x86/apex-combat`
- Branch: `feat/stamina-parry-foundation` @ `2430775` (pushed, tree clean at
  time of last session start)
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
   `kBowDraw=8`, `kBowDrawn=10`, `kBowReleasing=11` (`ActorState.h:23-37`).
   Plain struct read — VR-safe.
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
  `GetAttackState()` is `kBowDraw` or `kBowDrawn`:
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
- `RestoreActorValue` 2-arg fix; native tests green (3 CTest suites +
  7 tools tests, ASan/UBSan); DLL audit clean (VR layout on, exports
  OK, release CRT).
- README.md + THIRD_PARTY_NOTICES.md written.
- gh auth + SSH push verified working.

## Next steps (execute in order)

1. Implement Core changes (CombatRules.h/.cpp per design above).
2. Implement Configuration changes (Configuration.h/.cpp).
3. Implement Runtime changes (Runtime.cpp: classification, exhaustion
   delay, dodge delay, UpdateHooks bow drain).
4. Update `package/SKSE/Plugins/ApexCombat.ini` + README config table.
5. Extend tests; run `tools/build.sh native-tests`.
6. Rebuild `local-cross`, audit, `tools/package.py`.
7. Commit (user has authorized) and push.
8. Update this document: move steps to Completed, record test results
   and any deviations from the design.

## Open items / parked

- MO2 deployment apply is pending a user decision (preview-only so far).
- Milestone 0.2 (issue #6): poise + For Honor counterattacks — next
  after this stamina round.
- Player-only vs NPC scope for bow drain: default drains both player
  and NPCs; if user wants player-only, gate with `IsPlayerRef()`.
- In-game validation of all new behaviors still required from the user
  after build+deploy.
