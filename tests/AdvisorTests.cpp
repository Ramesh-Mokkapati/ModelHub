#include <gtest/gtest.h>

#include "Advisor.h"
#include "Catalog.h"
#include "TestUtil.h"

using namespace modelhub;

namespace {

const Catalog& Shipped() {
    static Catalog c = [] {
        Catalog cat;
        std::string err;
        ParseCatalog(ReadTextFile(std::filesystem::path(MODELHUB_SOURCE_DIR) / L"config" / L"catalog.json"), cat, err);
        return cat;
    }();
    return c;
}

HardwareInfo Pc(uint64_t ramGB, uint64_t vramGB) {
    HardwareInfo hw;
    hw.ramMB = ramGB * 1024;
    hw.availRamMB = hw.ramMB / 2;
    hw.vramMB = vramGB * 1024;
    hw.gpuName = vramGB ? "Test GPU" : "";
    hw.cores = 16;
    hw.avx2 = true;
    return hw;
}

auto None = [](const CatalogModel&) { return false; };

}  // namespace

TEST(Advisor, StartsSmallForLogDiagnosisWithoutWorkload) {
    const auto& c = Shipped();
    Advice a = Advise(c, *c.FindTask("log-diagnosis"), Pc(16, 0), Engine::EmbeddedCpu, nullptr, "", 0, None);
    ASSERT_NE(a.recommended, nullptr);
    EXPECT_EQ(a.recommended->id, "qwen2.5-coder-1.5b");
    EXPECT_EQ(a.recommended, a.firstModel);
    EXPECT_FALSE(a.notes.empty());
}

TEST(Advisor, LargeWorkloadAsksForTheStrongestModelThatFits) {
    const auto& c = Shipped();
    Workload big;
    big.metrics = {{"lines", 600000}, {"symbols", 40000}};
    big.summary = "600000 lines";
    Advice roomy = Advise(c, *c.FindTask("log-diagnosis"), Pc(64, 0), Engine::EmbeddedCpu, &big, "qwen2.5-coder-1.5b", 0, None);
    EXPECT_EQ(roomy.recommended->id, "qwen3-coder-30b-a3b");
    EXPECT_EQ(roomy.tierIndex, 3);
    EXPECT_TRUE(roomy.upgradeSuggested);

    Advice small = Advise(c, *c.FindTask("log-diagnosis"), Pc(16, 0), Engine::EmbeddedCpu, &big, "", 0, None);
    EXPECT_NE(small.recommended->id, "qwen3-coder-30b-a3b");  // 18.6 GB of weights do not fit 16 GB
    EXPECT_EQ(small.ideal->id, "qwen3-coder-30b-a3b");
    EXPECT_EQ(FitFor(*small.recommended, Pc(16, 0), 8192, 1, Engine::EmbeddedCpu), Fit::Cpu);
}

TEST(Advisor, RenamingPicksTheLargestModelOnTheGpu) {
    const auto& c = Shipped();
    const TaskProfile& t = *c.FindTask("binary-renaming");
    for (uint64_t vram : {8, 12, 16, 24}) {
        HardwareInfo hw = Pc(32, vram);
        Advice a = Advise(c, t, hw, Engine::Hub, nullptr, "", 0, None);
        ASSERT_NE(a.recommended, nullptr) << vram;
        const CatalogModel* expected = nullptr;
        for (const auto& id : t.tiers[0].prefer)
            if (!expected && FitFor(*c.Find(id), hw, t.contextTokens, t.parallel, Engine::Hub) == Fit::Gpu) expected = c.Find(id);
        ASSERT_NE(expected, nullptr) << vram;
        EXPECT_EQ(a.recommended->id, expected->id) << vram << " GB";
        EXPECT_EQ(GpuLayersFor(*a.recommended, hw, t.contextTokens, t.parallel), 999);
    }
    // The same task on a CPU-only engine ignores the GPU.
    Advice cpu = Advise(c, t, Pc(32, 24), Engine::EmbeddedCpu, nullptr, "", 0, None);
    EXPECT_EQ(FitFor(*cpu.recommended, Pc(32, 24), t.contextTokens, t.parallel, Engine::EmbeddedCpu), Fit::Cpu);
}

TEST(Advisor, PartialOffloadAndDiskAndInstalledState) {
    const auto& c = Shipped();
    const CatalogModel& big = *c.Find("qwen2.5-coder-32b");
    HardwareInfo hw = Pc(64, 8);
    EXPECT_EQ(FitFor(big, hw, 16384, 1, Engine::Hub), Fit::PartialOffload);
    int layers = GpuLayersFor(big, hw, 16384, 1);
    EXPECT_GT(layers, 0);
    EXPECT_LT(layers, big.layers);

    const TaskProfile& t = *c.FindTask("code-conversion");
    Workload w;
    w.metrics = {{"largest_file_lines", 100}, {"lines", 1000}};
    w.summary = "1000 lines";
    auto installed7b = [](const CatalogModel& m) { return m.id == "qwen2.5-coder-7b"; };
    Advice a = Advise(c, t, Pc(32, 0), Engine::EmbeddedCpu, &w, "", 0, installed7b);
    EXPECT_EQ(a.recommended->id, "qwen2.5-coder-7b");
    EXPECT_TRUE(a.recommendedInstalled);

    // Too little disk for anything but what is installed.
    Advice disk = Advise(c, *c.FindTask("log-diagnosis"), Pc(64, 0), Engine::EmbeddedCpu, &w, "", 2ull << 30, installed7b);
    for (const auto& o : disk.options) EXPECT_EQ(o.fitsDisk, o.installed || o.model->SizeBytes() + (1ull << 30) <= (2ull << 30));
}

TEST(Advisor, NothingFits) {
    const auto& c = Shipped();
    Advice a = Advise(c, *c.FindTask("code-conversion"), Pc(4, 0), Engine::EmbeddedCpu, nullptr, "", 0, None);
    EXPECT_EQ(a.recommended, nullptr);
    ASSERT_FALSE(a.notes.empty());
    EXPECT_NE(a.notes[0].find("too large"), std::string::npos);
    HardwareInfo noAvx = Pc(64, 0);
    noAvx.avx2 = false;
    EXPECT_EQ(Advise(c, *c.FindTask("log-diagnosis"), noAvx, Engine::EmbeddedCpu, nullptr, "", 0, None).recommended, nullptr);
}
