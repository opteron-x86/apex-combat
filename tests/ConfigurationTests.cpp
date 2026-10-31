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
    near(defaults.config.rules.perfectBlockRestore, 25);
    near(defaults.config.regenDelaySeconds, 0.5F);
    near(defaults.config.blockAngleDegrees, 120);
    check(defaults.config.rules.affordability == Core::Affordability::PositiveStamina);
    check(defaults.config.rules.perfectBlockWindowMs == 150);
    check(defaults.config.rules.parryRearmDelayMs == 200);

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
affordability=full
regenDelaySeconds=2.25
attackCommitTimeoutMs=1500
[PerfectBlock]
enabled=false
npcEnabled=0
windowMs=125
restore=42.5
rearmDelayMs=0
blockAngleDegrees=90
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
    near(full.config.rules.perfectBlockRestore, 42.5F);
    near(full.config.regenDelaySeconds, 2.25F);
    near(full.config.blockAngleDegrees, 90);
    check(full.config.rules.affordability == Core::Affordability::FullCost);
    check(full.config.rules.attackCommitTimeoutMs == 1500);
    check(full.config.rules.perfectBlockWindowMs == 125);
    check(full.config.rules.parryRearmDelayMs == 0);
    Core::CombatRules validated(full.config.rules);
    check(validated.settings().perfectBlockWindowMs == 125);
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
        "[ PerfectBlock ]\r\n"
        "windowMS = 149\r\n"
        "[Debug]\r\n"
        "trace = 1");
    check(result.diagnostics.empty());
    check(result.config.enabled && !result.config.playerAttacks && result.config.trace);
    check(result.config.rules.affordability == Core::Affordability::FullCost);
    near(result.config.rules.lightOneHandedCost, 125);
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
regenDelaySeconds=10
attackCommitTimeoutMs=1
[PerfectBlock]
windowMs=1000
restore=10000
rearmDelayMs=10000
blockAngleDegrees=360
)ini");
    check(limits.diagnostics.empty());
    near(limits.config.rules.lightOneHandedCost, 0);
    near(limits.config.rules.lightTwoHandedCost, 10000);
    check(limits.config.rules.attackCommitTimeoutMs == 1);
    check(limits.config.rules.perfectBlockWindowMs == 1000);
    near(limits.config.blockAngleDegrees, 360);
    Core::CombatRules validated(limits.config.rules);
    check(validated.settings().parryRearmDelayMs == 10000);
    const auto invalid = parseConfiguration(R"ini(
[Stamina]
regenDelaySeconds=10.01
attackCommitTimeoutMs=0
affordability=anything
[PerfectBlock]
restore=-1
rearmDelayMs=-1
blockAngleDegrees=0
)ini");
    check(invalid.diagnostics.size() == 6);
    near(invalid.config.regenDelaySeconds, 0.5F);
    near(invalid.config.blockAngleDegrees, 120);
    near(invalid.config.rules.perfectBlockRestore, 25);
    check(invalid.config.rules.attackCommitTimeoutMs == 1000);
    check(invalid.config.rules.parryRearmDelayMs == 200);
    check(invalid.config.rules.affordability == Core::Affordability::PositiveStamina);
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
    }
    std::cout << "Configuration: " << checks << " checks passed\n";
}
