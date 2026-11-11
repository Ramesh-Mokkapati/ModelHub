#include <gtest/gtest.h>

#include "Catalog.h"
#include "Store.h"
#include "TestUtil.h"

using namespace modelhub;
namespace fs = std::filesystem;

namespace {
Catalog Parse(const std::string& json) {
    Catalog c;
    std::string err;
    EXPECT_TRUE(ParseCatalog(json, c, err)) << err;
    return c;
}
}  // namespace

TEST(Store, FindsVerifiesCachesAndRejectsFiles) {
    auto root = testutil::TempDir("modelhub-store-");
    fs::path hubModels = root / L"hub" / L"models", otherApp = root / L"otherapp";
    fs::create_directories(hubModels);
    fs::create_directories(otherApp);
    testutil::FakeModel a{"model-a", "a.gguf", "AAAA-weights"};
    testutil::FakeModel b{"model-b", "b.gguf", "BBBB-weights"};
    b.altFile = "b-other.gguf";
    b.altContent = "BBBB-from-elsewhere";
    testutil::FakeModel c{"model-c", "c.gguf", "CCCC-weights"};
    Catalog cat = Parse(testutil::CatalogJson({a, b, c}, "[]"));

    WriteTextFile(hubModels / L"a.gguf", a.content);                  // downloaded by the hub
    WriteTextFile(otherApp / L"b-other.gguf", b.altContent);          // another app's copy (an alternate file)
    WriteTextFile(otherApp / L"c.gguf", std::string(c.content.size(), 'x'));  // right size, wrong content

    ModelStore store(hubModels, root / L"hub" / L"manifest.json", {otherApp});
    std::vector<std::string> log;
    ScanResult r = store.Scan(cat, [&](const std::string& s) { log.push_back(s); });
    EXPECT_EQ(r.found, 2);
    EXPECT_EQ(r.verified, 3);
    EXPECT_EQ(r.rejected, 1);

    auto fa = store.Find(*cat.Find("model-a"));
    ASSERT_TRUE(fa);
    EXPECT_TRUE(fa->inHubFolder);
    auto fb = store.Find(*cat.Find("model-b"));
    ASSERT_TRUE(fb);
    EXPECT_FALSE(fb->inHubFolder);
    EXPECT_EQ(fb->path, otherApp / L"b-other.gguf");  // used in place
    EXPECT_FALSE(store.Find(*cat.Find("model-c")));

    store.AddUser(*cat.Find("model-b"), "RevEng Studio");
    // A second store over the same manifest: nothing is hashed again, users are kept.
    ModelStore again(hubModels, root / L"hub" / L"manifest.json", {otherApp});
    ScanResult r2 = again.Scan(cat, nullptr);
    EXPECT_EQ(r2.verified, 0);
    EXPECT_EQ(r2.found, 2);
    ASSERT_TRUE(again.Find(*cat.Find("model-b")));
    EXPECT_EQ(again.Find(*cat.Find("model-b"))->users, std::vector<std::string>{"RevEng Studio"});

    // A file that changes is verified again; a deleted one disappears.
    WriteTextFile(otherApp / L"c.gguf", c.content);
    fs::remove(hubModels / L"a.gguf");
    ScanResult r3 = again.Scan(cat, nullptr);
    EXPECT_EQ(r3.verified, 1);
    EXPECT_TRUE(again.Find(*cat.Find("model-c")));
    EXPECT_FALSE(again.Find(*cat.Find("model-a")));
    EXPECT_TRUE(fs::exists(otherApp / L"b-other.gguf"));  // never touched
    std::error_code ec;
    fs::remove_all(root, ec);
}
