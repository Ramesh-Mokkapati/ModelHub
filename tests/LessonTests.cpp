// Lessons shared by every application that uses the hub: records per key, a playbook per domain, seeding.
#include <gtest/gtest.h>

#include <thread>

#include "HubCore.h"
#include "LessonGeneral.h"
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
    {
        t.emplace_back([&, w]
            {
            LessonStore s(dir);  // each its own object, like separate processes
            for (int i = 0; i < 25; ++i) s.Add("d", "app" + std::to_string(w), json{{"key", "k"}, {"added", {std::to_string(w * 100 + i)}}}); });
    }

    for (auto& x : t)
    {
        x.join();
    }

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

TEST(Lessons, AnExistingInstallGetsTheShippedLessonsItLacksAndKeepsItsOwn)
{
    fs::path root = testutil::TempDir("modelhub-lessons-");
    fs::create_directories(root / L"seed");
    LessonStore s(root / L"lessons");

    // this install already learned one fix and wrote its own playbook section
    json own = { { "key", "C2664" }, { "model", "m1" }, { "removed", "f(s.c_str());" }, { "added", "f(CA2T(s.c_str()));" } };
    s.Add("port-fix", "PortAssistAI", own);
    s.PlaybookAdd("port-fix", "PortAssistAI", "C2664 - Compiler Error C2664", "Wrap narrow text in CA2T.");

    // a later version ships that fix, a new one, and the playbook with one known and one new section
    json shipped = own;
    shipped["app"] = "PortAssistAI";
    shipped["time"] = "2026-11-01T00:00:00Z";
    json fresh = { { "key", "C4477" }, { "model", "m2" }, { "removed", "printf(\"%d\", n);" }, { "added", "printf(\"%zu\", n);" },
        { "app", "PortAssistAI" }, { "time", "2026-11-02T00:00:00Z" } };
    WriteTextFile(root / L"seed" / L"port-fix.jsonl", shipped.dump() + "\n" + fresh.dump() + "\n");
    WriteTextFile(root / L"seed" / L"port-fix.md",
        "# port-fix lessons\n\n## C2664 - Compiler Error C2664\nWrap narrow text in CA2T.\n\n## C4477 - Compiler Warning C4477\nUse %zu for size_t.\n");
    WriteTextFile(root / L"seed" / L"refactor-fix.jsonl", fresh.dump() + "\n");

    EXPECT_EQ(s.Seed(root / L"seed"), 3);  // the new record, the new section, and the domain this install did not have
    EXPECT_EQ(s.Seed(root / L"seed"), 0);  // again: nothing more

    auto all = s.Query("port-fix", {}, "", 10)["lessons"];
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0]["key"], "C4477");
    EXPECT_EQ(all[0]["time"], "2026-11-02T00:00:00Z");  // a shipped record keeps its own time and application
    EXPECT_EQ(all[1]["added"], "name1(CA2T(name2.c_str()));");  // stored in its general form
    const std::string book = s.Playbook("port-fix");
    EXPECT_NE(book.find("## C4477 - Compiler Warning C4477\nUse %zu for size_t."), std::string::npos);
    EXPECT_EQ(book.find("## C2664"), book.rfind("## C2664"));  // the section it had is not added twice
    EXPECT_EQ(s.Query("refactor-fix", {}, "", 10)["lessons"].size(), 1u);
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

TEST(Lessons, ALessonKeepsWhatTheFixDidAndNotWhoseCodeItWas)
{
    json r = { { "key", "C2664" },
        { "error", "'void CheckFailure(HRESULT,const CString &)': cannot convert argument 2 from 'const char [9]' to 'const CString &'" },
        { "removed", "    CheckFailure(m_pImpl->QueryInterface(IID_PPV_ARGS(&m_pEnv)), \"Failed!\"); // see Zone_0001\n    printf(\"%d\", nLen);" },
        { "added", "    CheckFailure(m_pImpl->QueryInterface(IID_PPV_ARGS(&m_pEnv)), _T(\"Failed!\"));\n    printf(\"%zu\", nLen);" } };
    const json g = GeneralLesson(r);

    // the program's own names (kept on both sides) and texts become placeholders; comments go
    EXPECT_EQ(g["removed"], "name1(name2->QueryInterface(IID_PPV_ARGS(&name3)), \"text1\");\nprintf(\"%d\", name4);");  // from column 0

    // what the fix is about stays: _T, the format strings, and the names of the libraries
    EXPECT_EQ(g["added"], "name1(name2->QueryInterface(IID_PPV_ARGS(&name3)), _T(\"text1\"));\nprintf(\"%zu\", name4);");
    EXPECT_EQ(g["error"], "'void name1(HRESULT,const CString &)': cannot convert argument 2 from 'const char [9]' to 'const CString &'");
    EXPECT_TRUE(g["general"].get<bool>());
    EXPECT_EQ(GeneralLesson(g), g);  // once is enough

    // a name on one side only is what the fix is about: it stays
    json api = { { "key", "C2664" }, { "removed", "CharUpperBuffA(joined, 5);" }, { "added", "CharUpperBuff(joined, 5);" } };
    EXPECT_EQ(GeneralLesson(api)["added"], "CharUpperBuff(name1, 5);");
    json type = { { "key", "C4244" }, { "removed", "DWORD address = (DWORD_PTR)buffer;" }, { "added", "DWORD_PTR address = (DWORD_PTR)buffer;" } };
    EXPECT_EQ(GeneralLesson(type)["added"], "DWORD_PTR name1 = (DWORD_PTR)name2;");

    // lists of lines (RevEng) the same way, one numbering for the whole record
    json lines = { { "key", "C2065" }, { "removed", { "int x = LAB_1400073d8;", "Use(iVar1);" } }, { "added", { "int x = 0;", "Use(iVar1);" } } };
    const json gl = GeneralLesson(lines);
    EXPECT_EQ(gl["removed"], json({ "int name1 = LAB_1400073d8;", "name2(name3);" }));
    EXPECT_EQ(gl["added"], json({ "int name1 = 0;", "name2(name3);" }));
    EXPECT_EQ(LessonLines(gl), 2u);
}

TEST(Lessons, CleaningKeepsShortKeptFixesInTheirGeneralFormOnce)
{
    fs::path dir = testutil::TempDir("modelhub-lessons-") / L"lessons";
    fs::create_directories(dir);
    json a = { { "key", "C2664" }, { "removed", "Use(sUser.c_str());" }, { "added", "Use(CA2T(sUser.c_str()));" }, { "model", "m" } };
    json b = { { "key", "C2664" }, { "removed", "Use(sName.c_str());" }, { "added", "Use(CA2T(sName.c_str()));" }, { "model", "m" } };  // the same fix
    json failed = a;
    failed["outcome"] = "failed";
    failed["removed"] = "x();";
    json longer = { { "key", "C2440" }, { "removed", "a;\nb;\nc;\nd;" }, { "added", "a;" } };
    WriteTextFile(dir / L"port-fix.jsonl", a.dump() + "\n" + b.dump() + "\n" + failed.dump() + "\n" + longer.dump() + "\n");
    LessonStore s(dir);
    const json r = s.Clean("", 3, false);
    ASSERT_EQ(r["domains"].size(), 1u);
    EXPECT_EQ(r["domains"][0]["before"], 4);
    EXPECT_EQ(r["domains"][0]["after"], 1);
    const json left = s.Query("port-fix", {}, "", 10)["lessons"];
    ASSERT_EQ(left.size(), 1u);
    EXPECT_EQ(left[0]["added"], "name1(CA2T(name2.c_str()));");
    EXPECT_EQ(s.Clean("", 3, false)["domains"][0]["after"], 1);  // again: the same
}
