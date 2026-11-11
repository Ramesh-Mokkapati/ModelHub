// ModelHubSdk.dll through its C interface (modelhub.h) and the C++14 wrapper (modelhub.hpp).
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <thread>

#include "Mirror.h"
#include "TestUtil.h"
#include "modelhub/modelhub.hpp"

using namespace modelhub;
namespace fs = std::filesystem;
using nlohmann::json;

namespace cxx14check {
std::string SdkCallViaCxx14(const std::string& options, const std::string& method, const std::string& args,
                            std::string* error);
}

namespace {

struct SdkFixture : ::testing::Test {
    fs::path root, home, appFolder;
    testutil::Mirror mirror;
    testutil::FakeModel shared{"shared-model", "shared.gguf", std::string(300000, 's'), "shared:1b"};
    testutil::FakeModel remote{"remote-model", "remote.gguf", std::string(9 << 20, 'r')};  // > 4 MB: progress is reported
    std::string options;

    void SetUp() override {
        root = testutil::TempDir("modelhub-sdk-");
        home = root / L"home";
        appFolder = root / L"app-models";
        fs::create_directories(appFolder);
        fs::create_directories(home);
        WriteTextFile(appFolder / L"shared.gguf", shared.content);  // an application already has this one
        for (size_t i = 0; i < remote.content.size(); i += 5) remote.content[i] = static_cast<char>('a' + i % 26);
        mirror.files["remote.gguf"] = remote.content;
        std::string tasks = R"([{"id":"chat","name":"Chat","start_small":false,"tiers":[{"prefer":["remote-model","shared-model"]}]}])";
        WriteTextFile(home / L"catalog.json",
                      testutil::CatalogJson({shared, remote}, tasks, "http://127.0.0.1:" + std::to_string(mirror.port) + "/"));
        // No default search folders: only what the test adds.
        WriteTextFile(home / L"config.json", R"({"search_dirs": []})");
        options = json({{"app", "SdkTest"},
                        {"home", PathToUtf8(home)},
                        {"llama_server", PathToUtf8(fs::path(MODELHUB_BINARY_DIR) / L"sdk" / L"bin" / L"llama" / L"llama-server.exe")}})
                      .dump();
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    static json Call(Sdk& sdk, const std::string& method, const json& args = json::object()) {
        return json::parse(sdk.Call(method, args.dump()));
    }
};

}  // namespace

TEST_F(SdkFixture, CApiBasics) {
    EXPECT_STREQ(mh_version(), MODELHUB_VERSION);  // the version CMake was configured with
    EXPECT_EQ(mh_api_version(), MODELHUB_API_VERSION);
    char* err = nullptr;
    EXPECT_EQ(mh_open("not json", nullptr, nullptr, &err), nullptr);
    ASSERT_NE(err, nullptr);
    mh_free(err);
    mh_hub* hub = mh_open(options.c_str(), nullptr, nullptr, &err);
    ASSERT_NE(hub, nullptr) << (err ? err : "");
    char* r = mh_call(hub, "nonsense", nullptr);
    EXPECT_EQ(json::parse(r)["status"], 404);
    mh_free(r);
    r = mh_call(hub, "status", "{bad");
    EXPECT_EQ(json::parse(r)["status"], 400);
    mh_free(r);
    mh_close(hub);
}

TEST_F(SdkFixture, SharesFoldersAdvisesAndFitsLikeTheService) {
    Sdk sdk;
    std::string err;
    ASSERT_TRUE(sdk.Open(options, nullptr, &err)) << err;
    EXPECT_EQ(Call(sdk, "catalog")["models"].size(), 2u);
    json added = Call(sdk, "add_folder", {{"path", PathToUtf8(appFolder)}});
    EXPECT_TRUE(added["added"].get<bool>());
    Call(sdk, "wait_scan", {{"timeout_ms", 20000}});
    json m = Call(sdk, "model", {{"id", "shared:1b"}});  // alias
    ASSERT_TRUE(m["installed"].get<bool>()) << m.dump();
    EXPECT_EQ(fs::path(Widen(m["path"].get<std::string>())), appFolder / L"shared.gguf");
    EXPECT_EQ(m["users"], json::array({"SdkTest"}));
    EXPECT_NE(ReadTextFile(home / L"config.json").find("app-models"), std::string::npos);  // shared with everyone

    json advice = Call(sdk, "advise", {{"task", "chat"}});
    EXPECT_EQ(advice["recommended"], "remote-model");
    EXPECT_FALSE(advice["recommended_installed"].get<bool>());
    ASSERT_EQ(advice["options"].size(), 2u);
    EXPECT_FALSE(advice["options"][0]["speed"].get<std::string>().empty());

    json fits = Call(sdk, "fits", {{"context_tokens", 4096}, {"parallel", 1}, {"engine", "embedded-cpu"}});
    ASSERT_EQ(fits["models"].size(), 2u);
    EXPECT_EQ(fits["models"][0]["gpu_layers"], 0);  // embedded-cpu: no GPU
    EXPECT_EQ(Call(sdk, "advise", {{"task", "nope"}})["status"], 404);

    // The C++14 wrapper sees the same store.
    std::string viaCxx14 = cxx14check::SdkCallViaCxx14(options, "models", "{}", &err);
    EXPECT_NE(viaCxx14.find("shared-model"), std::string::npos) << err;
}

TEST_F(SdkFixture, DownloadsOnceForEveryProcessAndCanBeCancelled) {
    Sdk a, b;  // two applications sharing the store
    std::string err;
    ASSERT_TRUE(a.Open(options, nullptr, &err)) << err;
    ASSERT_TRUE(b.Open(options, nullptr, &err)) << err;
    Call(a, "wait_scan");
    Call(b, "wait_scan");

    // Cancelled on the first progress report: nothing installed, the partial file stays for later.
    json cancelled = json::parse(a.Download("remote-model", [](const std::string&) { return false; }));
    EXPECT_EQ(cancelled["error"]["type"], "cancelled") << cancelled.dump();

    std::atomic<int> reports{0};
    json done = json::parse(a.Download("remote-model", [&](const std::string& p) {
        EXPECT_NE(p.find("\"model\":\"remote-model\""), std::string::npos);
        ++reports;
        return true;
    }));
    ASSERT_TRUE(done.value("installed", false)) << done.dump();
    EXPECT_GT(reports.load(), 0);
    EXPECT_EQ(ReadTextFile(Widen(done["path"].get<std::string>())), remote.content);

    // The other process sees it at once (shared manifest), without scanning; downloading again is a no-op.
    EXPECT_TRUE(Call(b, "model", {{"id", "remote-model"}})["installed"].get<bool>());
    int before = mirror.requests.load();
    json again = json::parse(b.Download("remote-model"));
    EXPECT_TRUE(again.value("installed", false));
    EXPECT_EQ(mirror.requests.load(), before);
}

// A download asked for while the files are still being checked waits for the check instead of fetching a model
// some application already has.
TEST_F(SdkFixture, DownloadWaitsForTheFileCheck) {
    WriteTextFile(appFolder / L"remote.gguf", remote.content);
    Sdk sdk;
    ASSERT_TRUE(sdk.Open(options));
    Call(sdk, "add_folder", {{"path", PathToUtf8(appFolder)}, {"persist", false}});  // starts a check
    json r = json::parse(sdk.Download("remote-model"));
    ASSERT_TRUE(r.value("installed", false)) << r.dump();
    EXPECT_EQ(fs::path(Widen(r["path"].get<std::string>())), appFolder / L"remote.gguf");
    EXPECT_EQ(mirror.requests.load(), 0);
}

TEST_F(SdkFixture, CustomModelsAreDownloadableAndPersist) {
    std::string extra(4096, 'c');
    mirror.files["custom.gguf"] = extra;
    {
        Sdk sdk;
        std::string err;
        ASSERT_TRUE(sdk.Open(options, nullptr, &err)) << err;
        json entry = {{"id", "my-custom"},
                      {"file", "custom.gguf"},
                      {"url", "http://127.0.0.1:" + std::to_string(mirror.port) + "/custom.gguf"},
                      {"size", extra.size()},
                      {"sha256", Sha256(extra)}};
        json r = Call(sdk, "add_model", entry);
        ASSERT_TRUE(r.value("added", false)) << r.dump();
        EXPECT_FALSE(Call(sdk, "add_model", entry).value("added", true));  // already there
        EXPECT_EQ(Call(sdk, "add_model", {{"id", "bad"}, {"file", "x.gguf"}})["status"], 400);
        json d = json::parse(sdk.Download("my-custom"));
        EXPECT_TRUE(d.value("installed", false)) << d.dump();
        EXPECT_EQ(Call(sdk, "resolve_hf", {{"spec", "not a spec"}})["status"], 400);
    }
    Sdk reopened;  // custom_models.json is read again
    ASSERT_TRUE(reopened.Open(options));
    Call(reopened, "wait_scan");
    EXPECT_TRUE(Call(reopened, "model", {{"id", "my-custom"}})["installed"].get<bool>());
}

TEST_F(SdkFixture, ServerStartReportsWhyAModelCannotRun) {
    if (!fs::exists(fs::path(MODELHUB_BINARY_DIR) / L"sdk" / L"bin" / L"llama" / L"llama-server.exe")) GTEST_SKIP();
    Sdk sdk;
    ASSERT_TRUE(sdk.Open(options));
    Call(sdk, "add_folder", {{"path", PathToUtf8(appFolder)}, {"persist", false}});
    Call(sdk, "wait_scan");
    EXPECT_EQ(Call(sdk, "server_start", {{"model", "remote-model"}})["error"]["type"], "model_not_installed");
    json r = Call(sdk, "server_start", {{"model", "shared-model"}});  // not a real GGUF
    EXPECT_EQ(r["status"], 503) << r.dump();
    EXPECT_NE(r["error"]["message"].get<std::string>().find("exited while loading"), std::string::npos);
}

TEST_F(SdkFixture, DeletesEveryCopyOfAModelAndItsPartialDownload) {
    WriteTextFile(appFolder / L"remote.gguf", remote.content);  // a copy in an application's folder
    Sdk sdk;
    ASSERT_TRUE(sdk.Open(options));
    Call(sdk, "add_folder", {{"path", PathToUtf8(appFolder)}, {"persist", false}});
    Call(sdk, "wait_scan");
    Call(sdk, "model", {{"id", "remote-model"}, {"app", "OtherApp"}});  // records a user
    fs::path part = home / L"models" / L"shared.gguf.part";
    fs::create_directories(part.parent_path());
    WriteTextFile(part, "partial");

    EXPECT_EQ(Call(sdk, "delete_model", {{"model", "nope"}})["status"], 404);
    json r = Call(sdk, "delete_model", {{"model", "remote-model"}, {"recycle", false}});  // tests keep the Recycle Bin clean
    ASSERT_EQ(r["deleted"].size(), 1u) << r.dump();
    EXPECT_FALSE(fs::exists(appFolder / L"remote.gguf"));
    EXPECT_EQ(r["users"], json::array({"OtherApp"}));
    EXPECT_FALSE(r["installed"].get<bool>());
    EXPECT_FALSE(Call(sdk, "model", {{"id", "remote-model"}})["installed"].get<bool>());
    EXPECT_EQ(Call(sdk, "delete_model", {{"model", "remote-model"}})["error"]["type"], "model_not_installed");

    // Another process sees it gone (manifest.json).
    Sdk other;
    ASSERT_TRUE(other.Open(json({{"app", "Other"}, {"home", PathToUtf8(home)}, {"scan", false}}).dump()));
    EXPECT_FALSE(Call(other, "model", {{"id", "remote-model"}})["installed"].get<bool>());

    // A model's partial download in the store goes with its files.
    r = Call(sdk, "delete_model", {{"model", "shared-model"}, {"recycle", false}});
    EXPECT_EQ(r["deleted"].size(), 2u) << r.dump();
    EXPECT_FALSE(fs::exists(part));
}

TEST_F(SdkFixture, DeleteRefusesWhileTheModelDownloads) {
    mirror.delayMs = 200;  // slow enough to still be running
    Sdk sdk;
    ASSERT_TRUE(sdk.Open(options));
    Call(sdk, "wait_scan");
    ASSERT_EQ(Call(sdk, "start_download", {{"model", "remote-model"}})["status"], 202);
    json r = Call(sdk, "delete_model", {{"model", "remote-model"}});
    EXPECT_EQ(r["status"], 409) << r.dump();
    EXPECT_EQ(r["error"]["type"], "download_running");
    Call(sdk, "cancel_download", {{"model", "remote-model"}});
}
