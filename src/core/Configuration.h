#pragma once

#include "CombatRules.h"
#include "PoiseRules.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Apex {

struct Configuration {
    Core::Settings rules;
    Core::PoiseSettings poiseSettings;
    bool enabled{true};
    bool playerAttacks{true};
    bool npcAttacks{true};
    // Custom poise is opt-in: Chocolate Poise stays the owner until this is
    // enabled and that mod is retired (migration rule from the design doc).
    bool poise{false};
    bool perfectBlock{true};
    bool npcPerfectBlock{true};
    float blockAngleDegrees{120.0F};
    float regenDelaySeconds{0.5F};
    float dodgeRegenDelaySeconds{0.5F};
    float exhaustionRecoveryDelaySeconds{1.0F};
    float bowDrawDrainPerSecond{5.0F};
    bool trace{false};
    bool patchAttackIdles{true};
};

struct ConfigurationResult {
    Configuration config;
    std::vector<std::string> diagnostics;
    bool fileLoaded{false};
};

// Keys and sections are case-insensitive. Values must be complete valid tokens.
// Invalid numbers/enums retain defaults; invalid booleans become false.
// The first occurrence of a recognized key wins. Diagnostics include line numbers.
ConfigurationResult parseConfiguration(std::string_view text);
// A missing file uses defaults. An unreadable or oversized file disables the plugin.
ConfigurationResult readConfiguration(const std::filesystem::path& path);

}  // namespace Apex
