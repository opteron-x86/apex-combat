# Apex Combat — Living Status Document

This document is the handoff record for continuing work on Apex Combat.
Update it at the end of every session: mark completed work, record new
decisions with their evidence, and rewrite "Next steps" so a fresh session
can resume without re-deriving context.

Last updated: 2026-11-01 (session: milestone 0.3 round 1 — AiRules core + [Director] config shipped, director disabled by default)

## Project

Skyrim SE/AE/VR SKSE plugin (`ApexCombat`) implementing combat stamina
gating, BFCO/MCO attack admission, and perfect-block parries. Milestone
0.1 (issues #1–#5) is complete, packaged, and pushed. Milestone 0.2
(poise + counterattacks, issue #6) is shipped and validated in game.
Current work is milestone 0.3: the Combat AI Director (attack slots,
bash discipline, shield-turtle behavior) as a full native replacement
for Wait Your Turn Redux + Enhanced Combat AI.

- Repo: `https://github.com/opteron-x86/apex-combat`
- Branch: `feat/stamina-parry-foundation` @ `31fa9de` + uncommitted 0.3
  round (AiRules core + config); master merged and pushed at `31fa9de`.
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
- **Poise round (commit `d3ebd99`, pushed 2026-10-31) — milestone 0.2
  first half (issue #6)**: custom poise replacing the old owner, shipped
  **off by default** (`[Poise] enabled = false`) per the design doc's
  staged migration rule (old owner stays enabled until this is validated).
  - Core: `src/core/PoiseRules.h/.cpp` — per-actor pool, `PoiseKind`
    {Dagger, Sword, Axe, Mace, TwoHanded, Unarmed}, `PoiseSource`
    {Hit, BlockedHit, PerfectParry}, defaults (maxPoise 100, per-class
    light damage 20/25/30/35/40/15, power 1.5x, blocked 0.5x, flat 50
    parry damage ignoring kind, regen 20/s after 3000 ms delay, 1500 ms
    break recovery). Break staggers and ignores further poise damage
    inside the window (Chocolate Poise StaggerMode=1 analog); poise
    refills to max lazily when the window expires. `validate()` throws
    on non-finite/negative settings. `poise()` never inserts map entries.
  - Configuration: `[Poise]` section, 12 keys registered in
    `knownKey()`/`apply()` (maxPoise 1..100000, damages 0..10000,
    power 0..100, blocked 0..10, parry 0..10000, delay/recovery seconds
    0..60 converted to ms, regen 0..100000); `knownSection` gained
    "poise"; `Configuration` gained `poiseSettings` + `poise` fields.
  - Runtime: `PoiseKindFromWeapon` helper (GetAttackingWeapon →
    GetWeaponType mapping; null weapon → Unarmed for creatures);
    `ApplyDefenderPoise` helper (locks `stateMutex` — recursive — itself,
    traces `poise-damage`/`poise-break` with reason codes). `PreHit`
    restructured: gate-fail path applies unblocked poise damage
    (`staggerStart` on break); non-perfect block path chips at the
    blocked fraction; perfect-parry path deals flat parry damage to the
    attacker (break → `staggerStart` instead of `recoilStart`); all
    graph notifications stay after lock release. Wiring: `Prune`,
    `TESObjectLoadedEvent`, LoadingMenu/MainMenu handlers all reset
    poise alongside stamina rules; `InitializeRuntime` calls
    `poiseRules.setSettings` when enabled; capabilities log extended
    with `poise={}`.
  - INI: `[Poise]` section (all defaults, enabled=false) with a
    migration comment ("enable only after the previous poise mod is
    removed or disabled, or both systems will stagger").
  - README: poise capability bullet + migration note; config table
    gained 10 Poise rows; "not yet implemented" now lists For Honor
    adapters, projectile/spell eligibility, sprint/jump/dodge costs,
    poise HUD (owner undecided).
  - Tests: new `tests/PoiseTests.cpp` (5 suites: break/chain-guard,
    regen/damage-delay, multipliers/parry, validation/resets,
    replacement settings) registered as CTest `poise_rules`;
    ConfigurationTests gained a `poiseConfiguration()` block (defaults,
    full custom, limits, invalid expecting 11 diagnostics) and
    shipped-INI checks in the `argv[1]` block.
  - Test-arithmetic fixes made while verifying against the
    implementation: break at `now=3` sets `staggeredUntil == 1503`
    (not 1500); regen expectation 85 moved to `now=13000` (13500
    would be 95, capped 100); invalid poise diagnostics count is 11.
   - Validation: `tools/build.sh native-tests` 4/4 CTest suites +
     7 tools tests all green; `local-cross` rebuilt
     (`build/windows-local/ApexCombat.dll`); `tools/audit-dll.py`
     clean (exit 0); re-packaged `dist/` (dll inside zip sha256
     `3a3b7ea2...`, differs from the `f0ab372` build `849120e7...`).
- **TrueHUD poise bar round (this session, uncommitted→committed
  below)**: vendored `third_party/truehud/TrueHUDAPI.h` (copied from
  the installed SCAR source; TrueHUD 1.1.9.0 installed, API V1).
  `ConnectTrueHud()` requests V1, takes special-bar control (warns and
  degrades on `AlreadyTaken`), registers current/max callbacks
  (recursive `stateMutex` lock; `PoiseResource` helper). Bar flashes:
  `FlashPoiseResult` sends a short flash on applied poise damage and a
  long flash on a break, wired into all three `PreHit` branches after
  lock release (`ApplyDefenderPoise` now returns `Core::PoiseResult`).
  Config: `poise.hud` (default true, only consulted when poise is on);
  capabilities log gained `TrueHUD poise bar={}`. **Poise flipped to
  enabled in the shipped INI** (`[Poise] enabled = true`): the user
  retired Chocolate Poise; code default stays false so a missing
  config cannot double-stagger.
- **Perfect-parry feedback SFX round (this session)**: user asked
  whether For Honor or Sekiro Combat effects could be used. Mutagen
  dump (active profile `KINMUNE CS TEST`) showed `SekiroCombat_II.esp`
  exposes no MGEF/SPEL records (script/native-driven), while
  `For Honor Balance Patch.esp` carries the right sound descriptors:
  `0x82D` SNDR `WeaponParrySFX`, `0x82C` SNDR `ShieldParrySFX`,
   `0x868` SNDR `ParryStance`. (The `WeaponParryTrigger` etc. MGEFs are
   script-typed containers gated by conditions — not directly usable.)
   Implemented sound playback instead of spell application:
   `perfectblock.parrySoundWeapon` / `parrySoundShield` config keys
   (`Plugin.esp|0xFormID` specs, empty disables). **Defaults are vanilla
   records — `Skyrim.esm|0x3C73C` `WPNBlockBlade1HandVsOtherSD` (weapon)
   and `Skyrim.esm|0x3C740` `WPNBlockShieldLSD` (shield)** — after the
   user pointed out `For Honor Balance Patch.esp` is not in their main
   load order (it is only enabled in `KINMUNE CS TEST` and `KINMUNE II`,
   not `KINMUNE CS`), making a For Honor default degrade silently to
   nothing. No companion ESP needed for SFX (Skyrim audio always plays
   through a plugin form, and vanilla forms cover it); an ESP is only
   worth revisiting for the milestone 0.2 counterattack adapter records,
   which genuinely need one. For Honor users can repoint via the INI
   (shipped INI documents both For Honor specs as comments).
   `ResolveConfiguredSound` parses/validates at init
   (warn-once, `FormType::SoundRecord` check, 0 on failure);
  `PlayParryFeedback` plays via `BSAudioManager::Play(FormID)`, picks
  the shield variant when `GetWornArmor(kShield)`, and only fires when
  the attacker or defender is the player (avoids distant 2D clangs).
  Gated on the previously-unused `parried` flag in `PreHit`.
  Capabilities log gained `parry sfx={}`.
  - Tests: ConfigurationTests defaults/full-override/shipped-INI
    coverage for both new keys; `tools/build.sh native-tests` 4/4
    CTest suites + 7 tools tests green; `local-cross` rebuilt;
     `tools/audit-dll.py` clean; re-packaged `dist/` (dll inside zip
     sha256 `819de127...` with the vanilla defaults).
  - README: poise paragraph rewritten (enabled-in-shipped-INI, TrueHUD
    bar, parry clangs); config table gained `poise.hud` and both
    parry-sound rows; "not yet implemented" no longer lists the poise
    HUD.
- **Perfect-parry spark VFX round (this session)**: layered a visual on
  top of the working clang. Went through three iterations and two
  distinct root causes before landing a clean, minimal implementation.
  - **Final design (Option A — engine-driven spark).** `PlayParryFeedback`
    spawns a single `BSTempEffectParticle::Spawn(cell, 0.0f,
    "ApexCombat\\impact{Weapon,Shield}Root.nif", rotate, position, 1.0f,
    4U, node)` at the blocking weapon/shield node. Those root NIFs are
    byte-exact copies of vanilla `fxmetalsparkimpact01.nif`; attaching
    the `FXMetalSparkImpact01`/`AddOnNode52` root makes **Skyrim itself
    fire the vanilla metal-spark impact** at that node — the same flash
    players see on a real block. Same recipe Elden Parry uses (its DLL
    spawns only its own renamed copy of that vanilla NIF; verified by
    disassembly). No custom spark/flare NIFs, no emission patching.
  - **RCA #1 — the "instant crash" was the SOUND, not the VFX.** Two
    CrashLogger logs (`crash-2026-11-01-00-29-21`, `-00-35-49`) both
    showed `EXCEPTION_ACCESS_VIOLATION` null-read (`mov rax,[rcx]`,
    `rcx=0`) in the audio manager resolving a `BGSSoundDescriptorForm`
    from `ApexCombat.esp` — the missing-CNAM SNDR bug fixed earlier.
    Cause: the MO2 mod folder had a **stale pre-fix `ApexCombat.esp`**
    (sha256 `f6eecd59...`); the fixed ESP (`1ff4a84f...`) had never been
    copied over it. Redeployed the fixed ESP → crash gone. Lesson:
    verify the deployed ESP sha, not just the DLL, after each round.
  - **RCA #2 — "sound but no visual" was corrupted root NIFs.** The C++
    spawn was always correct (identical to Elden Parry). All four Apex
    `impact{Weapon,Shield}Root.nif` files (mod dir + package) were
    structurally broken vs vanilla: `BSXFlags=4` (wrongly marks the
    effect root a skeleton NIF; vanilla=2), `BSValueNode.Value=901`
    (references a non-existent attachment; vanilla=52=`AddOnNode52`),
    and a bloated string table. Engine looked up attachment 901, found
    nothing, rendered zero. Fixed by replacing all four with exact
    vanilla `fxmetalsparkimpact01.nif` bytes (593 B, sha256
    `2dd35d9f...`) — pure asset swap, no rebuild.
  - **Cleanup.** Option A does not spawn `sparks.nif` /
    `sparksflarerings.nif` / `RingsFlare.dds`, so those dead assets were
    removed from `package/` and the live mod dir; stale "ring flare /
    sparks burst" comments corrected in `Configuration.h` and the INI.
  - Reference mods studied (installed 2026-11-01): **Elden Parry**
    (dTry; SKSE DLL, the architecture template), **Valhalla Combat**,
    **Parry For All - ImpactFX** (ToyzFX; source of the earlier
    spark/flare NIF experiments).
  - Validation: user confirmed in-game — spark flash now appears on
    parry with the clang, no crash, no lingering orb. `local-cross`
    rebuilt (dll sha256 `f150c0a9...`), `tools/audit-dll.py` clean,
    `tools/package.py` re-packaged `dist/` (zip ESP `1ff4a84f...`, NIFs
    `2dd35d9f...`, SHA256SUMS refreshed).
  - README: perfect-parry paragraph now mentions the spark flash;
    config table gained `parrySoundVolume` and `parryVisual` rows.
  - Not perfect yet (user): spark visual could be tuned further (size/
    position/optional fuller ring flare). Parked as a follow-up polish
    item — see "Open items".
- **Counterattack window round (2026-11-01, validated in game)** —
  perfect parry grants the defender a bounded window (default 1500 ms)
  where the next power attack costs 0 stamina and all attacks deal
  bonus poise damage:
  - Free power attack: `grantCounterattack`/`counterattackReady` core
    state, INI `[CounterAttack] enabled/windowMs`. Reward semantics are
    **spend on commit**: any non-commit ending (supersede by engine
    retry, rejection, cancellation, window expiry) re-arms it via the
    `rearmFreeCounterattack` helper; expired windows are never
    resurrected. Runtime recaptures `freePowerBaseline` and rebinds
    `freePowerStage` on every consuming begin; `SettleFreeCounterattack`
    is guarded by `freePowerStage != 0`; settles happen on
    `attackrelease`, `attackstop`/`staggerstart`/`recoilstart`, and a
    per-frame fallback in `UpdateHooks::Hook` (before the bow-drain
    early returns). Root cause of the original "counter power attack
    still drains stamina" bug: MCO/BFCO fires multiple begin events per
    swing (admit → dispatch-failed retries → commit), the engine drain
    lands after commit, and the earlier build consumed the reward on
    the first retry begin without refunding.
  - Poise bonus: `PoiseSettings::counterattackPoiseMultiplier`
    (default 1.25, validated >= 1.0); `PoiseRules::applyDamage` gained
    `float damageMultiplier = 1.0F` (applied after the power
    multiplier, before the block reduction; the flat perfect-parry
    counter-damage ignores it; NaN/negative rejected);
    `CombatRules::counterattackActive` (true while the window is open,
    even after the reward is spent); runtime `ApplyDefenderPoise`
    passes the multiplier when the attacker's window is active.
    INI key `poise.counterattackpoisemultiplier` (range 1..100).
  - Tests: `counterattackRewardSurvivesRetries` +
    `counterattackActiveTracksOpenWindow` (CoreTests),
    `counterattackPoiseBonus` (PoiseTests), ConfigurationTests
    defaults/full/limits/invalid extended (invalid diagnostics 11→12).
    All green: 238 combat + 57 poise + 233 config checks, parry
    contacts, 7 tools tests.
  - Build/deploy: `local-cross` rebuilt (dll sha256 `9d04f192...`),
    `tools/audit-dll.py` clean, `tools/package.py` re-packaged `dist/`
    (zip sha256 `216e88c2...`), DLL + INI synced to
    `/mnt/mods/Amaranth/mods/Apex Combat/SKSE/Plugins/` (hashes
    verified).
  - **In-game validation (user, 2026-11-01)**: trace shows the
    counterattack power attack after a perfect parry admits at
    `amount=0` and commits via `commit-engine-cost` with no stamina
    drain, and window poise damage carries the bonus (e.g. NPC dagger
    light hits `amount=18.75` = 15 × 1.25). User confirmed: "playtest
    looks good — no stamina drain on counter power attack after a
    perfect parry."
- **Milestone 0.3 round 1 (uncommitted)** — Combat AI Director core +
  config (full design in `docs/MILESTONE-0.3-combat-ai-director.md`):
  - Research: incumbent comparison from installed configs + source-level
    analysis of ECAI (github.com/tavvfiq/combat-ai-ng, cloned at
    `/tmp/opencode/combat-ai-ng`). ECAI has no native movement (CPR
    graph vars), no sustained block, 100ms-polled attack detection.
  - Core (`src/core/AiRules.h/.cpp`, CTest suite `ai_rules`): slot
    ledger per target (lazy expiry, randomized window in
    [slotWindowMinMs, slotWindowMaxMs], holder re-acquire keeps expiry,
    retaliation window bypasses the count exactly once and is consumed),
    bash discipline (per-actor cooldown recorded at begin, `cancelBash`
    only cancels the matching recordedAt, reaction/proactive chance
    rolls with injected samples). `AiSettings` validation (1..5
    attackers, min<=max window, finite chances 0..1, non-negative cost,
    maxCircleMs > 0); `resetActor` / `resetSession`.
  - Configuration: `[Director]` section with 13 keys (enabled,
    playerOnly, maxAttackers, slot window min/max seconds, retaliation
    seconds, bash cooldown/reaction/proactive/stamina cost, max circle
    seconds, circle radius min/max) — all with ranges and seconds->ms
    conversion; `Configuration` gained `aiSettings`, `director` (false),
    `directorPlayerOnly` (true), `circleRadiusMin/Max` (200/320).
  - Shipped INI: `[Director]` block added with defaults and
    `enabled = false` (staged migration: no behavior change until
    playtested).
  - Tests: 17 AiRules test functions (custom check-macro style);
    ConfigurationTests gained `directorConfiguration()` (defaults,
    full custom, limits, invalid count 11, AiRules cross-validation)
    plus shipped-INI director assertions. All green: 5 CTest suites +
    7 tools tests.
  - Build/deploy: `local-cross` rebuilt, `tools/audit-dll.py` clean,
    `tools/package.py` re-packaged `dist/`, DLL + INI synced to
    `/mnt/mods/Amaranth/mods/Apex Combat/SKSE/Plugins/`.
  - README: 12 Director config table rows added.
- **Milestone 0.3 round 2 (uncommitted)** — runtime admission wiring in
  `src/runtime/Runtime.cpp`:
  - `aiRules` instance alongside `rules`/`poiseRules`; setSettings at init
    (unconditional — bash stamina cost applies even with the director off),
    resetActor on unload, resetSession on load/main-menu.
  - Bash discipline: `BashEvent()` matcher (`bashstart`, `bashpowerstart`,
    `mcobashstart`, `bfco_bashstart`); `BeginBash()` gates NPC bashes through
    `admitBash` (cooldown trace `bash-denied-cooldown` with remaining ms,
    stamina trace `bash-denied-stamina`), then rides the ordinary
    admission/commit path via the DualWield custom-cost kind with
    `bashStaminaCost` as the nominal cost — SCAR rejection, exhaustion,
    and the existing regen delay all apply unchanged. `bashPending` latch +
    `CancelPendingBash()` restore the cooldown on rejected/superseded
    begins, dispatch failure, attackstop/stagger/recoil, matching the
    counterattack re-arm pattern. Player bashes skip the cooldown but pay
    the stamina cost.
  - Attack slots: `DirectorTarget()` resolves the NPC's combat target via
    `Actor::combatController->targetHandle`; gated on `config.director`,
    non-player attacker, and `directorPlayerOnly` (player targets only).
    Slot acquire (`tryAcquire` with `RandomUnit()` window sample) happens
    before admission; denial returns false with trace `slot-denied`, and
    a retaliation grant traces `slot-retaliate`. The slot is released only
    if `Begin` fails afterwards; otherwise it lives for its window (lazy
    expiry/rotation in the ledger).
  - Retaliation arming: PreHit unblocked branch calls
    `onTargetDamagedAttacker(target=player, attacker=NPC)` when a
    director-scoped target damages an NPC.
  - `RandomUnit()`: `std::mt19937` + `generate_canonical<float,24>` in
    [0,1) for window draws (reaction/proactive rolls are still pure-core
    and unwired — the turtle state machine round consumes them).
  - Capability log line gained `director={}`.
  - Build: native tests green (5 CTest + 7 tools), local-cross linked
    clean, audit-dll clean (x64, 3 exports), re-packaged, DLL synced to
    the MO2 mod dir (sha256 verified).
- **Milestone 0.3 round 3 (uncommitted)** — shield-turtle state machine +
  circling movement, plus round-2 hardening fixes:
  - Fix: bash dispatch failure and interrupt paths previously cancelled
    `state.stage`, which could cancel an interleaved attack's token.
    `ActorState.bashStage` now latches the bash's own admission token;
    `Commit` clears it; dispatch-failure and attackstop/stagger/recoil
    cancel exactly it (skipped when the bash cost nothing).
  - Fix: bash branch in the Notify hook now checks `IsStaggered()` like
    the attack path.
  - `AiRules::isOpening()`: pure turtle-opening predicate (target
    recovering/staggered/exhausted, live retaliation, or max-circle
    timeout). 9 new checks in `turtleOpenings()`.
  - `ActorState` turtle fields: `turtleCircling`, `turtleCircleSince`,
    `turtleClockwise`, `turtleDirection`, `directorLastTick`.
  - Turtle driver (`TurtleStop`/`TurtleTick`/`DirectorTick`):
    - Entry: blocking NPC with a left-hand shield, no slot held, within
      1.5x circleRadiusMax of a director-scoped target; random clockwise
      coin-flip; traces `turtle-enter`.
    - Circling: per-tick graph-var strafe (`Speed`=90, `Direction`,
      `movementDirection`, `movementSpeed`, `InputDirection`,
      `InputMagnitude` — ECAI's proven variable set) perpendicular to the
      target bearing, curving inward/outward toward the
      [circleRadiusMin, circleRadiusMax] band; re-asserts `blockStart`
      when block drops.
    - Commit: on any opening (target mid-recovery = uncommitted attack
      stage, stagger, exhaustion, live retaliation, or maxCircleMs
      timeout) stops circling and fires a vanilla `attackStart`, which
      routes through the ordinary admission + slot gate; traces
      `turtle-commit`.
    - Tick banding in the per-frame `Actor::Update` hook (NPC vtable):
      100 ms within 1000 u of the player, 200 ms to 2000 u, skipped
      beyond (directorLastTick).
    - Unload while circling zeroes the locomotion writes before the
      actor's state is erased.
  - In-game validation of rounds 1-2 (user): WYR + ECAI disabled,
    `director.enabled=true` — bash spam visibly reduced, bash stamina
    costs landing, trace logs correct. This also confirmed bash commits
    fire prehitframe/weaponswing (the debit lands).
  - Build: native tests green (6 CTest suites incl. new turtle checks +
    7 tools), local-cross linked clean, audit-dll clean (x64, 3 exports),
    re-packaged, DLL synced to the MO2 mod dir (sha256 verified).

## Next steps (execute in order)

1. Milestone 0.3 (design: `docs/MILESTONE-0.3-combat-ai-director.md`):
   runtime admission wiring in `src/runtime/Runtime.cpp` — `AiRules`
   instance + mutex-protected bash gate at bash begins (with
   `cancelBash` on SCAR rejection / supersede / cancellation / expiry,
   same re-arm pattern as counterattacks), bash stamina debit through
   the existing `beginAttack`/`commitAttack` path, slot acquire/release
   on NPC attack begins/commits targeting the player,
   `onTargetDamagedAttacker` from PreHit, traces
   (`bash-denied-cooldown`/`bash-denied-stamina`/`bash-reaction`).
   Then shield-turtle state machine, circling movement, in-game
   validation, retire WYR + ECAI.
2. Milestone 0.2 remainder (issue #6): For Honor counterattack effect
   adapters (trace the actual plugin records — FormKeys 0x5F5/0x89F/
   0x8BD via Variable01/Variable03 per the audit doc). Poise HUD owner
   is settled: TrueHUD special resource bar shipped in `88dea79`.
3. Consolidation roadmap follow-ups (user's 3819-mod setup, after 0.3):
   stamina regen + stagger knobs, per-enemy styles, SCAR-AE fold-in
   (github.com/doodlum/SCAR-AE) as a separate later decision.

## Open items / parked

- MO2 deployment apply is pending a user decision (preview-only so far).
- Poise (milestone 0.2 first half) is shipped and validated in game
  (TrueHUD bar, poise damage/break, counterattack window bonus).
  Remaining milestone 0.2 work: For Honor counterattack effect
  adapters.
- Player-only vs NPC scope for bow drain: default drains both player
  and NPCs; if user wants player-only, gate with `IsPlayerRef()`.
- Separate pullback-vs-full-draw bow drain rates: current build uses
  one rate (`stamina.bowdrawdrainpersecond`) for the whole draw; add a
  second key only if the user asks.
- Parry spark VFX polish (Option A shipped, user: "decent, not
  perfect"): candidate tweaks are spark size/position and re-adding a
  fuller ring-flare layer on top of the vanilla spark. The removed
  `sparks.nif`/`sparksflarerings.nif`/`RingsFlare.dds` experiments are
  recoverable from git history if a layered look is wanted later.
- Watch for idle-hold drain: `kBowAttached` is assumed to be a
  transient pullback state; if the user reports stamina draining while
  idling with a nocked bow, gate the `kBowAttached` branch behind a
  seen-draw-start latch.
- In-game validation of the bow pullback fix was folded into the
  2026-11-01 playtest (no drain complaints); consider it accepted
  unless the user reports otherwise.
