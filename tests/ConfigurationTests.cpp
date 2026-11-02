#include "core/Configuration.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>

using namespace Apex;

namespace {
int checks = 0;
void check(bool condition, const std::source_location location = std::source_location::current())
{
    ++checks;
    if (!condition) {
        std::cerr << location.file_name() << ':' << location.line() << ": check failed\n";
        std::exit(EXIT_FAILURE);
    }
}
void near(float actual, float expected,
          const std::source_location location = std::source_location::current())
{
    check(std::fabs(actual - expected) < 0.0001F, location);
}

struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt != 10; ++attempt) {
            path = std::filesystem::temp_directory_path() /
                   ("apex-config-tests-" + std::to_string(stamp) + "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(path)) return;
        }
        throw std::runtime_error("Could not create temporary test directory");
    }
    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void defaultsAndCompleteConfiguration()
{
    const auto defaults = parseConfiguration("");
    check(defaults.diagnostics.empty());
    check(defaults.config.enabled && defaults.config.playerAttacks && defaults.config.npcAttacks);
    check(defaults.config.perfectBlock && defaults.config.npcPerfectBlock && defaults.config.patchAttackIdles);
    check(!defaults.config.trace && !defaults.config.rules.chargePowerAttacks);
    near(defaults.config.rules.lightOneHandedCost, 20);
    near(defaults.config.rules.lightTwoHandedCost, 33);
    near(defaults.config.rules.unarmedCost, 10);
    near(defaults.config.rules.daggerCost, 10);
    near(defaults.config.rules.axeCost, 25);
    near(defaults.config.rules.maceCost, 25);
    near(defaults.config.rules.dualWieldMultiplier, 1.6F);
    near(defaults.config.rules.perfectBlockRestore, 25);
    near(defaults.config.regenDelaySeconds, 0.5F);
    near(defaults.config.dodgeRegenDelaySeconds, 0.5F);
    near(defaults.config.exhaustionRecoveryDelaySeconds, 1.0F);
    near(defaults.config.bowDrawDrainPerSecond, 5.0F);
    near(defaults.config.blockAngleDegrees, 120);
    check(defaults.config.rules.affordability == Core::Affordability::PositiveStamina);
    check(defaults.config.rules.perfectBlockWindowMs == 150);
    check(defaults.config.rules.parryRearmDelayMs == 200);
    check(defaults.config.parrySoundWeapon == "ApexCombat.esp|0x800");
    check(defaults.config.parrySoundShield == "ApexCombat.esp|0x801");
    near(defaults.config.parrySoundVolume, 1.0F);
    check(defaults.config.parryVisual);
    check(defaults.config.rules.counterattackEnabled);
    check(defaults.config.rules.counterattackWindowMs == 1500);

    const auto full = parseConfiguration(R"ini(
[General]
enabled=false
playerAttacks=0
npcAttacks=1
patchAttackIdles=false
[Stamina]
lightOneHandedCost=12.5
lightTwoHandedCost=40
    unarmedCost=0
    daggerCost=7.5
    axeCost=55
    maceCost=60
    dualWieldMultiplier=2.5
    affordability=full
    regenDelaySeconds=2.25
    dodgeRegenDelaySeconds=1.5
    exhaustionRecoveryDelaySeconds=3.75
    bowDrawDrainPerSecond=12.5
    attackCommitTimeoutMs=1500
[PerfectBlock]
enabled=false
npcEnabled=0
windowMs=125
restore=42.5
rearmDelayMs=0
blockAngleDegrees=90
parrySoundWeapon=My Mod.esp|0x123
parrySoundShield=
parrySoundVolume=1.75
parryVisual=0
[CounterAttack]
enabled=0
windowMs=900
[Debug]
trace=true
)ini");
    check(full.diagnostics.empty());
    check(!full.config.enabled && !full.config.playerAttacks && full.config.npcAttacks);
    check(!full.config.patchAttackIdles && !full.config.perfectBlock && !full.config.npcPerfectBlock);
    check(full.config.trace && !full.config.rules.chargePowerAttacks);
    near(full.config.rules.lightOneHandedCost, 12.5F);
    near(full.config.rules.lightTwoHandedCost, 40);
    near(full.config.rules.unarmedCost, 0);
    near(full.config.rules.daggerCost, 7.5F);
    near(full.config.rules.axeCost, 55);
    near(full.config.rules.maceCost, 60);
    near(full.config.rules.dualWieldMultiplier, 2.5F);
    near(full.config.rules.perfectBlockRestore, 42.5F);
    near(full.config.regenDelaySeconds, 2.25F);
    near(full.config.dodgeRegenDelaySeconds, 1.5F);
    near(full.config.exhaustionRecoveryDelaySeconds, 3.75F);
    near(full.config.bowDrawDrainPerSecond, 12.5F);
    near(full.config.blockAngleDegrees, 90);
    check(full.config.rules.affordability == Core::Affordability::FullCost);
    check(full.config.rules.attackCommitTimeoutMs == 1500);
    check(full.config.rules.perfectBlockWindowMs == 125);
    check(full.config.rules.parryRearmDelayMs == 0);
    check(full.config.parrySoundWeapon == "My Mod.esp|0x123");
    check(full.config.parrySoundShield.empty());
    near(full.config.parrySoundVolume, 1.75F);
    check(!full.config.parryVisual);
    check(!full.config.rules.counterattackEnabled);
    check(full.config.rules.counterattackWindowMs == 900);
    Core::CombatRules validated(full.config.rules);
    check(validated.settings().perfectBlockWindowMs == 125);
    check(!validated.settings().counterattackEnabled);
    check(validated.settings().counterattackWindowMs == 900);
}

void parryVisualConfiguration()
{
    const auto defaults = parseConfiguration("");
    near(defaults.config.parryVisualScale, 1.0F);
    check(defaults.config.parryVisualRings);
    const auto custom = parseConfiguration("[PerfectBlock]\nparryVisualScale=1.5\nparryVisualRings=false\n");
    check(custom.diagnostics.empty());
    near(custom.config.parryVisualScale, 1.5F);
    check(!custom.config.parryVisualRings && custom.config.parryVisual);
    for (const auto* value : {"0.25", "3"}) {
        const auto parsed = parseConfiguration(std::string("[PerfectBlock]\nparryVisualScale=") + value);
        check(parsed.diagnostics.empty());
    }
    for (const auto* value : {"0", "0.249", "3.01", "nan", "inf", "1.2junk"}) {
        const auto parsed = parseConfiguration(std::string("[PerfectBlock]\nparryVisualScale=") + value);
        check(parsed.diagnostics.size() == 1);
        near(parsed.config.parryVisualScale, 1.0F);
    }
}

void poiseConfiguration()
{
    const auto defaults = parseConfiguration("");
    check(!defaults.config.poise);
    check(defaults.config.poiseHud);
    near(defaults.config.poiseSettings.maxPoise, 100);
    near(defaults.config.poiseSettings.swordDamage, 25);
    near(defaults.config.poiseSettings.perfectParryDamage, 50);
    near(defaults.config.poiseSettings.counterattackPoiseMultiplier, 1.25F);
    check(defaults.config.poiseSettings.recoveryDelayMs == 3000);
    check(defaults.config.poiseSettings.breakRecoveryMs == 1500);

    const auto full = parseConfiguration(R"ini(
[Poise]
enabled=1
maxPoise=40
daggerDamage=5
swordDamage=30
axeDamage=45
maceDamage=35
twoHandedDamage=60
unarmedDamage=10
powerMultiplier=2.5
blockedMultiplier=0.25
counterattackPoiseMultiplier=2.0
perfectParryDamage=80
recoveryDelaySeconds=5
recoveryPerSecond=50
breakRecoverySeconds=2
hud=0
)ini");
    check(full.diagnostics.empty());
    check(full.config.poise);
    check(!full.config.poiseHud);
    near(full.config.poiseSettings.maxPoise, 40);
    near(full.config.poiseSettings.daggerDamage, 5);
    near(full.config.poiseSettings.swordDamage, 30);
    near(full.config.poiseSettings.axeDamage, 45);
    near(full.config.poiseSettings.maceDamage, 35);
    near(full.config.poiseSettings.twoHandedDamage, 60);
    near(full.config.poiseSettings.unarmedDamage, 10);
    near(full.config.poiseSettings.powerMultiplier, 2.5F);
    near(full.config.poiseSettings.blockedMultiplier, 0.25F);
    near(full.config.poiseSettings.counterattackPoiseMultiplier, 2.0F);
    near(full.config.poiseSettings.perfectParryDamage, 80);
    check(full.config.poiseSettings.recoveryDelayMs == 5000);
    check(full.config.poiseSettings.breakRecoveryMs == 2000);
    near(full.config.poiseSettings.recoveryPerSecond, 50);
    Core::PoiseRules validated(full.config.poiseSettings);
    check(validated.settings().maxPoise == 40);

    auto limits = parseConfiguration(R"ini(
[Poise]
maxPoise=100000
daggerDamage=10000
swordDamage=0
axeDamage=10000
maceDamage=10000
twoHandedDamage=10000
unarmedDamage=10000
powerMultiplier=100
blockedMultiplier=10
counterattackPoiseMultiplier=100
perfectParryDamage=10000
recoveryDelaySeconds=60
recoveryPerSecond=100000
breakRecoverySeconds=60
)ini");
    check(limits.diagnostics.empty());
    near(limits.config.poiseSettings.maxPoise, 100000);
    check(limits.config.poiseSettings.recoveryDelayMs == 60000);
    check(limits.config.poiseSettings.breakRecoveryMs == 60000);
    Core::PoiseRules validatedLimits(limits.config.poiseSettings);
    check(validatedLimits.settings().blockedMultiplier == 10);
    check(validatedLimits.settings().counterattackPoiseMultiplier == 100);

    const auto invalid = parseConfiguration(R"ini(
[Poise]
maxPoise=0
daggerDamage=-1
swordDamage=nan
axeDamage=10001
maceDamage=inf
powerMultiplier=100.5
blockedMultiplier=-0.5
counterattackPoiseMultiplier=0.5
perfectParryDamage=10001
recoveryDelaySeconds=61
recoveryPerSecond=100001
breakRecoverySeconds=60.5
)ini");
    check(invalid.diagnostics.size() == 12);
    near(invalid.config.poiseSettings.maxPoise, 100);
    near(invalid.config.poiseSettings.daggerDamage, 20);
    near(invalid.config.poiseSettings.swordDamage, 25);
    near(invalid.config.poiseSettings.axeDamage, 30);
    near(invalid.config.poiseSettings.maceDamage, 35);
    near(invalid.config.poiseSettings.powerMultiplier, 1.5F);
    near(invalid.config.poiseSettings.blockedMultiplier, 0.5F);
    near(invalid.config.poiseSettings.counterattackPoiseMultiplier, 1.25F);
    near(invalid.config.poiseSettings.perfectParryDamage, 50);
    check(invalid.config.poiseSettings.recoveryDelayMs == 3000);
    check(invalid.config.poiseSettings.recoveryPerSecond == 20);
    check(invalid.config.poiseSettings.breakRecoveryMs == 1500);
}

void directorConfiguration()
{
    const auto defaults = parseConfiguration("");
    check(!defaults.config.director);
    check(defaults.config.directorPlayerOnly);
    check(defaults.config.aiSettings.maxAttackers == 2);
    check(defaults.config.aiSettings.slotWindowMinMs == 4000);
    check(defaults.config.aiSettings.slotWindowMaxMs == 8000);
    check(defaults.config.aiSettings.retaliationMs == 3000);
    check(defaults.config.aiSettings.bashCooldownMs == 6000);
    near(defaults.config.aiSettings.bashReactionChance, 0.35F);
    near(defaults.config.aiSettings.bashProactiveChance, 0.0F);
    near(defaults.config.aiSettings.bashStaminaCost, 0);
    check(defaults.config.aiSettings.maxCircleMs == 10000);
    near(defaults.config.circleRadiusMin, 200);
    near(defaults.config.circleRadiusMax, 320);

    const auto full = parseConfiguration(R"ini(
[Director]
enabled=1
playerOnly=0
maxAttackers=4
slotWindowMinSeconds=2
slotWindowMaxSeconds=6.5
retaliationSeconds=5
bashCooldownSeconds=8
bashReactionChance=0.5
bashProactiveChance=0.1
bashStaminaCost=40
maxCircleSeconds=15
circleRadiusMin=180
circleRadiusMax=400
)ini");
    check(full.diagnostics.empty());
    check(full.config.director);
    check(!full.config.directorPlayerOnly);
    check(full.config.aiSettings.maxAttackers == 4);
    check(full.config.aiSettings.slotWindowMinMs == 2000);
    check(full.config.aiSettings.slotWindowMaxMs == 6500);
    check(full.config.aiSettings.retaliationMs == 5000);
    check(full.config.aiSettings.bashCooldownMs == 8000);
    near(full.config.aiSettings.bashReactionChance, 0.5F);
    near(full.config.aiSettings.bashProactiveChance, 0.1F);
    near(full.config.aiSettings.bashStaminaCost, 40);
    check(full.config.aiSettings.maxCircleMs == 15000);
    near(full.config.circleRadiusMin, 180);
    near(full.config.circleRadiusMax, 400);
    Core::AiRules validated(full.config.aiSettings);
    check(validated.settings().maxAttackers == 4);

    const auto limits = parseConfiguration(R"ini(
[Director]
maxAttackers=5
slotWindowMinSeconds=0.5
slotWindowMaxSeconds=30
retaliationSeconds=0
bashCooldownSeconds=1
bashReactionChance=1
bashProactiveChance=0
bashStaminaCost=1000
maxCircleSeconds=120
circleRadiusMin=50
circleRadiusMax=2000
)ini");
    check(limits.diagnostics.empty());
    check(limits.config.aiSettings.maxAttackers == 5);
    check(limits.config.aiSettings.slotWindowMinMs == 500);
    check(limits.config.aiSettings.slotWindowMaxMs == 30000);
    check(limits.config.aiSettings.retaliationMs == 0);
    check(limits.config.aiSettings.bashCooldownMs == 1000);
    check(limits.config.aiSettings.maxCircleMs == 120000);
    Core::AiRules validatedLimits(limits.config.aiSettings);
    check(validatedLimits.settings().maxAttackers == 5);

    const auto invalid = parseConfiguration(R"ini(
[Director]
maxAttackers=0
slotWindowMinSeconds=0.4
slotWindowMaxSeconds=31
retaliationSeconds=-1
bashCooldownSeconds=61
bashReactionChance=1.1
bashProactiveChance=-0.1
bashStaminaCost=1001
maxCircleSeconds=0.9
circleRadiusMin=49
circleRadiusMax=2001
)ini");
    check(invalid.diagnostics.size() == 11);
    check(invalid.config.aiSettings.maxAttackers == 2);
    check(invalid.config.aiSettings.slotWindowMinMs == 4000);
    check(invalid.config.aiSettings.slotWindowMaxMs == 8000);
    check(invalid.config.aiSettings.retaliationMs == 3000);
    check(invalid.config.aiSettings.bashCooldownMs == 6000);
    near(invalid.config.aiSettings.bashReactionChance, 0.35F);
    near(invalid.config.aiSettings.bashProactiveChance, 0.0F);
    near(invalid.config.aiSettings.bashStaminaCost, 0);
    check(invalid.config.aiSettings.maxCircleMs == 10000);
    near(invalid.config.circleRadiusMin, 200);
    near(invalid.config.circleRadiusMax, 320);
}

void potionAndRegenConfiguration()
{
    const auto defaults = parseConfiguration("");
    near(defaults.config.staminaRegenMult, 1.5F);
    near(defaults.config.dodgeStaminaCost, 0);
    check(defaults.config.potionSettings.enabled);
    near(defaults.config.potionSettings.discountMinor, 10);
    near(defaults.config.potionSettings.discountStandard, 20);
    near(defaults.config.potionSettings.discountPlentiful, 30);
    near(defaults.config.potionSettings.discountVigorous, 50);
    near(defaults.config.potionSettings.discountExtreme, 75);
    near(defaults.config.potionSettings.discountUltimate, 100);
    check(defaults.config.potionSettings.standardSeconds == 60);
    check(defaults.config.potionSettings.ultimateSeconds == 20);

    const auto full = parseConfiguration(R"ini(
[Stamina]
regenMult=2.0
dodgeStaminaCost=35
[Potions]
enabled=0
discountMinor=5
discountStandard=15
discountPlentiful=25
discountVigorous=40
discountExtreme=60
discountUltimate=90
standardSeconds=45
ultimateSeconds=15
)ini");
    check(full.diagnostics.empty());
    near(full.config.staminaRegenMult, 2.0F);
    near(full.config.dodgeStaminaCost, 35);
    check(!full.config.potionSettings.enabled);
    near(full.config.potionSettings.discountMinor, 5);
    near(full.config.potionSettings.discountStandard, 15);
    near(full.config.potionSettings.discountPlentiful, 25);
    near(full.config.potionSettings.discountVigorous, 40);
    near(full.config.potionSettings.discountExtreme, 60);
    near(full.config.potionSettings.discountUltimate, 90);
    check(full.config.potionSettings.standardSeconds == 45);
    check(full.config.potionSettings.ultimateSeconds == 15);

    const auto invalid = parseConfiguration(R"ini(
[Stamina]
regenMult=0.05
dodgeStaminaCost=-1
[Potions]
discountMinor=-1
discountStandard=101
discountPlentiful=nan
discountVigorous=inf
discountExtreme=100.5
discountUltimate=-0.1
standardSeconds=0
ultimateSeconds=3601
)ini");
    check(invalid.diagnostics.size() == 10);
    near(invalid.config.staminaRegenMult, 1.5F);
    near(invalid.config.dodgeStaminaCost, 0);
    near(invalid.config.potionSettings.discountMinor, 10);
    near(invalid.config.potionSettings.discountStandard, 20);
    near(invalid.config.potionSettings.discountPlentiful, 30);
    near(invalid.config.potionSettings.discountVigorous, 50);
    near(invalid.config.potionSettings.discountExtreme, 75);
    near(invalid.config.potionSettings.discountUltimate, 100);
    check(invalid.config.potionSettings.standardSeconds == 60);
    check(invalid.config.potionSettings.ultimateSeconds == 20);
}

void whitespaceCommentsAndCase()
{
    const auto result = parseConfiguration(
        "\xEF\xBB\xBF ; file comment\r\n"
        "[ GeNeRaL ] ; section comment\r\n"
        " EnAbLeD = TRUE # value comment\r\n"
        "PLAYERATTACKS = False\r\n"
        "# ignored\r\n"
        "[Stamina]\r\n"
        "affordability = FuLl\r\n"
        "lightOneHandedCost = 1.25e2\r\n"
        "DAGGERCOST = 8\r\n"
        "DualWieldMultiplier = 3\r\n"
        "BowDrawDrainPerSecond = 2.5\r\n"
        "[ PerfectBlock ]\r\n"
        "windowMS = 149\r\n"
        "[Debug]\r\n"
        "trace = 1");
    check(result.diagnostics.empty());
    check(result.config.enabled && !result.config.playerAttacks && result.config.trace);
    check(result.config.rules.affordability == Core::Affordability::FullCost);
    near(result.config.rules.lightOneHandedCost, 125);
    near(result.config.rules.daggerCost, 8);
    near(result.config.rules.dualWieldMultiplier, 3);
    near(result.config.bowDrawDrainPerSecond, 2.5F);
    check(result.config.rules.perfectBlockWindowMs == 149);
}

void strictNumbersAndFiniteBounds()
{
    for (const std::string value : {"nan", "NaN", "inf", "-inf", "1e999", "-1", "10001",
                                   "20stamina", "20.0.0", "0x20", "1_000", "", "20=30", "+20"}) {
        const auto result = parseConfiguration("[Stamina]\nlightOneHandedCost=" + value);
        check(result.diagnostics.size() == 1);
        near(result.config.rules.lightOneHandedCost, 20);
        check(std::isfinite(result.config.rules.lightOneHandedCost));
    }
    for (const std::string value : {"0", "-1", "1001", "150.5", "150ms", "999999999999999999999"}) {
        const auto result = parseConfiguration("[PerfectBlock]\nwindowMs=" + value);
        check(result.diagnostics.size() == 1);
        check(result.config.rules.perfectBlockWindowMs == 150);
    }
    auto limits = parseConfiguration(R"ini(
[Stamina]
lightOneHandedCost=0
lightTwoHandedCost=10000
unarmedCost=0.0001
daggerCost=0
axeCost=10000
maceCost=10000
dualWieldMultiplier=1
regenDelaySeconds=10
dodgeRegenDelaySeconds=10
exhaustionRecoveryDelaySeconds=10
bowDrawDrainPerSecond=10000
attackCommitTimeoutMs=1
[PerfectBlock]
windowMs=1000
restore=10000
rearmDelayMs=10000
blockAngleDegrees=360
parrySoundVolume=2
parryVisual=1
)ini");
    check(limits.diagnostics.empty());
    near(limits.config.parrySoundVolume, 2.0F);
    check(limits.config.parryVisual);
    near(limits.config.rules.lightOneHandedCost, 0);
    near(limits.config.rules.lightTwoHandedCost, 10000);
    near(limits.config.rules.daggerCost, 0);
    near(limits.config.rules.axeCost, 10000);
    near(limits.config.rules.maceCost, 10000);
    near(limits.config.rules.dualWieldMultiplier, 1);
    near(limits.config.dodgeRegenDelaySeconds, 10);
    near(limits.config.exhaustionRecoveryDelaySeconds, 10);
    near(limits.config.bowDrawDrainPerSecond, 10000);
    check(limits.config.rules.attackCommitTimeoutMs == 1);
    check(limits.config.rules.perfectBlockWindowMs == 1000);
    near(limits.config.blockAngleDegrees, 360);
    Core::CombatRules validated(limits.config.rules);
    check(validated.settings().parryRearmDelayMs == 10000);
    const auto invalid = parseConfiguration(R"ini(
[Stamina]
regenDelaySeconds=10.01
daggerCost=-1
axeCost=10001
maceCost=nan
dualWieldMultiplier=0.99
dodgeRegenDelaySeconds=10.01
exhaustionRecoveryDelaySeconds=11
bowDrawDrainPerSecond=-0.5
attackCommitTimeoutMs=0
affordability=anything
[PerfectBlock]
restore=-1
rearmDelayMs=-1
blockAngleDegrees=0
parrySoundVolume=2.01
parryVisual=broken
)ini");
    check(invalid.diagnostics.size() == 15);
    near(invalid.config.regenDelaySeconds, 0.5F);
    near(invalid.config.rules.daggerCost, 10);
    near(invalid.config.rules.axeCost, 25);
    near(invalid.config.rules.maceCost, 25);
    near(invalid.config.rules.dualWieldMultiplier, 1.6F);
    near(invalid.config.dodgeRegenDelaySeconds, 0.5F);
    near(invalid.config.exhaustionRecoveryDelaySeconds, 1.0F);
    near(invalid.config.bowDrawDrainPerSecond, 5.0F);
    near(invalid.config.blockAngleDegrees, 120);
    near(invalid.config.rules.perfectBlockRestore, 25);
    check(invalid.config.rules.attackCommitTimeoutMs == 1000);
    check(invalid.config.rules.parryRearmDelayMs == 200);
    check(invalid.config.rules.affordability == Core::Affordability::PositiveStamina);
    near(invalid.config.parrySoundVolume, 1.0F);
    check(!invalid.config.parryVisual);
}

void invalidBooleansDoNotEnableFeatures()
{
    for (const std::string value : {"yes", "on", "2", "-1", "trueish", "true;comment", "", "false true"}) {
        const auto result = parseConfiguration("[General]\nenabled=" + value +
                                               "\n[PerfectBlock]\nnpcEnabled=" + value);
        check(result.diagnostics.size() == 2);
        check(!result.config.enabled && !result.config.npcPerfectBlock);
    }
    const auto unrelated = parseConfiguration("[General]\nplayerAttacks=broken\n[Debug]\ntrace=broken");
    check(!unrelated.config.playerAttacks && !unrelated.config.trace);
    check(unrelated.config.npcAttacks);
}

void malformedAndDuplicateEntries()
{
    const auto duplicates = parseConfiguration(
        "[Stamina]\nlightOneHandedCost=42\nLIGHTONEHANDEDCOST=99\n"
        "[General]\nenabled=broken\nenabled=true\n");
    check(duplicates.diagnostics.size() == 3);
    near(duplicates.config.rules.lightOneHandedCost, 42);
    check(!duplicates.config.enabled);
    const auto malformed = parseConfiguration(
        "enabled=false\n[General] garbage\nenabled=false\n[Unknown]\nenabled=false\n"
        "[General]\nnot-an-assignment\n=emptykey\n[Stamina]\nchargePowerAttacks=true\n");
    check(malformed.diagnostics.size() == 8);
    check(malformed.config.enabled);
    check(!malformed.config.rules.chargePowerAttacks);
    check(malformed.diagnostics.front().starts_with("Line 1:"));
    const auto lineLimit = parseConfiguration("[General]\n" + std::string(2049, 'x') + "\nenabled=false");
    check(lineLimit.diagnostics.size() == 1 && !lineLimit.config.enabled);
    std::string many;
    for (int i = 0; i < 100; ++i) many += "bad\n";
    auto bounded = parseConfiguration(many);
    check(bounded.diagnostics.size() == 33);
    check(bounded.diagnostics.back() == "Further configuration diagnostics omitted.");
}

void invalidFilesAndSizeLimits()
{
    auto huge = parseConfiguration(std::string(65537, ' '));
    check(!huge.config.enabled && !huge.diagnostics.empty());
    auto exact = parseConfiguration(std::string(65536, '\n'));
    check(exact.config.enabled && exact.diagnostics.empty());
    std::string binary = "[General]\nenabled=true";
    binary.push_back('\0');
    binary += "ignored";
    check(!parseConfiguration(binary).config.enabled);

    TemporaryDirectory temporary;
    const auto file = temporary.path / "ApexCombat.ini";
    const auto missing = readConfiguration(file);
    check(!missing.fileLoaded && missing.config.enabled && !missing.diagnostics.empty());
    {
        std::ofstream output(file, std::ios::binary);
        output << "[Stamina]\nlightOneHandedCost=17.5\n[General]\nenabled=false\n";
        check(output.good());
    }
    const auto loaded = readConfiguration(file);
    check(loaded.fileLoaded && !loaded.config.enabled && loaded.diagnostics.empty());
    near(loaded.config.rules.lightOneHandedCost, 17.5F);
    {
        std::ofstream output(file, std::ios::binary);
        output << std::string(65537, ' ');
        check(output.good());
    }
    const auto oversized = readConfiguration(file);
    check(oversized.fileLoaded && !oversized.config.enabled && !oversized.diagnostics.empty());
    const auto directory = readConfiguration(temporary.path);
    check(!directory.fileLoaded && !directory.config.enabled && !directory.diagnostics.empty());
}
}  // namespace

int main(int argc, char** argv)
{
    defaultsAndCompleteConfiguration();
    parryVisualConfiguration();
    poiseConfiguration();
    directorConfiguration();
    potionAndRegenConfiguration();
    whitespaceCommentsAndCase();
    strictNumbersAndFiniteBounds();
    invalidBooleansDoNotEnableFeatures();
    malformedAndDuplicateEntries();
    invalidFilesAndSizeLimits();
    if (argc > 1) {
        const auto shipped = readConfiguration(argv[1]);
        check(shipped.fileLoaded && shipped.diagnostics.empty());
        check(shipped.config.enabled && !shipped.config.rules.chargePowerAttacks);
        Core::CombatRules validated(shipped.config.rules);
        check(validated.settings().perfectBlockWindowMs == 150);
        near(shipped.config.rules.daggerCost, 10);
        near(shipped.config.rules.dualWieldMultiplier, 1.6F);
        near(shipped.config.dodgeRegenDelaySeconds, 0.5F);
        near(shipped.config.bowDrawDrainPerSecond, 5.0F);
        check(shipped.config.poise);
        check(shipped.config.poiseHud);
        near(shipped.config.poiseSettings.maxPoise, 100);
        near(shipped.config.poiseSettings.swordDamage, 25);
        near(shipped.config.poiseSettings.perfectParryDamage, 50);
        check(shipped.config.parrySoundWeapon == "ApexCombat.esp|0x800");
        check(shipped.config.parrySoundShield == "ApexCombat.esp|0x801");
        near(shipped.config.parrySoundVolume, 1.0F);
        check(shipped.config.parryVisual);
        check(shipped.config.poiseSettings.recoveryDelayMs == 3000);
        check(shipped.config.poiseSettings.breakRecoveryMs == 1500);
        near(shipped.config.poiseSettings.recoveryPerSecond, 20);
        Core::PoiseRules validatedPoise(shipped.config.poiseSettings);
        check(validatedPoise.settings().maxPoise == 100);
        check(!shipped.config.director);
        check(shipped.config.directorPlayerOnly);
        Core::AiRules validatedAi(shipped.config.aiSettings);
        check(validatedAi.settings().maxAttackers == 2);
        check(validatedAi.settings().bashCooldownMs == 6000);
        near(shipped.config.circleRadiusMin, 200);
        near(shipped.config.circleRadiusMax, 320);
    }
    std::cout << "Configuration: " << checks << " checks passed\n";
}
