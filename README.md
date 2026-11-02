# Apex Combat

Apex Combat adds stamina costs, exhaustion, timed parries, poise, and enemy attack pacing to Skyrim combat. It works alongside attack and animation frameworks such as BFCO and authored movesets such as For Honor.

This is an experimental SKSE plugin. Automated tests and successful compilation do not certify every runtime or moveset. Remaining compatibility checks are tracked in [GitHub issues](https://github.com/opteron-x86/apex-combat/issues).

## Combat

- Light melee attacks spend stamina, including misses. Additional combo strikes must pass admission again. An already committed swing can finish when its cost exhausts the attacker.
- At zero stamina, ordinary attacks are denied. Blocking remains available. The default policy permits an attack with any positive stamina; `affordability = full` requires the full Apex-owned cost.
- A fresh block opens a 150 ms perfect-parry window. A successful eligible melee parry restores up to 25 stamina, prevents that hit, and damages the attacker's poise. Holding block does not renew the window. Successful parries produce a bright impact flash and sparks at the blocking weapon or shield, with an optional brief expanding ring.
- A perfect parry grants one power counterattack within 1.5 seconds, including at zero stamina. Apex applies the free counterattack to the native cost calculation before stamina is spent. Regular power attacks retain the game's weapon and perk calculations, with the active potion discount applied afterward.
- Player and NPC poise breaks cause stagger, followed by a recovery period that prevents repeated breaks. Ordinary blocks take reduced poise damage. TrueHUD can display the pool.
- The optional combat director limits concurrent melee attackers, spaces NPC bashes, and directs waiting enemies to circle. Retaliation can temporarily exceed the normal slot limit.
- Stamina potions can replace their refill with a timed cost discount through the optional potion patcher. A new dose replaces the current discount and starts its full duration; discounts do not stack. A discount does not refill an empty stamina bar or bypass ordinary exhaustion.

Default light-attack costs:

| Attack | Stamina |
| --- | ---: |
| Dagger or unarmed | 10 |
| One-handed sword | 20 |
| One-handed axe or mace | 25 |
| Two-handed | 33 |
| Dual wield | Average of both weapon costs × 1.6 |

Bow draw drains 5 stamina per second. Attack and dodge recovery delays default to 0.5 seconds; exhaustion adds a 1-second recovery delay. Player stamina regeneration is multiplied by 1.5 while retaining existing actor-value modifiers.

## Requirements and installation

1. Install the SKSE build and Address Library appropriate to your Skyrim runtime.
2. Install your attack framework and movesets, with their required behavior output. Apex does not supply BFCO, For Honor, SCAR, or a dodge animation framework.
3. Install [Precision](https://www.nexusmods.com/skyrimspecialedition/mods/72305) for supported melee parries and poise. [TrueHUD](https://www.nexusmods.com/skyrimspecialedition/mods/62775) is optional for the poise bar.
4. Install the Apex archive through your mod manager. Enable `ApexCombat.esp` for native power-attack cost modifiers and the included parry sounds.
5. Edit `SKSE/Plugins/ApexCombat.ini` and restart the game. Each setting is documented in that file.

The DLL is compiled with SE, AE, and VR layouts through [CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG). Gameplay reports currently cover AE 1.6.1170. Native power-cost dispatch was inspected on that runtime; SE and VR use the virtual layouts documented by the pinned CommonLib version and still need gameplay validation. The companion data plugins are ESL flagged, so VR also requires compatible ESL support. VR collision support is not supplied by this plugin; a universal DLL does not make Precision features available in VR.

Use one owner for each mechanic. Disable overlapping attack-cost, timed-parry, poise, and attack-slot features before enabling their Apex equivalents. The shipped INI enables poise and leaves the director off. Both code defaults are off when no INI is present.

Dodge admission and charging remain the dodge framework's responsibility. `dodgeStaminaCost = 0` prevents Apex from adding a second charge. Keep the framework's stamina restriction enabled. Setting its cost to zero without a replacement admission check can permit dodging while exhausted. Apex's nonzero dodge-cost option is experimental and is not a substitute for that restriction.

## Stamina potions

The optional [potion patcher](tools/PotionPatcher/README.md) converts supported winning potion records into keyword-tagged discount potions. It removes their instant stamina refill while preserving unrelated effects. Generate the patch for your own load order and enable it after the source plugins. Remove the generated patch to restore the original records.

The accepted discount appears as Stamina Efficiency in Active Effects for its full duration. Its timer pauses with the game and continues from its saved remaining duration after loading. Potion descriptions show the configured percentage and duration. Drinking another potion replaces the current discount, even with a weaker tier, and starts the new potion’s full duration.

Runtime-created or unpatched potions keep their original behavior. Without the generated keywords, Apex does not grant potion discounts. Disable the generated patch whenever disabling the potion feature.

Discounts apply to Apex-owned attack costs, native power-attack costs, and bow drain, plus the configured sprint adjustment. They do not discount ordinary block impacts, bashes, or costs owned by another dodge framework. Power attacks keep one native stamina debit; the discount modifies the calculation before that debit.

## Configuration and diagnostics

Configuration keys are case-insensitive. Duplicate keys keep their first value. Invalid numeric values retain defaults; invalid boolean values disable that option. Warnings are written to `ApexCombat.log` in the SKSE log directory.

In `[PerfectBlock]`, `parryVisualScale` adjusts the burst size (0.25–3), `parryVisualRings` toggles the ring, and `parryVisual` disables all parry visuals.

Set `[Debug] trace = true` for short diagnostic sessions. A useful issue report includes the Skyrim runtime, SKSE version, Apex version, moveset, relevant settings, the exact action, and the corresponding log excerpt.

Attack IDLE diagnostics report how many forms were found and gated. Shared and power-attack IDLEs remain outside static gating when zero-stamina counterattacks are enabled; complete direct-dispatch coverage still requires validation. Free counterattacks require a recognized attack admission; a native stamina check before that admission can still reject an exhausted attack. Combo transitions and cancels need validation with each moveset. Projectile parries and For Honor's special effect/counterattack adapters are not included.

## Building

Requires CMake 3.28+, Ninja, and a C++23 compiler. The Windows plugin uses pinned CommonLibSSE-NG and vcpkg dependencies.

Portable rules and tooling tests:

```sh
tools/build.sh native-tests
```

Windows, from a developer shell with clang-cl and `VCPKG_ROOT` set:

```sh
cmake --preset windows-release
cmake --build --preset windows-release
ctest --test-dir build/windows-release --output-on-failure
python tools/package.py --build build/windows-release --output dist
```

For Linux cross-compilation, provide a Windows SDK/CRT through `XWIN_SYSROOT` and compatible Windows dependency packages through `APEX_WINDOWS_PREFIX`, then run `tools/build.sh linux-cross`. The optional potion patcher uses .NET 8 and Mutagen.

Packages include checksums and build provenance. Packaging rejects changed source inputs or replaced DLL/PDB files after a recorded build. Debug symbols and source are distributed separately. `tools/deploy.py --help` describes explicit deployment and rollback options.
