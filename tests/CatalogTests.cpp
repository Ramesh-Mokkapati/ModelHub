#include <gtest/gtest.h>

#include "Catalog.h"
#include "TestUtil.h"

using namespace modelhub;

TEST(Catalog, ShippedCatalogParsesAndCoversBothApplications) {
    Catalog c;
    std::string err;
    ASSERT_TRUE(ParseCatalog(ReadTextFile(std::filesystem::path(MODELHUB_SOURCE_DIR) / L"config" / L"catalog.json"), c, err))
        << err;
    EXPECT_GE(c.models.size(), 15u);
    for (const char* task : {"log-diagnosis", "code-conversion", "binary-renaming", "rename-review", "triage-planning",
                             "embeddings"})
        EXPECT_NE(c.FindTask(task), nullptr) << task;
    // RevEng's Ollama-style ids still resolve, to the same model AppAssist uses.
    const CatalogModel* m = c.Find("qwen2.5-coder:7b");
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->id, "qwen2.5-coder-7b");
    EXPECT_EQ(m->files.size(), 2u);  // AppAssist's file + RevEng's alternate
    EXPECT_EQ(c.Find("Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf")->id, "qwen3-coder-30b-a3b");  // by file name
    EXPECT_EQ(c.FindBySha(m->files[1].sha256), m);
    for (size_t i = 1; i < c.models.size(); ++i) EXPECT_LE(c.models[i - 1].SizeBytes(), c.models[i].SizeBytes());
}

TEST(Catalog, RejectsBadEntries) {
    using testutil::CatalogJson;
    Catalog c;
    std::string err;
    std::string tasks = R"([{"id":"t","tiers":[{"prefer":["a"]}]}])";
    EXPECT_TRUE(ParseCatalog(CatalogJson({{"a", "a.gguf", "x"}}, tasks), c, err)) << err;

    std::string noHash = CatalogJson({{"a", "a.gguf", "x"}}, tasks);
    noHash.replace(noHash.find("\"sha256\":\"") + 10, 4, "zzzz");
    EXPECT_FALSE(ParseCatalog(noHash, c, err));
    EXPECT_NE(err.find("sha256"), std::string::npos);

    EXPECT_FALSE(ParseCatalog(CatalogJson({{"a", "a.gguf", "x"}}, R"([{"id":"t","tiers":[{"prefer":["b"]}]}])"), c, err));
    EXPECT_NE(err.find("unknown model"), std::string::npos);

    EXPECT_FALSE(ParseCatalog(CatalogJson({{"a", "..\\a.gguf", "x"}}, tasks), c, err));  // no paths in file names
    EXPECT_FALSE(ParseCatalog(CatalogJson({{"a", "a.gguf", "x"}}, tasks, "http://example.com/"), c, err));
    EXPECT_TRUE(ParseCatalog(CatalogJson({{"a", "a.gguf", "x"}}, tasks, "http://127.0.0.1:9/"), c, err));

    testutil::FakeModel noTools{"a", "a.gguf", "x"};
    noTools.tools = false;
    EXPECT_FALSE(ParseCatalog(CatalogJson({noTools}, R"([{"id":"t","needs_tools":true,"tiers":[{"prefer":["a"]}]}])"), c, err));
    EXPECT_NE(err.find("tool calling"), std::string::npos);

    EXPECT_FALSE(ParseCatalog(CatalogJson({{"a", "a.gguf", "x", "b"}, {"b", "b.gguf", "y"}}, tasks), c, err));
    EXPECT_NE(err.find("used twice"), std::string::npos);
    EXPECT_FALSE(ParseCatalog("{\"format\":\"appassist-models\"}", c, err));
}
