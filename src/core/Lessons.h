#pragma once
// Lessons: what applications learned while fixing things, shared by every application that uses ModelHub.
// Two kinds, both per "domain" (a topic such as "reveng-fix" or "reveng-stitch"):
//   - records: one JSON object per learned fix, appended to <home>\lessons\<domain>.jsonl. A record has a "key" (what
//     it applies to, e.g. an MSVC error code) and anything else the application wants back (error, removed, added,
//     model, ...). The hub adds "app" and "time".
//   - a playbook: <home>\lessons\<domain>.md, the human-readable how-to (symptoms, fixes, traps) that sessions and
//     people read before starting and extend with each new finding.
// Several processes append at once: every write holds a named mutex.
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace modelhub
{

class LessonStore
{
public:
    explicit LessonStore(std::filesystem::path dir)
        : dir_(std::move(dir))
    {
    }

    static bool ValidDomain(const std::string& domain);  // 1-64 of [A-Za-z0-9_.-]

    // Appends records (objects with a non-empty "key"); identical ones (same key, model, removed, added) are skipped.
    // Result: {"added", "skipped", "total"}.
    nlohmann::json Add(const std::string& domain, const std::string& app, const nlohmann::json& records);
    // Records whose key is one of `keys` (all when empty), newest first, at most `limit`; `model` (when given) first.
    nlohmann::json Query(const std::string& domain, const std::vector<std::string>& keys, const std::string& model, size_t limit);
    // {"domains":[{"domain","records","keys":{key:count},"playbook":bool}]} for one domain or all.
    nlohmann::json Stats(const std::string& domain);

    std::string Playbook(const std::string& domain);  // "" when there is none
    // Appends "## <section>\n<text>" unless the text is already in the playbook. Result: {"added":bool,"bytes"}.
    nlohmann::json PlaybookAdd(const std::string& domain, const std::string& app, const std::string& section, const std::string& text);
    // Copies playbooks (*.md) from `seedDir` that have no copy here yet. Returns how many.
    int Seed(const std::filesystem::path& seedDir);

    const std::filesystem::path& Dir() const
    {
        return dir_;
    }

private:
    std::filesystem::path dir_;
};

}  // namespace modelhub
