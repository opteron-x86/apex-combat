# Apex Combat

A CommonLibSSE-NG native plugin for Skyrim Special Edition / Anniversary Edition / VR (1.6.1170-class runtimes) that owns a small set of combat rules: melee stamina admission, exhaustion, perfect-block stamina recovery, and the counterattack reward. It is a bounded 0.1 prototype of the [KINMUNE combat system design](KINMUNE-combat-system-design.md): animation and movesets keep coming from your installed framework (BFCO / For Honor / SCAR / DMCO); Apex Combat only enforces the resource rules around them.

## Runtime capabilities

- **Stamina admission for melee attacks.** Every new attack stage (including BFCO/MCO `attackstart`-style events and combo continuations) must pass the configured affordability check before it can deal damage. A stage costs stamina once at commit; misses still cost; repeated events, multi-hit swings, and duplicate callbacks do not charge again.
- **Exhaustion.** At zero stamina no new attack is admitted. A committed swing finishes. Ordinary blocking remains available.
- **Power attacks.** Admission requires positive stamina, but the engine keeps ownership of the power-attack debit; Apex Combat never charges a second power cost.
- **Perfect-block stamina recovery.** A fresh block action opens a bounded window (default 150 ms). A confirmed eligible melee hit inside the window is rejected, grants one configurable stamina reward (default 25), and consumes the window. Holding block never renews it; ordinary or mistimed blocks grant nothing. Duplicate contacts of the same strike and defender are recognized and cannot refund twice.
- **Counterattack window.** The defender of a perfect parry earns one free power attack: armed for a bounded time (default 1500 ms), their next power attack is admitted at zero stamina. The reward fires once, only power attacks spend it, light attacks ignore it, and it expires with the window; another perfect parry while it is armed refreshes it. Because power-attack costs stay owned by the game, Apex Combat refunds the engine's own drain for that one swing, and every attack made inside the window deals bonus poise damage (`poise.counterattackPoiseMultiplier`, default 1.25).
- **IDLE gates.** Adds a stamina condition to vanilla and custom attack IDLE roots so directly dispatched attacks cannot bypass admission.
- **Poise.** Each actor carries a per-class poise pool: light hits subtract class-damage, power attacks scale it, ordinary blocks pass only the blocked fraction, and a perfect parry deals flat damage to the attacker. A break staggers the actor and ignores further poise damage until the break recovery elapses; poise then refills and regenerates after the configured delay.

Poise is **enabled in the shipped configuration**. If you still run an older poise mod (for example Chocolate Poise), remove it or set `poise.enabled = false` — running both at once double-staggers. When [TrueHUD](https://www.nexusmods.com/skyrimspecialedition/mods/62775) is present, the pool is published through its special resource bar and flashes on damage and breaks. A perfect parry also plays a configurable feedback sound (vanilla blade/shield block impacts by default; For Honor Reforged users can repoint at its parry clangs via the INI) and fires the engine's own metal-spark impact flash at the blocking weapon or shield (toggle via `parryVisual`). Perfect blocking and poise both require the [Precision](https://www.nexusmods.com/skyrimspecialedition/mods/72305) collision framework. Without it, stamina features remain available and perfect blocking stays off.

Not yet implemented: For Honor counterattack effect adapters, projectile/spell eligibility, and sprint/jump/dodge attack costs.

## Installation

1. Install SKSE and a compatible runtime (SE, AE, or VR). VR builds are produced from the same source with `ENABLE_SKYRIM_VR`.
2. Install [Precision](https://www.nexusmods.com/skyrimspecialedition/mods/72305) if you want perfect-block rewards.
3. Extract the release archive (`Apex-Combat-<version>.zip`) into your game Data directory, or install it through Mod Organizer 2. It contains `SKSE/Plugins/ApexCombat.dll`, `SKSE/Plugins/ApexCombat.ini`, and license documents.
4. Debug symbols are published as a separate `-symbols.zip`; `build-info.json` and `SHA256SUMS.txt` inside the archive record the exact DLL hashes, source commit, and CommonLib commit.

Replace every `<game>` path below with your own Skyrim install location; no path in this document refers to the author's machine.

## Configuration

All options live in `<game>/Data/SKSE/Plugins/ApexCombat.ini`. Restart the game after editing. Keys are case-insensitive; invalid values are logged and keep defaults; the first occurrence of a duplicate key wins.

| Section | Key | Default | Meaning |
| --- | --- | --- | --- |
| General | `enabled` | `true` | Master switch; `false` leaves no hooks installed. |
| General | `playerAttacks` / `npcAttacks` | `true` | Apply admission rules to the player / NPCs. |
| General | `patchAttackIdles` | `true` | Add stamina conditions to attack IDLE forms. |
| Stamina | `lightOneHandedCost` | `20` | One-handed light attack cost. |
| Stamina | `lightTwoHandedCost` | `33` | Two-handed light attack cost. |
| Stamina | `unarmedCost` | `10` | Unarmed attack cost. |
| Stamina | `daggerCost` | `10` | Dagger light attack cost. |
| Stamina | `axeCost` | `25` | One-handed axe light attack cost. |
| Stamina | `maceCost` | `25` | One-handed mace light attack cost. |
| Stamina | `dualWieldMultiplier` | `1.6` | Scales the averaged per-hand cost of a dual-wield attack. |
| Stamina | `affordability` | `positive` | `positive`: any stamina above zero admits. `full`: the whole cost must be affordable. |
| Stamina | `regenDelaySeconds` | `0.5` | Stamina regen delay after a debit. |
| Stamina | `dodgeRegenDelaySeconds` | `0.5` | Stamina regen delay after a dodge starts. |
| Stamina | `exhaustionRecoveryDelaySeconds` | `1.0` | Stamina regen delay after exhaustion. |
| Stamina | `bowDrawDrainPerSecond` | `5.0` | Stamina drained per second while a bow is drawn; `0` disables it. |
| Stamina | `attackCommitTimeoutMs` | `1000` | Maximum time between admission and commitment. |
| PerfectBlock | `enabled` / `npcEnabled` | `true` | Perfect-block rewards for the player / NPCs. |
| PerfectBlock | `windowMs` | `150` | Timing window opened by a fresh block action. |
| PerfectBlock | `restore` | `25` | Stamina restored by one perfect block, capped by the missing amount. |
| PerfectBlock | `rearmDelayMs` | `200` | Minimum interval between fresh block starts; `0` disables it. |
| PerfectBlock | `blockAngleDegrees` | `120` | Total frontal blocking cone. |
| PerfectBlock | `parrySoundWeapon` / `parrySoundShield` | vanilla block-impact SNDRs | Perfect-parry feedback sounds as `Plugin.esp|0xFormID`; empty disables. |
| PerfectBlock | `parrySoundVolume` | `1.0` | Scales the parry clang volume, `0`–`2` where `1` is the descriptor level. |
| PerfectBlock | `parryVisual` | `true` | Attach the vanilla metal-spark impact root at the blocking weapon/shield so the engine fires its own spark flash. |
| CounterAttack | `enabled` | `true` (shipped INI) | Grant the defender of a perfect parry one zero-cost power attack. |
| CounterAttack | `windowMs` | `1500` | How long the armed reward lasts; `1`–`10000` milliseconds. |
| Poise | `enabled` | `true` (shipped INI) | Master switch; the code default stays off so a missing config cannot double-stagger alongside an older poise mod. |
| Poise | `hud` | `true` | Publish the poise pool through TrueHUD's special resource bar. |
| Poise | `maxPoise` | `100` | Poise pool size per actor. |
| Poise | `daggerDamage` / `swordDamage` / `axeDamage` / `maceDamage` / `twoHandedDamage` / `unarmedDamage` | `20/25/30/35/40/15` | Poise damage per light hit by weapon class. |
| Poise | `powerMultiplier` | `1.5` | Scales poise damage of power attacks. |
| Poise | `blockedMultiplier` | `0.5` | Fraction of poise damage that passes an ordinary held block. |
| Poise | `counterattackPoiseMultiplier` | `1.25` | Bonus multiplier on poise damage dealt by an attacker inside an active counterattack window; the flat perfect-parry counter-damage ignores it. |
| Poise | `perfectParryDamage` | `50` | Flat poise damage dealt to an attacker whose strike is perfect-parried. |
| Poise | `recoveryDelaySeconds` | `3.0` | Delay before poise regen after a hit. |
| Poise | `recoveryPerSecond` | `20.0` | Poise regenerated per second after the delay. |
| Poise | `breakRecoverySeconds` | `1.5` | Stagger length after a break; no further poise damage inside the window. |
| Debug | `trace` | `false` | Log every admission, commit, and parry decision. |

## Building

Dependency versions are pinned (`vcpkg.json` and the CommonLibSSE-NG commit in `CMakeLists.txt`); the tests do not require any game files.

### Portable logic tests (any OS)

```sh
tools/build.sh native-tests
```

Builds `src/core` with address/UB sanitizers and runs the combat-rules, configuration, and parry-contact suites plus the tooling unit tests.

### Windows DLL (MSVC + vcpkg)

```sh
VCPKG_ROOT=<vcpkg root> cmake --preset windows-release
cmake --build --preset windows-release --parallel 4
ctest --test-dir build/windows-release --output-on-failure
python tools/package.py --build build/windows-release --output dist
```

### Linux cross-compile (clang-cl + xwin)

Set `XWIN_SYSROOT` to a Windows SDK/CRT winsysroot and `APEX_WINDOWS_PREFIX` to the Windows dependency prefix, then:

```sh
tools/build.sh linux-cross
python tools/audit-dll.py build/linux-cross/ApexCombat.dll
python tools/package.py --build build/linux-cross --dependency-prefix "$APEX_WINDOWS_PREFIX" --output dist
```

`tools/deploy.py` can preview or apply a Mod Organizer 2 deployment with verified rollback; run it with `--help` first.

## Status and verification

Portable automated results only. The design document requires in-game acceptance checks (zero/just-below/exact-cost stamina, held combos, save/load, menus, weapon changes, cancellation) that have **not** been certified in a game session yet; see milestone 0.1 issue 5. Treat the DLL as a testable prototype, not a certified release.

## Documents

- [KINMUNE combat system design](KINMUNE-combat-system-design.md) — target architecture and combat contract.
- [KINMUNE CS combat audit](KINMUNE-CS-combat-audit.md) — the conflicts in the current installation that motivated this plugin.
