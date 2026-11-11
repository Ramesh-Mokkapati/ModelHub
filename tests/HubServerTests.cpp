#include <gtest/gtest.h>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <thread>

#include "HttpClient.h"
#include "HubServer.h"
#include "Mirror.h"
#include "TestUtil.h"
#include "modelhub/modelhub_client.h"

using namespace modelhub;
namespace fs = std::filesystem;
using nlohmann::json;

namespace cxx14check {  // tests/ClientCxx14.cpp, compiled as C++14
int StatusOfHealth(int port, const std::string& token);
std::wstring PathViaClient(const std::wstring& home, const std::string& model, std::string* error);
std::string AdviseViaClient(const std::wstring& home, const std::string& task, const std::string& metrics);
}  // namespace cxx14check

namespace {

struct HubFixture : ::testing::Test {
    fs::path root, home, otherApp;
    testutil::Mirror mirror;
    std::unique_ptr<Hub> hub;
    testutil::FakeModel small{"tiny-model", "tiny.gguf", std::string(300000, 't'), "tiny:1b"};
    testutil::FakeModel remote{"remote-model", "remote.gguf", std::string(2 << 20, 'r')};

    void SetUp() override {
        root = testutil::TempDir("modelhub-hub-");
        home = root / L"home";
        otherApp = root / L"otherapp";
        fs::create_directories(otherApp);
        WriteTextFile(otherApp / L"tiny.gguf", small.content);  // another app already has this one
        for (int i = 0; i < 2 * 1024 * 1024; i += 7) remote.content[i] = static_cast<char>('a' + i % 26);
        mirror.files["remote.gguf"] = remote.content;
        std::string tasks = R"([{"id":"chat","name":"Chat","start_small":false,"tiers":[{"prefer":["remote-model","tiny-model"]}]}])";
        fs::create_directories(home);
        WriteTextFile(home / L"catalog.json",
                      testutil::CatalogJson({small, remote}, tasks, "http://127.0.0.1:" + std::to_string(mirror.port) + "/"));
        HubOptions opt;
        opt.home = home;
        opt.catalogFiles = {home / L"catalog.json"};
        opt.config.port = 0;
        opt.config.searchDirs = {otherApp};
        opt.config.llamaServer = fs::path(MODELHUB_BINARY_DIR) / L"sdk" / L"bin" / L"llama" / L"llama-server.exe";
        hub = std::make_unique<Hub>(opt);
        std::string err;
        ASSERT_TRUE(hub->Start(&err)) << err;
    }
    void TearDown() override {
        hub.reset();
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    std::string Url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(hub->Port()) + path; }
    HttpHeaders Auth() const { return {{"Authorization", "Bearer " + hub->Token()}}; }
    json GetJson(const std::string& path) {
        auto r = HttpGet(Url(path), Auth(), 10000);
        EXPECT_TRUE(r.Ok()) << path << " " << r.status << " " << r.body;
        return json::parse(r.body, nullptr, false);
    }
};

}  // namespace

TEST_F(HubFixture, RequiresTokenAndLocalHost) {
    EXPECT_EQ(HttpGet(Url("/health")).status, 200);
    EXPECT_EQ(HttpGet(Url("/hub/v1/status")).status, 401);
    EXPECT_EQ(HttpGet(Url("/hub/v1/status"), {{"Authorization", "Bearer wrong"}}).status, 401);
    EXPECT_EQ(HttpGet(Url("/hub/v1/status"), Auth()).status, 200);
    auto evil = Auth();
    evil.emplace_back("Host", "attacker.example:" + std::to_string(hub->Port()));
    EXPECT_EQ(HttpGet(Url("/v1/models"), evil).status, 403);
    EXPECT_EQ(cxx14check::StatusOfHealth(hub->Port(), hub->Token()), 200);
}

TEST_F(HubFixture, FindsOtherApplicationsFileAndServesItsPathToTheCxx14Client) {
    std::string err;
    std::wstring p = cxx14check::PathViaClient(home.wstring(), "tiny:1b", &err);  // alias; waits for the scan
    EXPECT_EQ(fs::path(p), otherApp / L"tiny.gguf") << err;
    json m = GetJson("/hub/v1/models/tiny-model");
    EXPECT_TRUE(m["installed"].get<bool>());
    EXPECT_FALSE(m["in_hub_folder"].get<bool>());
    EXPECT_EQ(m["users"], json::array({"cxx14-test"}));
    json models = GetJson("/v1/models");
    ASSERT_EQ(models["data"].size(), 1u);
    EXPECT_EQ(models["data"][0]["id"], "tiny-model");
}

TEST_F(HubFixture, ApplicationsCanShareTheirModelFolders) {
    fs::path appFolder = root / L"appassist-models";
    fs::create_directories(appFolder);
    WriteTextFile(appFolder / L"remote.gguf", remote.content);  // an app already has this one
    auto bad = HttpPost(Url("/hub/v1/folders"), R"({"path":"relative\\dir"})", Auth(), 10000);
    EXPECT_EQ(bad.status, 400);
    json body = {{"path", PathToUtf8(appFolder)}};
    auto r = HttpPost(Url("/hub/v1/folders"), body.dump(), Auth(), 10000);
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_TRUE(json::parse(r.body)["added"].get<bool>());
    EXPECT_FALSE(json::parse(HttpPost(Url("/hub/v1/folders"), body.dump(), Auth(), 10000).body)["added"].get<bool>());
    json m;
    for (int i = 0; i < 100; ++i) {  // 503 while the hub is still checking the new folder
        m = json::parse(HttpGet(Url("/hub/v1/models/remote-model"), Auth(), 10000).body, nullptr, false);
        if (m.is_object() && m["installed"].get<bool>()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_TRUE(m["installed"].get<bool>());
    EXPECT_EQ(fs::path(Widen(m["path"].get<std::string>())), appFolder / L"remote.gguf");
    EXPECT_EQ(mirror.requests.load(), 0);  // nothing was downloaded
    // Kept for the next start.
    json cfg = json::parse(ReadTextFile(home / L"config.json"));
    EXPECT_NE(cfg.dump().find("appassist-models"), std::string::npos);
}

TEST_F(HubFixture, AdvisesThenDownloadsResumesAndVerifies) {
    while (GetJson("/hub/v1/status")["scanning"].get<bool>()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    json advice = json::parse(cxx14check::AdviseViaClient(home.wstring(), "chat", ""), nullptr, false);
    ASSERT_TRUE(advice.is_object());
    EXPECT_EQ(advice["recommended"], "remote-model");
    EXPECT_FALSE(advice["recommended_installed"].get<bool>());

    // Half a download left over from an earlier attempt: it is resumed, not restarted.
    fs::create_directories(home / L"models");
    WriteTextFile(home / L"models" / L"remote.gguf.part", remote.content.substr(0, remote.content.size() / 2));
    auto start = HttpPost(Url("/hub/v1/downloads"), R"({"model":"remote-model"})", Auth(), 10000);
    EXPECT_EQ(start.status, 202) << start.body;
    json d;
    for (int i = 0; i < 200; ++i) {
        d = GetJson("/hub/v1/downloads")["downloads"][0];
        if (!d["running"].get<bool>()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_EQ(d["phase"], "done") << d.dump();
    EXPECT_EQ(ReadTextFile(home / L"models" / L"remote.gguf"), remote.content);
    EXPECT_FALSE(fs::exists(home / L"models" / L"remote.gguf.part"));
    json m = GetJson("/hub/v1/models/remote-model");
    EXPECT_TRUE(m["installed"].get<bool>());
    EXPECT_TRUE(m["in_hub_folder"].get<bool>());
    EXPECT_EQ(HttpPost(Url("/hub/v1/downloads"), R"({"model":"remote-model"})", Auth(), 10000).status, 200);  // installed
}

TEST_F(HubFixture, DeletesAModelOverHttp) {
    while (GetJson("/hub/v1/status")["scanning"].get<bool>()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_TRUE(GetJson("/hub/v1/models/tiny-model")["installed"].get<bool>());
    auto r = HttpRequest("DELETE", Url("/hub/v1/models/tiny-model?recycle=false"), "", Auth(), 10000);
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_EQ(json::parse(r.body)["deleted"].size(), 1u);
    EXPECT_FALSE(fs::exists(otherApp / L"tiny.gguf"));
    EXPECT_FALSE(GetJson("/hub/v1/models/tiny-model")["installed"].get<bool>());
    EXPECT_EQ(HttpRequest("DELETE", Url("/hub/v1/models/tiny-model"), "", Auth(), 10000).status, 404);
    EXPECT_EQ(HttpRequest("DELETE", Url("/hub/v1/models/tiny-model"), "", {}, 10000).status, 401);
}

TEST_F(HubFixture, InferenceErrorsAreOpenAiShaped) {
    auto unknown = HttpPost(Url("/v1/chat/completions"), R"({"model":"nope","messages":[]})", Auth(), 10000);
    EXPECT_EQ(unknown.status, 404);
    EXPECT_NE(json::parse(unknown.body)["error"]["message"].get<std::string>().find("unknown model"), std::string::npos);
    auto missing = HttpPost(Url("/v1/chat/completions"), R"({"model":"remote-model","messages":[]})", Auth(), 10000);
    EXPECT_EQ(missing.status, 404);
    EXPECT_EQ(json::parse(missing.body)["error"]["type"], "model_not_installed");
    // tiny.gguf is not a real model: llama-server starts, fails to load it, and the hub reports why.
    std::string err;
    cxx14check::PathViaClient(home.wstring(), "tiny-model", &err);
    if (!fs::exists(fs::path(MODELHUB_BINARY_DIR) / L"sdk" / L"bin" / L"llama" / L"llama-server.exe")) GTEST_SKIP() << "no llama-server";
    auto broken = HttpPost(Url("/v1/chat/completions"), R"({"model":"tiny-model","messages":[{"role":"user","content":"hi"}]})",
                           Auth(), 120000);
    EXPECT_EQ(broken.status, 503) << broken.body;
    EXPECT_NE(broken.body.find("exited while loading"), std::string::npos) << broken.body;
}

// Real inference through the hub (set MODELHUB_IT_MODEL to a GGUF file from the shipped catalog).
TEST(HubIntegration, ChatThroughTheHub) {
    char buf[1024];
    size_t n = 0;
    if (getenv_s(&n, buf, "MODELHUB_IT_MODEL") || n == 0) GTEST_SKIP() << "set MODELHUB_IT_MODEL=<path to a catalog .gguf>";
    fs::path model = Widen(buf);
    auto root = testutil::TempDir("modelhub-it-");
    HubOptions opt;
    opt.home = root;
    opt.catalogFiles = {fs::path(MODELHUB_SOURCE_DIR) / L"config" / L"catalog.json"};
    opt.config.port = 0;
    opt.config.searchDirs = {model.parent_path()};
    opt.config.llamaServer = fs::path(MODELHUB_BINARY_DIR) / L"sdk" / L"bin" / L"llama" / L"llama-server.exe";
    Hub hub(opt);
    std::string err;
    ASSERT_TRUE(hub.Start(&err)) << err;
    std::string url = "http://127.0.0.1:" + std::to_string(hub.Port());
    HttpHeaders auth = {{"Authorization", "Bearer " + hub.Token()}};
    json models;
    for (int i = 0; i < 600; ++i) {  // hashing a multi-GB file takes a while
        json s = json::parse(HttpGet(url + "/hub/v1/status", auth).body);
        if (!s["scanning"].get<bool>()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    models = json::parse(HttpGet(url + "/v1/models", auth).body);
    ASSERT_FALSE(models["data"].empty()) << "the file is not in the catalog or failed verification";
    std::string id = models["data"][0]["id"];
    json req = {{"model", id}, {"messages", {{{"role", "user"}, {"content", "Reply with the single word: ready"}}}},
                {"max_tokens", 8}, {"temperature", 0}};
    auto r = HttpPost(url + "/v1/chat/completions", req.dump(), auth, 600000);
    ASSERT_EQ(r.status, 200) << r.body;
    EXPECT_FALSE(json::parse(r.body)["choices"][0]["message"]["content"].get<std::string>().empty());
    req["stream"] = true;
    std::string streamed;
    auto s = HttpRequestStream("POST", url + "/v1/chat/completions", req.dump(), auth, 600000, [&](std::string_view c) {
        streamed += c;
        return true;
    });
    EXPECT_TRUE(s.Ok()) << s.error;
    EXPECT_NE(streamed.find("data: "), std::string::npos);
    json status = json::parse(HttpGet(url + "/hub/v1/status", auth).body);
    ASSERT_EQ(status["servers"].size(), 1u);  // both requests used one llama-server
    std::cout << "  " << id << ": " << status["servers"][0].dump() << "\n";
}
