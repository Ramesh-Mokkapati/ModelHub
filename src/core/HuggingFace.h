#pragma once
// GGUF files on Hugging Face: "owner/repo/file.gguf" specs and their exact size + SHA-256 from the API.
#include <optional>
#include <string>
#include <string_view>

#include "Catalog.h"

namespace modelhub {

struct HfFile {
    std::string repo;  // owner/name
    std::string file;  // in the repository root
    std::string Url() const { return "https://huggingface.co/" + repo + "/resolve/main/" + file; }
};

// "owner/repo/file.gguf", "hf:owner/repo/file.gguf" or a huggingface.co/.../(resolve|blob)/main/file.gguf URL.
std::optional<HfFile> ParseHfSpec(std::string_view spec);
// Size and SHA-256 from https://huggingface.co/api/models/<repo>/tree/main (public, non-gated repositories).
std::optional<CatalogFile> ResolveHfFile(const HfFile& f, std::string* error);

}  // namespace modelhub
