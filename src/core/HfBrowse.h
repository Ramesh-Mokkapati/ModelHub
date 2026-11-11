#pragma once
// Browsing GGUF models on Hugging Face: search, what a model is best used for, which of its files suit this PC.
//
// The JSON shapes are those of the hf_search and hf_repo commands (HubCore). The parsing and judging functions are
// separate from the network calls, so they can be tested with recorded answers.
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Catalog.h"
#include "Hardware.h"

namespace modelhub {

// What a model is for, from its name, pipeline tag, tags and size.
inline constexpr const char* kNotForLlama = "Speech, audio or images";

struct UseInfo {
    std::string category;  // "Coding", "Chat and writing", "Reasoning", "Embeddings", "Vision + text", "Math", "Base model"
    std::string bestFor;   // one or two sentences
};
UseInfo ClassifyUse(const std::string& repo, const std::string& pipelineTag, const std::vector<std::string>& tags,
                    double paramsB, bool tools);

// The quantisation in a GGUF file name ("Q4_K_M", "IQ3_XS", "Q8_0", "BF16", ...).
struct QuantInfo {
    std::string name;      // "" if the name has none
    std::string quality;   // for people
    double rank = 0;       // higher = closer to the original model (F16 8, Q8_0 7, Q6 6, Q5 5, Q4_K_M 4.5, Q4 4, Q3 3, Q2 2)
    bool armOnly = false;  // Q4_0_4_4 / Q4_0_4_8 / Q4_0_8_8: repacked for ARM processors
};
QuantInfo ParseQuant(const std::string& fileName);

// The first paragraph of a model card (README.md) that describes the model, as plain text ("" if none).
// Paragraphs about quantising or running GGUF files are skipped: they describe the packaging, not the model.
std::string CardParagraph(const std::string& markdown, size_t maxChars = 600);

// Parameters (billions) and the active ones of a mixture-of-experts model ("30B-A3B"), from the API's gguf.total
// or else the repository name; 0 if unknown.
double ParamsFromName(const std::string& name, double* activeB);

// A catalog model for judging how a file runs here: layers and KV cache are estimated from the parameters.
CatalogModel EstimatedModel(const std::string& id, uint64_t sizeBytes, double paramsB, double activeB, int contextK);

std::string HfSearchUrl(const std::string& base, const std::string& query, const std::string& kind,
                        const std::string& sort, int limit);
std::string HfModelUrl(const std::string& base, const std::string& repo);  // model info with gguf + cardData

// One search result: {repo, downloads, likes, updated, pipeline, architecture, params_b, active_params_b, context_k,
// gated, license, category, best_for, tools, q4_size, fit, speed} (fit and speed for a Q4_K_M file, estimated).
nlohmann::json SummarizeSearchItem(const nlohmann::json& item, const HardwareInfo& hw);

// Whether a file with this SHA-256 is installed / which catalog model has it.
struct KnownFile {
    std::string catalogId;
    bool installed = false;
};
using KnownFileFn = std::function<KnownFile(const std::string& sha256)>;

// hf_repo: the model (from the model info JSON), its files (from the tree JSON) judged for this PC, the recommended
// file, the best use and a description (from the model cards: the repository's, or its base model's).
nlohmann::json EvaluateRepo(const std::string& base, const std::string& repo, const nlohmann::json& info,
                            const nlohmann::json& tree, const std::string& card, const std::string& baseCard,
                            const HardwareInfo& hw, const KnownFileFn& known);

// The add_model entry for one evaluated file of an evaluated repository (EvaluateRepo's output).
std::optional<nlohmann::json> CustomModelEntry(const nlohmann::json& repo, const std::string& file, std::string* error);

}  // namespace modelhub
