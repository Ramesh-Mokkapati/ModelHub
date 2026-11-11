// Browsing Hugging Face: the rules (best use, quantisations, model cards, which file suits this PC) and the
// hf_search / hf_repo / hf_add commands against a local stand-in for huggingface.co.
#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "Hash.h"
#include "HfBrowse.h"
#include "Mirror.h"
#include "TestUtil.h"
#include "Text.h"
#include "modelhub/modelhub.hpp"

using namespace modelhub;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

HardwareInfo Gpu8GB() {
    HardwareInfo hw;
    hw.gpuName = "Test GPU";
    hw.vramMB = 8 * 1024;
    hw.ramMB = 32 * 1024;
    hw.cores = 16;
    hw.avx2 = true;
    return hw;
}
HardwareInfo CpuOnly16GB() {
    HardwareInfo hw;
    hw.ramMB = 16 * 1024;
    hw.cores = 8;
    hw.avx2 = true;
    return hw;
}

json TreeEntry(const std::string& path, uint64_t size, const std::string& sha = std::string(64, 'a')) {
    return {{"type", "file"}, {"path", path}, {"size", size}, {"lfs", {{"oid", sha}, {"size", size}}}};
}

// A 7B coder repository like bartowski's: many quantisations, ARM-only ones, a split F32, an mmproj.
json Tree7B() {
    const uint64_t G = 1ull << 30;
    return json::array({TreeEntry("Coder-7B-Instruct-Q2_K.gguf", 3 * G), TreeEntry("Coder-7B-Instruct-Q3_K_M.gguf", 3 * G + G / 2),
                        TreeEntry("Coder-7B-Instruct-Q4_K_M.gguf", 4 * G + G / 2), TreeEntry("Coder-7B-Instruct-Q4_0_4_8.gguf", 4 * G + G / 3),
                        TreeEntry("Coder-7B-Instruct-Q5_K_M.gguf", 5 * G + G / 3), TreeEntry("Coder-7B-Instruct-Q8_0.gguf", 8 * G),
                        TreeEntry("Coder-7B-Instruct-f32-00001-of-00002.gguf", 15 * G),
                        TreeEntry("Coder-7B-Instruct-f32-00002-of-00002.gguf", 15 * G), TreeEntry("mmproj-f16.gguf", G / 2),
                        TreeEntry("README.md", 4000, ""), {{"type", "directory"}, {"path", "old"}}});
}
json Info7B() {
    return {{"id", "someone/Coder-7B-Instruct-GGUF"},
            {"pipeline_tag", "text-generation"},
            {"tags", {"gguf", "code"}},
            {"gated", false},
            {"downloads", 1234},
            {"gguf", {{"total", 7.6e9}, {"architecture", "qwen2"}, {"context_length", 32768}, {"chat_template", "{% if tools %}"}}},
            {"cardData", {{"license", "apache-2.0"}, {"base_model", {"Org/Coder-7B-Instruct"}}}}};
}

const json* File(const json& repo, const std::string& name) {
    for (const auto& f : repo["files"])
        if (f["file"] == name) return &f;
    return nullptr;
}

}  // namespace

TEST(HfBrowse, ParsesQuantisations) {
    EXPECT_EQ(ParseQuant("Qwen2.5-7B-Instruct-Q4_K_M.gguf").name, "Q4_K_M");
    EXPECT_EQ(ParseQuant("model-IQ3_XS.gguf").name, "IQ3_XS");
    EXPECT_EQ(ParseQuant("model.Q8_0.gguf").rank, 7);
    EXPECT_EQ(ParseQuant("model-bf16.gguf").name, "BF16");
    EXPECT_TRUE(ParseQuant("model-Q4_0_4_8.gguf").armOnly);
    EXPECT_FALSE(ParseQuant("model-Q4_0.gguf").armOnly);
    EXPECT_EQ(ParseQuant("model-Q6_K_L.gguf").name, "Q6_K_L");
    EXPECT_GT(ParseQuant("x-Q4_K_M.gguf").rank, ParseQuant("x-Q4_K_S.gguf").rank);
    EXPECT_EQ(ParseQuant("plain.gguf").name, "");
}

TEST(HfBrowse, ClassifiesWhatAModelIsFor) {
    EXPECT_EQ(ClassifyUse("Qwen/Qwen2.5-Coder-7B-Instruct-GGUF", "text-generation", {}, 7.6, true).category, "Coding");
    EXPECT_NE(ClassifyUse("Qwen/Qwen2.5-Coder-7B-Instruct-GGUF", "text-generation", {}, 7.6, true).bestFor.find("tool calling"),
              std::string::npos);
    EXPECT_EQ(ClassifyUse("nomic-ai/nomic-embed-text-v1.5-GGUF", "", {}, 0.1, false).category, "Embeddings");
    EXPECT_EQ(ClassifyUse("x/bge-small-en-v1.5-GGUF", "feature-extraction", {}, 0.03, false).category, "Embeddings");
    EXPECT_EQ(ClassifyUse("unsloth/DeepSeek-R1-Distill-Qwen-7B-GGUF", "text-generation", {}, 7, false).category, "Reasoning");
    EXPECT_EQ(ClassifyUse("x/Qwen2.5-VL-7B-Instruct-GGUF", "image-text-to-text", {}, 7, false).category, "Vision + text");
    EXPECT_EQ(ClassifyUse("x/Llama-3.2-3B-Instruct-GGUF", "text-generation", {}, 3.2, false).category, "Chat and writing");
    EXPECT_EQ(ClassifyUse("x/Mistral-7B-base-GGUF", "text-generation", {}, 7, false).category, "Base model");
    EXPECT_EQ(ClassifyUse("x/some-model-GGUF", "", {"code"}, 1, false).category, "Coding");
    EXPECT_EQ(ClassifyUse("x/VieNeu-TTS-v3-Turbo", "text-to-speech", {}, 0.1, false).category, kNotForLlama);
    EXPECT_EQ(ClassifyUse("x/parakeet-unified-en-0.6b-gguf", "", {}, 0.6, false).category, kNotForLlama);
    EXPECT_NE(ClassifyUse("x/Llama-3.3-70B-Instruct-GGUF", "", {}, 70, false).bestFor.find("strongest"), std::string::npos);
}

TEST(HfBrowse, ReadsParametersFromNames) {
    double active = 0;
    EXPECT_DOUBLE_EQ(ParamsFromName("Qwen3-Coder-30B-A3B-Instruct-GGUF", &active), 30);
    EXPECT_DOUBLE_EQ(active, 3);
    EXPECT_DOUBLE_EQ(ParamsFromName("Qwen2.5-Coder-1.5B-Instruct-GGUF", &active), 1.5);
    EXPECT_DOUBLE_EQ(active, 1.5);
    EXPECT_DOUBLE_EQ(ParamsFromName("gemma-3-12b-it-GGUF", nullptr), 12);
    EXPECT_EQ(ParamsFromName("nomic-embed-text-v1.5-GGUF", nullptr), 0);
}

TEST(HfBrowse, TakesTheModelCardsDescriptionNotTheQuantisationNotes) {
    std::string quantCard = "---\nlicense: mit\n---\n## Llamacpp imatrix Quantizations of X\n\nUsing llama.cpp release b3772 for "
                            "quantization of this model, with imatrix data.\n\nOriginal model: https://huggingface.co/X\n";
    EXPECT_EQ(CardParagraph(quantCard), "");
    std::string card = "---\nlicense: mit\n---\n# X\n\n![logo](x.png)\n\n<p align=center>badges</p>\n\n## Introduction\n\n"
                       "**X-Coder** is the latest series of [code-specific](https://x) large language models, with "
                       "significant improvements in code generation, code reasoning and code fixing.\n\n```\ncode\n```\n";
    EXPECT_EQ(CardParagraph(card), "X-Coder is the latest series of code-specific large language models, with significant "
                                   "improvements in code generation, code reasoning and code fixing.");
    std::string listIntro = "# X\n\nX is a family of models for code, in six sizes. It improves on the previous "
                            "generation in these ways:\n\n- one\n- two\n";
    EXPECT_EQ(CardParagraph(listIntro), "X is a family of models for code, in six sizes.");
    std::string longer(2000, 'a');
    longer = "A model. " + std::string(300, 'b') + ". " + std::string(800, 'c');
    EXPECT_LE(CardParagraph(longer, 600).size(), 603u);
}

TEST(HfBrowse, JudgesEveryFileForThisPcAndRecommendsOne) {
    json r = EvaluateRepo("https://huggingface.co", "someone/Coder-7B-Instruct-GGUF", Info7B(), Tree7B(), "", "", Gpu8GB(), {});
    EXPECT_EQ(r["category"], "Coding");
    EXPECT_EQ(r["architecture"], "qwen2");
    EXPECT_EQ(r["context_k"], 32);
    EXPECT_TRUE(r["tools"].get<bool>());
    EXPECT_EQ(r["files"].size(), 7u);  // mmproj, README, folders and split parts after the first are left out
    EXPECT_EQ(File(r, "mmproj-f16.gguf"), nullptr);
    EXPECT_EQ((*File(r, "Coder-7B-Instruct-Q4_0_4_8.gguf"))["reason"], "For ARM processors; this PC is x64");
    EXPECT_EQ((*File(r, "Coder-7B-Instruct-f32-00001-of-00002.gguf"))["reason"], "Split into parts: not supported yet");
    EXPECT_EQ((*File(r, "Coder-7B-Instruct-f32-00001-of-00002.gguf"))["size"], 30ull << 30);  // both parts
    EXPECT_EQ((*File(r, "Coder-7B-Instruct-Q8_0.gguf"))["fit"], "gpu+cpu");
    EXPECT_EQ((*File(r, "Coder-7B-Instruct-Q4_K_M.gguf"))["fit"], "gpu");
    // 8 GB of graphics memory: the best quality that still fits on the card.
    EXPECT_EQ(r["recommended"], "Coder-7B-Instruct-Q4_K_M.gguf");
    EXPECT_EQ((*File(r, "Coder-7B-Instruct-Q4_K_M.gguf"))["url"],
              "https://huggingface.co/someone/Coder-7B-Instruct-GGUF/resolve/main/Coder-7B-Instruct-Q4_K_M.gguf");

    HardwareInfo big = Gpu8GB();
    big.vramMB = 24 * 1024;
    EXPECT_EQ(EvaluateRepo("b", "someone/r", Info7B(), Tree7B(), "", "", big, {})["recommended"], "Coder-7B-Instruct-Q8_0.gguf");
    EXPECT_EQ(EvaluateRepo("b", "someone/r", Info7B(), Tree7B(), "", "", CpuOnly16GB(), {})["recommended"],
              "Coder-7B-Instruct-Q4_K_M.gguf");  // on the CPU, Q4_K_M: bigger files are slower
    HardwareInfo tiny = CpuOnly16GB();
    tiny.ramMB = 4 * 1024;
    json none = EvaluateRepo("b", "someone/r", Info7B(), Tree7B(), "", "", tiny, {});
    EXPECT_EQ(none["recommended"], "");
    EXPECT_EQ(none["compatible_files"], 0);

    json gatedInfo = Info7B();
    gatedInfo["gated"] = "manual";
    json g = EvaluateRepo("b", "someone/r", gatedInfo, Tree7B(), "", "", Gpu8GB(), {});
    EXPECT_EQ(g["compatible_files"], 0);
    EXPECT_NE(g["notes"][0].get<std::string>().find("Gated"), std::string::npos);
}

TEST(HfBrowse, MakesACustomModelEntryForACompatibleFile) {
    json r = EvaluateRepo("https://huggingface.co", "someone/Coder-7B-Instruct-GGUF", Info7B(), Tree7B(), "", "", Gpu8GB(), {});
    std::string err;
    auto e = CustomModelEntry(r, "Coder-7B-Instruct-Q4_K_M.gguf", &err);
    ASSERT_TRUE(e) << err;
    EXPECT_EQ((*e)["id"], "coder-7b-instruct-q4_k_m");
    EXPECT_EQ((*e)["name"], "Coder-7B-Instruct (Q4_K_M)");
    EXPECT_NEAR((*e)["params_b"].get<double>(), 7.6, 0.01);
    EXPECT_GT((*e)["kv_cache_mb_8k"].get<uint64_t>(), 0u);
    EXPECT_TRUE((*e)["tools"].get<bool>());
    EXPECT_FALSE(CustomModelEntry(r, "Coder-7B-Instruct-Q4_0_4_8.gguf", &err));
    EXPECT_NE(err.find("ARM"), std::string::npos);
    EXPECT_FALSE(CustomModelEntry(r, "missing.gguf", &err));
}

// hf_search, hf_repo and hf_add through the DLL, against a local stand-in for huggingface.co; the added model then
// downloads and verifies like any other.
TEST(HfBrowse, SearchesJudgesAddsAndDownloadsThroughTheSdk) {
    fs::path root = testutil::TempDir("modelhub-hf-");
    fs::path home = root / L"home";
    fs::create_directories(home);
    testutil::Mirror hf;
    const std::string gguf(300000, 'g');
    const std::string repo = "someone/Tiny-0.5B-Instruct-GGUF";
    json info = {{"id", repo},
                 {"pipeline_tag", "text-generation"},
                 {"gated", false},
                 {"downloads", 99},
                 {"likes", 5},
                 {"gguf", {{"total", 0.5e9}, {"architecture", "qwen2"}, {"context_length", 32768}}},
                 {"cardData", {{"license", "apache-2.0"}, {"base_model", {"someone/Tiny-0.5B-Instruct"}}}}};
    hf.files["api/models"] = json::array({info}).dump();
    hf.files["api/models/" + repo] = info.dump();
    hf.files["api/models/" + repo + "/tree/main"] =
        json::array({TreeEntry("Tiny-0.5B-Instruct-Q4_K_M.gguf", gguf.size(), Sha256(gguf))}).dump();
    hf.files[repo + "/raw/main/README.md"] = "# Quantizations of Tiny\n\nGGUF quants made with llama.cpp for this model, nothing else.\n";
    hf.files["someone/Tiny-0.5B-Instruct/raw/main/README.md"] =
        "# Tiny\n\nTiny is a small instruction-tuned model for quick answers and short summaries on any computer.\n";
    hf.files[repo + "/resolve/main/Tiny-0.5B-Instruct-Q4_K_M.gguf"] = gguf;
    WriteTextFile(home / L"config.json", R"({"search_dirs": []})");
    testutil::FakeModel other{"other-model", "other.gguf", std::string(1000, 'o')};
    WriteTextFile(home / L"catalog.json", testutil::CatalogJson({other}, "[]", "https://example.invalid/"));
    {
        Sdk sdk;
        std::string err;
        const std::string base = "http://127.0.0.1:" + std::to_string(hf.port);
        ASSERT_TRUE(sdk.Open(json({{"app", "HfTest"}, {"home", PathToUtf8(home)}, {"hf_base", base}}).dump(), nullptr, &err)) << err;

        json s = json::parse(sdk.Call("hf_search", R"({"query":"tiny","kind":"text"})"));
        ASSERT_EQ(s["results"].size(), 1u) << s.dump();
        EXPECT_EQ(s["results"][0]["repo"], repo);
        EXPECT_EQ(s["results"][0]["category"], "Chat and writing");
        EXPECT_TRUE(s["results"][0].contains("fit"));  // how a Q4_K_M file would run here (depends on the machine)

        json r = json::parse(sdk.Call("hf_repo", json({{"repo", repo}}).dump()));
        ASSERT_FALSE(r.contains("error")) << r.dump();
        EXPECT_EQ(r["description"], "Tiny is a small instruction-tuned model for quick answers and short summaries on any computer.");
        EXPECT_EQ(r["recommended"], "Tiny-0.5B-Instruct-Q4_K_M.gguf");
        EXPECT_EQ(r["files"][0]["url"], base + "/" + repo + "/resolve/main/Tiny-0.5B-Instruct-Q4_K_M.gguf");
        EXPECT_EQ(json::parse(sdk.Call("hf_repo", R"({"repo":"no slash"})"))["status"], 400);
        EXPECT_EQ(json::parse(sdk.Call("hf_repo", R"({"repo":"someone/missing"})"))["status"], 404);

        json added = json::parse(sdk.Call("hf_add", json({{"repo", repo}, {"file", "Tiny-0.5B-Instruct-Q4_K_M.gguf"}}).dump()));
        ASSERT_TRUE(added.value("added", false)) << added.dump();
        const std::string id = added["model"]["id"];
        EXPECT_EQ(id, "tiny-0.5b-instruct-q4_k_m");
        json d = json::parse(sdk.Download(id));
        EXPECT_TRUE(d.value("installed", false)) << d.dump();
        EXPECT_EQ(ReadTextFile(Widen(d["path"].get<std::string>())), gguf);

        // The repository now shows the file as installed (even from the cache); adding it again changes nothing.
        r = json::parse(sdk.Call("hf_repo", json({{"repo", repo}}).dump()));
        EXPECT_TRUE(r["files"][0].value("installed", false)) << r.dump();
        EXPECT_EQ(r["files"][0]["catalog_id"], id);
        json again = json::parse(sdk.Call("hf_add", json({{"repo", repo}, {"file", "Tiny-0.5B-Instruct-Q4_K_M.gguf"}}).dump()));
        EXPECT_FALSE(again.value("added", true)) << again.dump();
    }
    std::error_code ec;
    fs::remove_all(root, ec);
}
