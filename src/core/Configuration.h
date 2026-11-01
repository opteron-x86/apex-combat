#pragma once

#include "AiRules.h"
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
    Core::AiSettings aiSettings;
    // Combat director (attack slots, bash discipline, circling). Staged
    // migration: off in code and the shipped INI until validated in game.
    bool director{false};
    // Limits slot pacing to NPCs targeting the player; NPC-vs-NPC stays vanilla.
    bool directorPlayerOnly{true};
    // Circling ring radius band (game units) for non-slot holders.
    float circleRadiusMin{200.0F};
    float circleRadiusMax{320.0F};
    bool enabled{true};
    bool playerAttacks{true};
    bool npcAttacks{true};
    // Custom poise owning stagger. The shipped INI enables it; the code
    // default stays off so a missing config cannot double-stagger alongside
    // an older poise mod.
    bool poise{false};
    // Publishes poise through TrueHUD's special resource bar. Only consulted
    // when poise itself is enabled.
    bool poiseHud{true};
    bool perfectBlock{true};
    bool npcPerfectBlock{true};
    // Perfect-parry feedback sounds, "Plugin.esp|0xFormID" of a sound
    // descriptor. Empty disables feedback; an unresolvable spec warns once
    // and degrades to silence. Defaults point at the shipped ApexCombat.esp
    // records (Apex_ParryWeaponPerfect_SD / Apex_ParryShieldPerfect_SD);
    // without that plugin the clangs are silent (VFX still fire).
    std::string parrySoundWeapon{"ApexCombat.esp|0x800"};
    std::string parrySoundShield{"ApexCombat.esp|0x801"};
    // Scales the parry clang volume: 0..2, where 1 is the descriptor level.
    float parrySoundVolume{1.0F};
    // Parry spark VFX: attaches the vanilla metal-spark impact root at the
    // blocking weapon/shield node so the engine fires its own spark. Independent
    // of the clang.
    bool parryVisual{true};
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
