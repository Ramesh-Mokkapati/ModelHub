// ModelHubSdk.dll: the C interface of modelhub.h over HubCore.
#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "Config.h"
#include "HubCore.h"
#include "Paths.h"
#include "Text.h"
#include "modelhub/modelhub.h"

#ifndef MODELHUB_VERSION
#define MODELHUB_VERSION "0.0.0"
#endif

namespace fs = std::filesystem;
using nlohmann::json;

struct mh_hub {
    std::unique_ptr<modelhub::HubCore> core;
};

namespace {

char* Dup(const std::string& s) {
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (p) std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

fs::path DllDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&mh_version), &self);
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(self, buf, static_cast<DWORD>(std::size(buf)));
    return fs::path(std::wstring(buf, n)).parent_path();
}

}  // namespace

extern "C" {

MH_API const char* MH_CALL mh_version(void) { return MODELHUB_VERSION; }
MH_API int MH_CALL mh_api_version(void) { return MODELHUB_API_VERSION; }

MH_API mh_hub* MH_CALL mh_open(const char* optionsJson, mh_log_fn log, void* logUser, char** error) {
    if (error) *error = nullptr;
    try {
        json o = json::parse(optionsJson && *optionsJson ? optionsJson : "{}", nullptr, false);
        if (!o.is_object()) {
            if (error) *error = Dup("options must be a JSON object");
            return nullptr;
        }
        modelhub::CoreOptions co;
        co.home = o.contains("home") ? modelhub::Utf8ToPath(o.value("home", "")) : modelhub::HubHome();
        co.config = modelhub::LoadHubConfig(co.home / L"config.json");
        fs::path dll = DllDir();
        if (o.contains("catalog")) co.catalogFiles.push_back(modelhub::Utf8ToPath(o.value("catalog", "")));
        co.catalogFiles.push_back(co.home / L"catalog.json");
        co.catalogFiles.push_back(dll / L"catalog.json");
        co.catalogFiles.push_back(dll.parent_path() / L"catalog.json");  // SDK and release layout: bin/catalog.json
        for (const auto& d : o.value("search_dirs", json::array()))
            if (d.is_string()) co.extraSearchDirs.push_back(modelhub::Utf8ToPath(d.get<std::string>()));
        std::error_code ec;
        if (o.contains("llama_server")) {
            co.config.llamaServer = modelhub::Utf8ToPath(o.value("llama_server", ""));
        } else {  // llama\ next to the DLL (an application folder or the SDK), else config.json's
            for (const fs::path& p : {dll / L"llama" / L"llama-server.exe", dll.parent_path() / L"llama" / L"llama-server.exe"})
                if (fs::exists(p, ec)) {
                    co.config.llamaServer = p;
                    break;
                }
        }
        co.config.basePort = o.value("base_port", co.config.basePort);
        co.config.idleMinutes = std::max(1, o.value("idle_minutes", co.config.idleMinutes));
        co.config.parallel = std::max(1, o.value("parallel", co.config.parallel));
        co.app = o.value("app", "");
        co.scanOnInit = o.value("scan", true);
        co.hfBase = o.value("hf_base", co.hfBase);
        if (log) co.log = [log, logUser](const std::string& line) { log(logUser, line.c_str()); };
        auto hub = std::make_unique<mh_hub>();
        hub->core = std::make_unique<modelhub::HubCore>(co);
        std::string err;
        if (!hub->core->Init(&err)) {
            if (error) *error = Dup(err);
            return nullptr;
        }
        return hub.release();
    } catch (const std::exception& e) {
        if (error) *error = Dup(std::string("ModelHub: ") + e.what());
        return nullptr;
    }
}

MH_API void MH_CALL mh_close(mh_hub* hub) { delete hub; }

MH_API char* MH_CALL mh_call(mh_hub* hub, const char* method, const char* argsJson) {
    if (!hub || !method) return Dup(modelhub::HubCore::Error(400, "no hub or no method").dump());
    try {
        json args = json::parse(argsJson && *argsJson ? argsJson : "{}", nullptr, false);
        if (args.is_discarded()) return Dup(modelhub::HubCore::Error(400, "arguments are not valid JSON").dump());
        return Dup(hub->core->Call(method, args).dump());
    } catch (const std::exception& e) {
        return Dup(modelhub::HubCore::Error(500, e.what()).dump());
    }
}

MH_API char* MH_CALL mh_download(mh_hub* hub, const char* model, mh_progress_fn progress, void* user) {
    if (!hub || !model) return Dup(modelhub::HubCore::Error(400, "no hub or no model").dump());
    try {
        json r = hub->core->Download(model, [&](const json& p) {
            return !progress || progress(user, p.dump().c_str()) == 0;
        });
        return Dup(r.dump());
    } catch (const std::exception& e) {
        return Dup(modelhub::HubCore::Error(500, e.what()).dump());
    }
}

MH_API void MH_CALL mh_free(char* text) { std::free(text); }

}  // extern "C"
