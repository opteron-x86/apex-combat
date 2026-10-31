# KINMUNE CS combat audit and recommended fixes

October 31, 2026. Installation: `/mnt/mods/Amaranth`. Profile: KINMUNE CS.

The stack has the foundations for the requested third-person combat, but several integration problems weaken stamina restrictions and poise consistency. BFCO plus For Honor Reforged is the intended foundation. Other combat components should support its movesets and mechanics. The clearest route to stamina restoration is to use Parry for All's newer native reward setting for the timed block you actually use. No live mod, profile, configuration, or save files were changed during this audit.

The findings below combine the active profile, Fluorine's resolved virtual filesystem, plugin records, October 31 runtime logs, installed scripts/settings, and mod-author documentation/source. They distinguish confirmed file/configuration conflicts from behavior that still needs an in-game test.

The repair recommendations describe the incremental option. Following your preference for a BFCO and For Honor-centered replacement, the [custom combat system design](/home/hackdirt/Documents/Codex/2026-10-31/i-x20/outputs/KINMUNE-combat-system-design.md) proposes a unified rules plugin that can retire the overlapping stamina, parry, and poise mods in stages.

**What currently handles each mechanic**

| Mechanic | Current owner and finding |
| --- | --- |
| Attack behavior and movesets | BFCO, For Honor in Skyrim, and For Honor Reforged; Reforged's NPC and directional options are installed. |
| Power-attack input | Both BFCO and Simple Power Attack respond to key code 260. This is duplicate input ownership. |
| Light-attack stamina | Gravitas, configured for 20 stamina for one-handed lights and 33 for two-handed lights. |
| Player attack admission | Gravitas adds stamina greater than zero to vanilla attack roots, but BFCO also has custom dispatch paths. |
| Dodge | DMCO with DMCO Stamina and Perk Requirements; the winning native DLL enforces a fixed 25-stamina cost. |
| NPC attack admission | SCAR NPC Exhaustion System wins the relevant records over Gravitas SCAR. |
| Timed block | Parry for All and Sekiro Combat S overlap. |
| Other parries | Parry – Simple Weapon Parrying provides swing/counterattack mechanics; For Honor Reforged provides its own moveset counters. |
| Poise | Chocolate Poise Reforged 4.2, receiving an older Blessed Settings configuration. |
| Stagger behavior | Chocolate Poise, Modern Stagger Lock, and selected For Honor reactions; one required guard-break behavior is missing. |

**1. Perfect-block stamina restoration has competing implementations**

You confirmed that the missing reward occurs when tapping block just before impact. Sekiro Combat S is intended to restore stamina on its timed block, so a stamina reward is already part of one installed mod's design. [Sekiro Combat S](https://www.nexusmods.com/skyrimspecialedition/mods/79206)

Your installed Parry for All package is 1.2.1. Its source corresponding to that generation intercepts successful parries in Precision's pre-hit callback and marks the hit ignored; its native hit hook also skips ordinary hit processing. Sekiro's installed script contains an `OnHit` handler and stamina restoration calls. This provides a strong explanation for the symptom: Parry for All can recognize the perfect block while preventing Sekiro from receiving the event that grants the reward. The October 31 log confirms Parry for All registered its Precision callbacks. This mechanism is supported by code and logs, but has not been reproduced in a controlled game session. [Parry for All historical hit handling](https://github.com/vinymayan/Parry-for-all/blob/c4484b47a76012be8b0ab3000c89294618cdb794/src/Hooks.cpp)

The current published Parry for All release is 1.3.2. Its matching September source includes native restoration settings, absent from your installed generation. The update changes packaging and adds State Behavior Framework and Dynamic Forms Generator integration. SBF 2.0, BDI, Notify Events, and SKSE Menu Framework are present; DFG is absent and is needed for the updated effects. Inspect the release archive and replace the old package deliberately rather than leaving stale ESP/assets mixed into it. [Release and requirements](https://www.nexusmods.com/skyrimspecialedition/mods/167873)

Recommended initial configuration after verifying the menu in the released build:

| Setting | Proposed value |
| --- | --- |
| Menu | SKSE Menu → Parry for All → Player Settings → Restore on Parry → Perfect Parry |
| Resource | Stamina |
| Calculation | Fixed Value |
| Amount and source | 25, Flat |
| Required perk | None |
| Normal parry restoration | None |
| Perfect window | Start with the existing generation's 150 ms default |

Twenty-five points funds one configured dodge or one one-handed light attack. The native restoration also applies to accepted projectile parries; a melee-only reward would need an extension. It does not itself clear the stamina regeneration delay. NPC restoration is separately configurable. [Restoration implementation and menu](https://github.com/vinymayan/Parry-for-all/blob/8aca0b78663ff304414ad648e0d0eb3e0c8698c3/src/Settings.cpp)

Use Parry for All as the single timed-block gameplay owner for this approach. Test Sekiro separately, then either keep a verified visual-only variant or remove its overlapping gameplay in the test setup. No built-in visual-only switch was found in your installation. Version 1.3.2 changes default feedback to block-hit; its author flags possible interactions with hit-driven mods. Recoil/None feedback is available if needed. [Parry for All changelog](https://www.nexusmods.com/skyrimspecialedition/mods/167873)

If retaining 1.2.1 is preferable, a small compatibility script could listen for the successful `PerfParriedCMF` event and restore stamina. It must not reward `PerfParryStartCMF`, which only opens the timing window. Such a patch would need compilation and testing; none was created or installed here.

**2. Chocolate Poise is loading, but its winning settings are mismatched**

Both `ChocolatePoise.ini` and `ChocolatePoise.json` resolve to Chocolate Poise Blessed Settings, overriding Reforged 4.2. The older INI is partially recognized through legacy aliases, producing a hybrid configuration. It is inaccurate to say it is entirely ignored.

The runtime log confirms these differences:

| Setting | Currently loaded | Shipped Reforged 4.2 preset |
| --- | ---: | ---: |
| Weapon weight contribution | 0.5 | 0.002 |
| Armor weight contribution | 0.005 | 0.002 |
| Normal bash poise multiplier | 0 | 1 |
| Poise regeneration rate | 0.035 | 0.05 |
| Normal impact threshold | 0.1 | 0.25 |

The first coefficient differs by a factor of 250; that does **not** mean final damage is 250 times greater, because other calculations and clamps apply. These settings can nevertheless substantially change how quickly poise breaks and how bashes behave.

Build a new configuration using Reforged 4.2's schema, then migrate intended Blessed tweaks explicitly. Preserve Blessed's plugin, distributed perk, and For Honor-specific JSON mappings while validating the result. Disabling the entire Blessed mod would remove more than the outdated INI. The author explicitly excludes old INI settings from compatibility. [Chocolate Poise Reforged](https://www.nexusmods.com/skyrimspecialedition/mods/181084)

Poise itself successfully loads, obtains the TrueHUD API, and initializes its race/equipment data. `StaggerMode=1` is active, preventing further poise damage while already staggered. The original Chocolate Poise mod being disabled is appropriate for this Reforged setup.

**3. Gravitas overwrites important BFCO and stagger restrictions**

A scan of the explicitly enabled plugins confirms Gravitas is the final winner for `Skyrim.esm:013211` and `Skyrim.esm:013216`, the right/left attack roots.

Its winning records add `Stamina > 0`, but remove Blessed's `IsStaggering == 0` graph-variable condition. On the right root they also restore vanilla parent/previous links, replacing BFCO's custom links to `SCSI-ACTbfco-Main.esp:000957` and `000830`.

These are confirmed record conflicts. The remedy should be a narrowly scoped compatibility ESP preserving the intended BFCO tree links and combining stamina and stagger conditions. Moving entire plugins around merely changes which requirement gets lost. Check the custom BFCO release/hotkey paths too; repairing vanilla roots alone does not establish a universal stamina lock.

**4. BFCO and Simple Power Attack both own the same input**

Both October 31 logs report power-attack key 260. The effective BFCO MCM override sets `iKeyAttackPowerNUM=260`, and Simple Power Attack's effective INI sets `iKeycode=260`.

The stale-looking BFCO NG JSON contains `bEnablePowerAttack=false`, but the currently loading BFCO DLL reads the older MCM/global setting and still reports the active binding. Its custom power-attack IDLEs lack an explicit stamina predicate, so direct dispatch is a plausible way around Simple Power Attack's restriction. This is a bypass candidate, not an observed in-game result.

Choose one input owner. If keeping Simple Power Attack, unbind BFCO's native power hotkey using its actual MCM/global configuration (`iKeyAttackPowerNUM=-1`) and verify the next runtime log. Keep Simple Power Attack's `bPowerAttackNoStamina=true`: despite its confusing name, source shows that this setting rejects power attacks at zero stamina. [BFCO input handler](https://github.com/vinymayan/BFCO/blob/og/src/Events.cpp), [Simple Power Attack source](https://github.com/InTheBottle/Simple-Power-Attack/blob/main/src/main.cpp)

**5. For Honor Reforged has installation and dependency mismatches**

The installed archive indicates Reforged 2.4. Its documentation requires Maxsu Block Overhaul for guard-break animation and links SCAR 2.01. The installed profile disables Maxsu Block Overhaul, and Pandora's selected patches and resolved behavior files confirm its graph is absent. SBF does not substitute for that graph. SCAR's current runtime log reports 1.0.6.0. Evaluate the author's compatible AE 2.01 package as a coordinated upgrade with the existing NPC stamina patches. [For Honor Reforged requirements](https://www.nexusmods.com/skyrimspecialedition/mods/172176), [SCAR 2.01 AE release](https://github.com/SkyHorizon3/SCAR/releases/tag/v2.01)

The author's installation directions also remove/hide the original For Honor meshes and scripts. The resolved filesystem still contains 2,838 mesh-tree files from the original package, including 150 OAR configuration files, plus five compiled scripts. That is an installation mismatch needing cleanup. It does not prove every residual file is executing; for example, Reforged removes the original parry script attachment from its relevant magic-effect override. [For Honor Reforged installation](https://www.nexusmods.com/skyrimspecialedition/mods/172176)

Ordinary poise breaks should produce stagger, with severity depending on the hit. Reforged reserves special knockdowns/follow-ups for particular counterattack or setup conditions; it does not guarantee a knockdown on every blocked hit. [For Honor Reforged mechanics](https://www.nexusmods.com/skyrimspecialedition/mods/172176)

**6. Stamina costs and NPC restrictions are partly correct already**

The winning DodgeFramework DLL is supplied by DMCO Stamina and Perk Requirements 1.0.2. Its runtime log confirms 25 stamina and no perk restriction. The winning Payload Interpreter configuration removes the original stamina payload, avoiding a duplicate deduction. The old DMCO MCM's displayed 5% cost is ignored by this replacement; it checks sufficient stamina for both dodge stages. [DMCO restrictions](https://www.nexusmods.com/skyrimspecialedition/mods/150718)

Gravitas's configured player light costs are 20/33, with bow draw at 5 per second and combat sneaking at 1 per second. Its vanilla-root admission rule only requires positive stamina, not enough to afford the entire attack. Those are different design choices: a near-empty bar may permit one more attack even though the full cost exceeds the remaining stamina.

SCAR NPC Exhaustion System is the final winner for the four relevant SCAR attack IDLEs. They require at least 1 stamina for lights, 30 for armed power attacks, and 20 for unarmed power attacks, plus no exhaustion effect and an additional random eligibility condition. Gravitas SCAR's 50-stamina power threshold does not win. `scar_adxp_staminaLimit.esp` is disabled; enabling it blindly would introduce another competing override.

For the exact requested rule, test every new attack at zero stamina, including held combos, sprint/jump attacks, directional attacks, and attacks following dodges. BFCO's custom release IDLE has no explicit stamina test, so the current evidence does not justify claiming that every attack path is gated.

**Installation health and smaller cleanup**

Skyrim 1.6.1170 and SKSE 2.2.6 load the core combat DLLs successfully. The selected Pandora patches include BFCO, DMCO, SCAR, Modern Stagger Lock, Precision/Creatures, SBF2, Payload, and TDM. All 39 generated mesh/animation-text files in Pandora Output win their paths. All 2,072 explicitly enabled plugin files exist; the master scan found no missing files after accounting for implicit Creation Club masters. This does not look like a general failure to deploy the combat stack.

OAR does skip four Dynamic Whirlwind Sprint DAR folders because their `_conditions.txt` files are absent: 8105000, 8106030, 8106040, and 8106050. Those replacement subsets need repair or removal, but this does not explain the parry refill.

**Recommended order and additional features**

1. Preserve the current profile/settings and use a test profile and disposable save for integration work.
2. Resolve BFCO/Gravitas/Blessed attack-root conflicts and duplicate power-attack input.
3. Migrate poise settings while retaining For Honor's necessary mappings and effects.
4. Restore Reforged's documented behavior dependencies and reconcile the SCAR update with NPC exhaustion.
5. Update Parry for All with its dependencies, configure a 25-point perfect-block reward, and test it as the sole timed-block gameplay owner.
6. Rebuild Pandora after changing behavior inputs, then verify the generated files still win.

For improvements, prioritize visible enemy poise bars, a brief reward cue on successful perfect blocks, and optional matching NPC perfect-block rewards. Consider restricting immediate parry stagger to perfect timing so a wider ordinary-parry window does not trivialize poise. Keep ordinary block available at zero stamina as the recovery route. A later tuning pass could require the full cost before each attack, but that is stricter than the requested zero-stamina lock.

Avoid adding another broad combat overhaul before these conflicts are resolved. The current stack already contains the needed systems.

**Proposed KINMUNE Combat Integration mod**

A dedicated compatibility mod would make the final rules explicit and keep future updates from silently undoing them. This is a proposed design, not an installed or tested implementation.

| Component | Responsibility |
| --- | --- |
| Small compatibility ESP | Preserve BFCO's attack tree, combine exhaustion and stagger restrictions, reconcile SCAR conditions, and retain necessary For Honor effects. |
| Versioned configuration overrides | Reforged-compatible poise settings, a single power-attack input owner, chosen costs, and perfect-parry reward settings. |
| Optional native stamina controller | Only if tests confirm bypasses: enforce the same admission rule on BFCO's custom attack and combo paths. Any transfer of stamina charging from Gravitas must disable its corresponding charges to avoid double deductions. |
| Optional parry-to-poise bridge | If desired, make a confirmed perfect parry inflict defined poise damage while Chocolate Poise determines the break. Avoid adding an unconditional stagger from several different mods to the same event. |

For a BFCO-led design I would evaluate BFCO's native power input as the eventual owner, while retaining its current binding for familiarity. Removing Simple Power Attack is contingent on proving that the replacement gate covers every BFCO path; simply disabling it now would remove a known stamina check. The compatibility layer should also preserve For Honor's deliberate counterattacks and special follow-ups instead of turning every perfect block into its special knockdown.

The initial combat contract would be: light attacks consume the chosen cost even on a miss; zero stamina rejects each new attack and dodge; attacks already committed finish; ordinary blocking remains possible; a successful perfect block grants 25 stamina once; poise breaks stagger either combatant; special For Honor knockdowns keep their own conditions. NPCs follow equivalent resource rules where their attack system supports them. Full-cost attack admission and player/NPC parry-reward symmetry remain explicit balancing options.

Parry for All's native reward makes a custom refill DLL unnecessary if its update works correctly in the test profile. A custom mod is most valuable for the gaps between BFCO, stamina admission, and poise—not for recreating features already implemented upstream.

**Acceptance checks for the eventual changes**

| Test | Expected result |
| --- | --- |
| Light, power, held combo, sprint/jump/directional attack at zero stamina | No new attack starts. |
| Dodge at 0, 24, and 25 stamina | First two fail; 25 permits one dodge. Test second roll separately. |
| One-handed and two-handed light attack | Configured cost applied once, including misses. |
| Ordinary or mistimed block | No stamina reward. |
| Perfect block while missing at least 25 stamina | Exactly 25 restored once, allowing for any separately configured block/reflection cost. |
| Perfect block at zero stamina | Can recover stamina and resume an action. |
| Perfect projectile parry | Matches the chosen reward policy. |
| Player and enemy poise break | Visible stagger; special knockdown only where its conditions apply. |
| NPC reaches exhaustion | New attacks stop under the intended SCAR rules, then resume after recovery. |
| Re-enable any overlapping parry feature | No duplicate reward, conflicting stagger, or extra input. |

**Local evidence locations**

- Profile: `/mnt/mods/Amaranth/profiles/KINMUNE CS/modlist.txt` and `/mnt/mods/Amaranth/profiles/KINMUNE CS/plugins.txt`.
- Resolved filesystem manifest: `/mnt/mods/Amaranth/vfs-index.json`.
- User configuration: `/mnt/mods/Amaranth/overwrite/MCM/Settings/Gravitas - MCM.ini`, `/mnt/mods/Amaranth/overwrite/MCM/Settings/SCSI-ACTbfco-Main.ini`, and `/mnt/mods/Amaranth/overwrite/SKSE/Plugins/SimplePowerAttack.ini`.
- Winning poise configuration: `/mnt/mods/Amaranth/mods/Chocolate Poise Blessed Settings/skse/plugins/ChocolatePoise.ini` and `/mnt/mods/Amaranth/mods/Chocolate Poise Blessed Settings/skse/plugins/ChocolatePoise.json`.
- Modern poise preset: `/mnt/mods/Amaranth/mods/Chocolate Poise - Reforged/skse/plugins/ChocolatePoise.ini`.
- Dodge restrictions: `/mnt/mods/Amaranth/mods/DMCO Stamina and Perk Requirements/SKSE/Plugins/DodgeFrameworkRestrictions.ini`.
- Behavior evidence: `/mnt/mods/Amaranth/mods/Pandora Output/Engine.log` and `/mnt/mods/Amaranth/mods/Pandora Output/Pandora_Engine/ActiveMods.json`.
- Runtime logs: `/home/hackdirt/.local/share/Steam/steamapps/compatdata/489830/pfx/drive_c/users/steamuser/Documents/My Games/Skyrim Special Edition/SKSE`.
