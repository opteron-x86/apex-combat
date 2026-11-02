#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/sinks/basic_file_sink.h>
#include "runtime/Runtime.h"

namespace {
void OnMessage(SKSE::MessagingInterface::Message* message) {
    if (!message) return;
    switch (message->type) {
    case SKSE::MessagingInterface::kDataLoaded:
        if (!Apex::InitializeRuntime()) SKSE::log::error("Apex Combat could not enable combat features.");
        break;
    case SKSE::MessagingInterface::kPreLoadGame:
        Apex::ResetSession();
        break;
    case SKSE::MessagingInterface::kPostLoadGame:
        if (message->data) Apex::StartSession();
        break;
    case SKSE::MessagingInterface::kNewGame:
        Apex::StartSession();
        break;
    default: break;
    }
}
}

extern "C" __declspec(dllexport) constinit auto SKSEPlugin_Version = []() noexcept {
    SKSE::PluginVersionData data{};
    data.PluginName("ApexCombat");
    data.AuthorName("Apex Combat contributors");
    data.PluginVersion(REL::Version(APEX_VERSION_MAJOR, APEX_VERSION_MINOR, APEX_VERSION_PATCH, 0));
    data.UsesAddressLibrary();
    data.UsesNoStructs();
    return data;
}();

extern "C" __declspec(dllexport) bool SKSEPlugin_Query(
    const SKSE::QueryInterface* skse, SKSE::PluginInfo* info) {
    if (!skse || !info || skse->IsEditor()) return false;
    info->infoVersion = SKSE::PluginInfo::kVersion;
    info->name = SKSEPlugin_Version.pluginName;
    info->version = SKSEPlugin_Version.pluginVersion;
    return true;
}

extern "C" __declspec(dllexport) bool SKSEPlugin_Load(const SKSE::LoadInterface* skse) {
    if (!skse || skse->IsEditor()) return false;
    try {
        auto directory = SKSE::log::log_directory();
        if (!directory) return false;
        *directory /= "ApexCombat.log";
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(directory->string(), true);
        auto logger = std::make_shared<spdlog::logger>("global", std::move(sink));
        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::info);
        spdlog::set_default_logger(std::move(logger));
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        SKSE::InitInfo init;
        init.log = false;
        SKSE::Init(skse, init);
        SKSE::log::info("Apex Combat {}; Skyrim {}; CommonLibSSE-NG", APEX_VERSION, skse->RuntimeVersion().string());
        auto* messaging = SKSE::GetMessagingInterface();
        auto* serialization = SKSE::GetSerializationInterface();
        if (!messaging || !serialization || !SKSE::GetTaskInterface()) return false;
        serialization->SetUniqueID(0x41504558);
        serialization->SetRevertCallback([](auto*) { Apex::ResetSession(); });
        return messaging->RegisterListener(OnMessage);
    } catch (const std::exception& error) {
        if (spdlog::default_logger()) SKSE::log::critical("Load failed: {}", error.what());
        return false;
    }
}
