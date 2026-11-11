// llama.cpp server arguments, GPU layers and device choice (moved here from RevEng Studio).
#include <gtest/gtest.h>

#include <algorithm>

#include "Advisor.h"
#include "HuggingFace.h"
#include "ServerPool.h"

using namespace modelhub;

namespace {
CatalogModel Model(double paramsB, double sizeGB, int layers) {
    CatalogModel m;
    m.id = "m";
    m.paramsB = m.activeParamsB = paramsB;
    m.layers = layers;
    m.kvCacheMB8k = static_cast<uint64_t>(paramsB * 64);
    m.files.push_back({"m.gguf", "https://x/m.gguf", static_cast<uint64_t>(sizeGB * (1ull << 30)), std::string(64, 'a')});
    return m;
}
HardwareInfo Hw(double vramGB, double ramGB) {
    HardwareInfo h;
    h.vramMB = static_cast<uint64_t>(vramGB * 1024);
    h.ramMB = static_cast<uint64_t>(ramGB * 1024);
    h.gpuName = vramGB > 0 ? "GPU" : "";
    h.cores = 16;
    h.avx2 = true;
    return h;
}
}  // namespace

TEST(Llama, GpuLayers) {
    auto m7 = Model(7, 4.4, 28);
    EXPECT_EQ(GpuLayersFor(m7, Hw(24, 64), 16384, 2), 999);  // fits: everything on the GPU
    int partial = GpuLayersFor(m7, Hw(5.8, 64), 16384, 4);
    EXPECT_GT(partial, 0);
    EXPECT_LT(partial, 28);
    EXPECT_EQ(GpuLayersFor(m7, Hw(0, 64), 16384, 1), 0);  // CPU only
}

TEST(Llama, ServerArgs) {
    auto a = ServerPool::BuildArgs(L"C:\\m\\x.gguf", 8090, 4, 16384, 20, "qwen2.5-coder-7b", false, "");
    auto has = [&](const std::wstring& flag, const std::wstring& value) {
        for (size_t i = 0; i + 1 < a.size(); ++i)
            if (a[i] == flag && a[i + 1] == value) return true;
        return false;
    };
    EXPECT_TRUE(has(L"--port", L"8090"));
    EXPECT_TRUE(has(L"--parallel", L"4"));
    EXPECT_TRUE(has(L"--ctx-size", L"65536"));  // 16K per slot x 4 slots
    EXPECT_TRUE(has(L"--n-gpu-layers", L"20"));
    EXPECT_TRUE(has(L"--host", L"127.0.0.1"));
    EXPECT_NE(std::find(a.begin(), a.end(), L"--jinja"), a.end());
    auto e = ServerPool::BuildArgs(L"e.gguf", 1, 1, 8192, 0, "emb", true, "");
    EXPECT_NE(std::find(e.begin(), e.end(), L"--embeddings"), e.end());
}

TEST(Llama, ChoosesTheDiscreteGpuNotTheIntegratedOne) {
    // Real `llama-server --list-devices` output from a laptop with an Intel iGPU and an NVIDIA dGPU.
    std::vector<std::string> out = {
        "Available devices:",
        "  Vulkan0: Intel(R) RaptorLake-S Mobile Graphics Controller (32610 MiB, 32610 MiB free)",
        "  Vulkan1: NVIDIA RTX 1000 Ada Generation Laptop GPU (5920 MiB, 5152 MiB free)"};
    EXPECT_EQ(ServerPool::ChooseDevice(out, "NVIDIA RTX 1000 Ada Generation Laptop GPU"), "Vulkan1");
    EXPECT_EQ(ServerPool::ChooseDevice(out, "AMD Radeon RX 7900"), "");
    EXPECT_EQ(ServerPool::ChooseDevice(out, ""), "");
    EXPECT_EQ(ServerPool::ChooseDevice(out, "Intel(R) UHD (integrated: treated as CPU)"), "");
    auto a = ServerPool::BuildArgs(L"x.gguf", 1, 1, 8192, 10, "m", false, "Vulkan1");
    EXPECT_EQ(a.at(a.size() - 2), L"--device");
    EXPECT_EQ(a.back(), L"Vulkan1");
}

TEST(HuggingFace, ParseSpecAcceptsRepoPathsAndUrls) {
    auto a = ParseHfSpec("bartowski/Qwen2.5-Coder-7B-Instruct-GGUF/Qwen2.5-Coder-7B-Instruct-Q4_K_M.gguf");
    ASSERT_TRUE(a);
    EXPECT_EQ(a->repo, "bartowski/Qwen2.5-Coder-7B-Instruct-GGUF");
    EXPECT_EQ(a->file, "Qwen2.5-Coder-7B-Instruct-Q4_K_M.gguf");
    auto b = ParseHfSpec("https://huggingface.co/owner/repo/resolve/main/model.Q8_0.gguf?download=true");
    ASSERT_TRUE(b);
    EXPECT_EQ(b->repo, "owner/repo");
    EXPECT_EQ(b->file, "model.Q8_0.gguf");
    EXPECT_EQ(b->Url(), "https://huggingface.co/owner/repo/resolve/main/model.Q8_0.gguf");
    EXPECT_TRUE(ParseHfSpec("hf.co/owner/repo/blob/main/x.gguf"));
    EXPECT_FALSE(ParseHfSpec("qwen2.5-coder:7b"));            // an Ollama tag
    EXPECT_FALSE(ParseHfSpec("owner/repo/sub/dir/x.gguf"));   // sub-folders
    EXPECT_FALSE(ParseHfSpec("owner/repo/readme.md"));
}
