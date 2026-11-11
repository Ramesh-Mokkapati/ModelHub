// modelhub.hpp - header-only C++14 wrapper for ModelHubSdk.dll (see modelhub.h).
//
//   modelhub::Sdk hub;
//   std::string error;
//   if (hub.Open("{\"app\":\"MyApp\"}", nullptr, &error)) {
//       std::string advice = hub.Call("advise", "{\"task\":\"log-diagnosis\",\"engine\":\"embedded-cpu\"}");
//       std::string model = hub.Download("qwen2.5-coder-7b", [](const std::string& progress) { return true; });
//   }
//
// Results are JSON text: parse them with the JSON library the application already uses.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "modelhub.h"

namespace modelhub {

class Sdk {
public:
    typedef std::function<void(const std::string&)> LogFn;
    typedef std::function<bool(const std::string& progressJson)> ProgressFn;  // false = cancel

    Sdk() : hub_(nullptr) {}
    ~Sdk() { Close(); }

    bool Open(const std::string& optionsJson, LogFn log = LogFn(), std::string* error = nullptr) {
        Close();
        log_.reset(log ? new LogFn(log) : nullptr);
        char* err = nullptr;
        hub_ = mh_open(optionsJson.c_str(), log_ ? &Sdk::OnLog : nullptr, log_.get(), &err);
        if (err) {
            if (error) *error = err;
            mh_free(err);
        }
        return hub_ != nullptr;
    }

    void Close() {
        if (hub_) mh_close(hub_);
        hub_ = nullptr;
    }

    bool IsOpen() const { return hub_ != nullptr; }

    std::string Call(const std::string& method, const std::string& argsJson = "{}") const {
        if (!hub_) return "{\"error\":{\"message\":\"ModelHub is not open\",\"type\":\"hub_error\"},\"status\":503}";
        return Take(mh_call(hub_, method.c_str(), argsJson.c_str()));
    }

    // Blocking; onProgress may be empty.
    std::string Download(const std::string& model, ProgressFn onProgress = ProgressFn()) const {
        if (!hub_) return "{\"error\":{\"message\":\"ModelHub is not open\",\"type\":\"hub_error\"},\"status\":503}";
        return Take(mh_download(hub_, model.c_str(), onProgress ? &Sdk::OnProgress : nullptr, &onProgress));
    }

    static std::string Version() { return mh_version(); }

private:
    Sdk(const Sdk&);
    Sdk& operator=(const Sdk&);

    static std::string Take(char* text) {
        std::string s = text ? text : "";
        if (text) mh_free(text);
        return s;
    }
    static void MH_CALL OnLog(void* user, const char* line) { (*static_cast<LogFn*>(user))(line ? line : ""); }
    static int MH_CALL OnProgress(void* user, const char* progress) {
        return (*static_cast<ProgressFn*>(user))(progress ? progress : "") ? 0 : 1;
    }

    mh_hub* hub_;
    std::unique_ptr<LogFn> log_;
};

}  // namespace modelhub
