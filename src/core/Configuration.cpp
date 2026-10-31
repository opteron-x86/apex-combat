#include "Configuration.h"

#include <charconv>
#include <cmath>
#include <fstream>
#include <set>
#include <system_error>
#include <utility>

namespace Apex {
namespace {
constexpr std::size_t maxFileBytes = 64 * 1024;
constexpr std::size_t maxLineBytes = 2048;
constexpr std::size_t maxDiagnostics = 32;

bool whitespace(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string_view trim(std::string_view value) noexcept
{
    while (!value.empty() && whitespace(value.front())) value.remove_prefix(1);
    while (!value.empty() && whitespace(value.back())) value.remove_suffix(1);
    return value;
}

std::string lower(std::string_view value)
{
    std::string output(value);
    for (char& c : output) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return output;
}

void diagnose(ConfigurationResult& result, std::size_t line, std::string message)
{
    if (result.diagnostics.size() < maxDiagnostics) {
        result.diagnostics.push_back("Line " + std::to_string(line) + ": " + std::move(message));
    } else if (result.diagnostics.size() == maxDiagnostics) {
        result.diagnostics.emplace_back("Further configuration diagnostics omitted.");
    }
}

template <class Number>
bool number(std::string_view text, Number minimum, Number maximum, Number& target)
{
    Number value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        !std::isfinite(static_cast<double>(value)) || value < minimum || value > maximum) {
        return false;
    }
    target = value;
    return true;
}

bool boolean(std::string_view text, bool& target)
{
    const auto value = lower(text);
    if (value == "true" || value == "1") {
        target = true;
        return true;
    }
    target = false;
    return value == "false" || value == "0";
}

bool knownSection(std::string_view section)
{
    return section == "general" || section == "stamina" || section == "perfectblock" ||
           section == "debug";
}

bool knownKey(std::string_view key)
{
    return key == "general.enabled" || key == "general.playerattacks" ||
           key == "general.npcattacks" || key == "general.patchattackidles" ||
           key == "stamina.lightonehandedcost" || key == "stamina.lighttwohandedcost" ||
           key == "stamina.unarmedcost" || key == "stamina.daggercost" ||
           key == "stamina.axecost" || key == "stamina.macecost" ||
           key == "stamina.dualwieldmultiplier" || key == "stamina.affordability" ||
           key == "stamina.regendelayseconds" || key == "stamina.dodgeregendelayseconds" ||
            key == "stamina.exhaustionrecoverydelayseconds" ||
           key == "stamina.bowdrawdrainpersecond" || key == "stamina.attackcommittimeoutms" ||
           key == "perfectblock.enabled" || key == "perfectblock.npcenabled" ||
           key == "perfectblock.windowms" || key == "perfectblock.restore" ||
           key == "perfectblock.rearmdelayms" || key == "perfectblock.blockangledegrees" ||
           key == "debug.trace";
}

bool apply(Configuration& config, const std::string& key, std::string_view value)
{
    auto& rules = config.rules;
    if (key == "general.enabled") return boolean(value, config.enabled);
    if (key == "general.playerattacks") return boolean(value, config.playerAttacks);
    if (key == "general.npcattacks") return boolean(value, config.npcAttacks);
    if (key == "general.patchattackidles") return boolean(value, config.patchAttackIdles);
    if (key == "perfectblock.enabled") return boolean(value, config.perfectBlock);
    if (key == "perfectblock.npcenabled") return boolean(value, config.npcPerfectBlock);
    if (key == "debug.trace") return boolean(value, config.trace);
    if (key == "stamina.lightonehandedcost") return number(value, 0.0F, 10000.0F, rules.lightOneHandedCost);
    if (key == "stamina.lighttwohandedcost") return number(value, 0.0F, 10000.0F, rules.lightTwoHandedCost);
    if (key == "stamina.unarmedcost") return number(value, 0.0F, 10000.0F, rules.unarmedCost);
    if (key == "stamina.daggercost") return number(value, 0.0F, 10000.0F, rules.daggerCost);
    if (key == "stamina.axecost") return number(value, 0.0F, 10000.0F, rules.axeCost);
    if (key == "stamina.macecost") return number(value, 0.0F, 10000.0F, rules.maceCost);
    if (key == "stamina.dualwieldmultiplier")
        return number(value, 1.0F, 10.0F, rules.dualWieldMultiplier);
    if (key == "stamina.regendelayseconds") return number(value, 0.0F, 10.0F, config.regenDelaySeconds);
    if (key == "stamina.dodgeregendelayseconds")
        return number(value, 0.0F, 10.0F, config.dodgeRegenDelaySeconds);
    if (key == "stamina.exhaustionrecoverydelayseconds")
        return number(value, 0.0F, 10.0F, config.exhaustionRecoveryDelaySeconds);
    if (key == "stamina.bowdrawdrainpersecond")
        return number(value, 0.0F, 10000.0F, config.bowDrawDrainPerSecond);
    if (key == "stamina.attackcommittimeoutms")
        return number(value, Core::TimeMs{1}, Core::TimeMs{10000}, rules.attackCommitTimeoutMs);
    if (key == "perfectblock.windowms")
        return number(value, Core::TimeMs{1}, Core::TimeMs{1000}, rules.perfectBlockWindowMs);
    if (key == "perfectblock.restore") return number(value, 0.0F, 10000.0F, rules.perfectBlockRestore);
    if (key == "perfectblock.rearmdelayms")
        return number(value, Core::TimeMs{0}, Core::TimeMs{10000}, rules.parryRearmDelayMs);
    if (key == "perfectblock.blockangledegrees") return number(value, 1.0F, 360.0F, config.blockAngleDegrees);
    if (key == "stamina.affordability") {
        const auto mode = lower(value);
        if (mode == "positive") {
            rules.affordability = Core::Affordability::PositiveStamina;
            return true;
        }
        if (mode == "full") {
            rules.affordability = Core::Affordability::FullCost;
            return true;
        }
    }
    return false;
}
}  // namespace

ConfigurationResult parseConfiguration(std::string_view text)
{
    ConfigurationResult result;
    if (text.size() > maxFileBytes || text.find('\0') != std::string_view::npos) {
        result.config.enabled = false;
        result.diagnostics.emplace_back("Configuration exceeds 64 KiB or contains NUL bytes; plugin disabled.");
        return result;
    }
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    std::string section;
    std::set<std::string> seen;
    std::size_t lineNumber = 0;
    while (!text.empty()) {
        ++lineNumber;
        const auto newline = text.find('\n');
        auto line = text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
        if (line.size() > maxLineBytes) {
            diagnose(result, lineNumber, "Line exceeds 2048 bytes; ignored.");
            continue;
        }
        line = trim(line);
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        // Inline comments require whitespace before the marker to avoid accepting a partial token.
        for (std::size_t i = 1; i < line.size(); ++i) {
            if ((line[i] == ';' || line[i] == '#') && whitespace(line[i - 1])) {
                line = trim(line.substr(0, i));
                break;
            }
        }
        if (line.front() == '[') {
            if (line.size() < 3 || line.back() != ']' || line.find(']') != line.size() - 1) {
                section.clear();
                diagnose(result, lineNumber, "Malformed section header; subsequent keys need a valid section.");
                continue;
            }
            section = lower(trim(line.substr(1, line.size() - 2)));
            if (!knownSection(section)) diagnose(result, lineNumber, "Unknown section '" + section + "'.");
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string_view::npos || trim(line.substr(0, equals)).empty()) {
            diagnose(result, lineNumber, "Expected key=value; ignored.");
            continue;
        }
        const auto key = section + "." + lower(trim(line.substr(0, equals)));
        if (!knownKey(key)) {
            diagnose(result, lineNumber, "Unknown key '" + key + "'; ignored.");
            continue;
        }
        if (!seen.insert(key).second) {
            diagnose(result, lineNumber, "Duplicate key '" + key + "'; first occurrence retained.");
            continue;
        }
        if (!apply(result.config, key, trim(line.substr(equals + 1)))) {
            diagnose(result, lineNumber, "Invalid value for '" + key + "'; default retained, or false for a boolean.");
        }
    }
    return result;
}

ConfigurationResult readConfiguration(const std::filesystem::path& path)
{
    ConfigurationResult result;
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (!exists && !error) {
        result.diagnostics.emplace_back("Configuration file not found; using defaults.");
        return result;
    }
    std::ifstream file(path, std::ios::binary);
    if (error || !file) {
        result.config.enabled = false;
        result.diagnostics.emplace_back("Configuration file could not be opened; plugin disabled.");
        return result;
    }
    std::string content(maxFileBytes + 1, '\0');
    file.read(content.data(), static_cast<std::streamsize>(content.size()));
    const auto count = file.gcount();
    if (file.bad() || (file.fail() && !file.eof())) {
        result.config.enabled = false;
        result.diagnostics.emplace_back("Configuration file could not be read; plugin disabled.");
        return result;
    }
    content.resize(static_cast<std::size_t>(count));
    result = parseConfiguration(content);
    result.fileLoaded = true;
    return result;
}

}  // namespace Apex
