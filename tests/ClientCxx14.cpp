// Compiles the public headers as C++14 (the standard AppAssist builds with), so a C++17/20 feature slipping
// into them fails the build. ModelHubTests calls these to exercise the headers.
#include "modelhub/modelhub.hpp"
#include "modelhub/modelhub_client.h"

namespace cxx14check {

int StatusOfHealth(int port, const std::string& token) {
    modelhub::client::HubInfo info;
    info.port = port;
    info.token = token;
    return modelhub::client::Request(info, L"GET", "/health", "").status;
}

std::wstring PathViaClient(const std::wstring& home, const std::string& model, std::string* error) {
    modelhub::client::Client c("cxx14-test", home);
    if (!c.Connect(false, 0, error)) return std::wstring();
    return c.ModelPath(model, error, 5000);
}

std::string AdviseViaClient(const std::wstring& home, const std::string& task, const std::string& metrics) {
    modelhub::client::Client c("cxx14-test", home);
    if (!c.Connect(false)) return std::string();
    return c.Advise(task, "hub", metrics, "test workload").body;
}

// The SDK wrapper (modelhub.hpp) from C++14: open, one command, close.
std::string SdkCallViaCxx14(const std::string& options, const std::string& method, const std::string& args,
                            std::string* error) {
    modelhub::Sdk sdk;
    if (!sdk.Open(options, modelhub::Sdk::LogFn(), error)) return std::string();
    return sdk.Call(method, args);
}

}  // namespace cxx14check
