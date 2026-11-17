// Lessons shared by every application that uses the hub: records per key, a playbook per domain, seeding.
#include <gtest/gtest.h>

#include <thread>

#include "HubCore.h"
#include "Lessons.h"
#include "TestUtil.h"

using namespace modelhub;
namespace fs = std::filesystem;
using nlohmann::json;

TEST(Lessons, RecordsAreAppendedDedupedAndQueriedByKeyAndModel)
{
    LessonStore s(testutil::TempDir("modelhub-lessons-") / L"lessons");
    json r1 = { { "key", "C2065" }, { "model", "m1" }, { "removed", { "a" } }, { "added", { "b" } } };
    json r2 = { { "key", "C2224" }, { "model", "m2" }, { "removed", { "x" } }, { "added", { "y" } } };
    auto a = s.Add("reveng-fix", "RevEng", json::array({ r1, r2, r1, json{ { "error", "no key" } } }));
    EXPECT_EQ(a["added"], 2);
    EXPECT_EQ(a["skipped"], 2);
    // another application adds to the same domain, and the same lesson again is not duplicated
    auto b = s.Add("reveng-fix", "Other", r2);
    EXPECT_EQ(b["added"], 0);
    EXPECT_EQ(b["total"], 2);
    s.Add("reveng-fix", "Other", json{ { "key", "C2065" }, { "model", "m2" }, { "removed", { "q" } }, { "added", { "r" } } });

    auto q = s.Query("reveng-fix", { "C2065" }, "m2", 10);
    ASSERT_EQ(q["lessons"].size(), 2u);
    EXPECT_EQ(q["lessons"][0]["model"], "m2");  // the asking model's own lesson first
    EXPECT_EQ(q["lessons"][0]["app"], "Other");
    EXPECT_FALSE(q["lessons"][0]["time"].get<std::string>().empty());
    EXPECT_EQ(s.Query("reveng-fix", {}, "", 10)["lessons"].size(), 3u);
    EXPECT_EQ(s.Query("reveng-fix", {}, "", 1)["lessons"].size(), 1u);
    EXPECT_TRUE(s.Query("nothing", {}, "", 10)["lessons"].empty());

    auto st = s.Stats("");
    ASSERT_EQ(st["domains"].size(), 1u);
    EXPECT_EQ(st["domains"][0]["records"], 3);
    EXPECT_EQ(st["domains"][0]["keys"]["C2065"], 2);
}

TEST(Lessons, SeveralWritersDoNotLoseRecords)
{
    fs::path dir = testutil::TempDir("modelhub-lessons-") / L"lessons";
    std::vector<std::thread> t;
    for (int w = 0; w < 4; ++w)
        t.emplace_back([&, w]
            {
            LessonStore s(dir);  // each its own object, like separate processes
            for (int i = 0; i < 25; ++i) s.Add("d", "app" + std::to_string(w), json{{"key", "k"}, {"added", {std::to_string(w * 100 + i)}}}); });
    for (auto& x : t)
        x.join();
    EXPECT_EQ(LessonStore(dir).Query("d", {}, "", 1000)["lessons"].size(), 100u);
}

TEST(Lessons, ThePlaybookGrowsOnceAndIsSeededFromAFolder)
{
    fs::path root = testutil::TempDir("modelhub-lessons-");
    fs::create_directories(root / L"seed");
    WriteTextFile(root / L"seed" / L"stitch.md", "# stitch\nseeded text\n");
    LessonStore s(root / L"lessons");
    EXPECT_EQ(s.Seed(root / L"seed"), 1);
    EXPECT_EQ(s.Seed(root / L"seed"), 0);  // never overwrites what applications added
    EXPECT_NE(s.Playbook("stitch").find("seeded text"), std::string::npos);
    EXPECT_TRUE(s.PlaybookAdd("stitch", "RevEng", "DLLs", "Export data needs its own symbol.")["added"].get<bool>());
    EXPECT_FALSE(s.PlaybookAdd("stitch", "RevEng", "DLLs", "Export data needs its own symbol.")["added"].get<bool>());
    std::string p = s.Playbook("stitch");
    EXPECT_NE(p.find("## DLLs"), std::string::npos);
    EXPECT_NE(p.find("seeded text"), std::string::npos);
    EXPECT_FALSE(LessonStore::ValidDomain("../x"));
    EXPECT_TRUE(LessonStore::ValidDomain("reveng-fix"));
}

TEST(Lessons, TheHubCommandsServeEveryApplication)
{
    fs::path home = testutil::TempDir("modelhub-lessons-");
    testutil::FakeModel m{ "m", "m.gguf", "weights" };
    WriteTextFile(home / L"catalog.json", testutil::CatalogJson({ m }, "[]"));
    CoreOptions o;
    o.home = home;
    o.catalogFiles = { home / L"catalog.json" };
    o.scanOnInit = false;
    o.app = "AppOne";
    o.lessonSeedDir = home / L"none";
    HubCore hub(o);
    std::string err;
    ASSERT_TRUE(hub.Init(&err)) << err;
    EXPECT_EQ(hub.Call("lessons_add", { { "domain", "d" }, { "record", { { "key", "K" }, { "added", { "z" } } } } })["added"], 1);
    auto q = hub.Call("lessons_query", { { "domain", "d" }, { "keys", { "K" } } });
    ASSERT_EQ(q["lessons"].size(), 1u);
    EXPECT_EQ(q["lessons"][0]["app"], "AppOne");
    EXPECT_TRUE(hub.Call("playbook_add", { { "domain", "d" }, { "text", "note" } })["added"].get<bool>());
    EXPECT_NE(hub.Call("playbook", { { "domain", "d" } })["text"].get<std::string>().find("note"), std::string::npos);
    EXPECT_EQ(hub.Call("lessons_stats", json::object())["domains"].size(), 1u);
    EXPECT_EQ(hub.Call("lessons_add", { { "domain", "../bad" }, { "record", { { "key", "K" } } } })["status"], 400);
    // a second hub on the same home (another application) sees the same lessons
    HubCore other(o);
    ASSERT_TRUE(other.Init(&err));
    EXPECT_EQ(other.Call("lessons_query", { { "domain", "d" } })["lessons"].size(), 1u);
}
