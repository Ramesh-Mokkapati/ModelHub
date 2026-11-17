#include "Lessons.h"

#include <windows.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "Hash.h"
#include "Text.h"

namespace modelhub
{

namespace fs = std::filesystem;
using nlohmann::json;

namespace
{

class Lock
{
public:
    explicit Lock(const fs::path& dir)
    {
        std::wstring name = L"Local\\ModelHub-lessons-" + Widen(Sha256(ToLower(PathToUtf8(dir.lexically_normal()))).substr(0, 16));
        mutex_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (mutex_)
            WaitForSingleObject(mutex_, 30000);
    }
    ~Lock()
    {
        if (mutex_)
        {
            ReleaseMutex(mutex_);
            CloseHandle(mutex_);
        }
    }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

private:
    HANDLE mutex_ = nullptr;
};

std::vector<json> ReadRecords(const fs::path& file)
{
    std::vector<json> out;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty())
            continue;
        json j = json::parse(line, nullptr, false);
        if (j.is_object() && !j.value("key", std::string()).empty())
            out.push_back(std::move(j));
    }
    return out;
}

std::string Identity(const json& r)
{
    return r.value("key", "") + "\x1f" + r.value("model", "") + "\x1f" + r.value("removed", json::array()).dump() + "\x1f" +
           r.value("added", json::array()).dump();
}

std::string Now()
{
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

}  // namespace

bool LessonStore::ValidDomain(const std::string& d)
{
    if (d.empty() || d.size() > 64 || d[0] == '.')
        return false;
    return std::all_of(d.begin(), d.end(), [](unsigned char c)
        { return std::isalnum(c) || c == '_' || c == '-' || c == '.'; });
}

json LessonStore::Add(const std::string& domain, const std::string& app, const json& records)
{
    std::error_code ec;
    fs::create_directories(dir_, ec);
    const fs::path file = dir_ / Widen(domain + ".jsonl");
    Lock lock(dir_);
    std::vector<json> existing = ReadRecords(file);
    std::set<std::string> seen;
    for (const auto& r : existing)
        seen.insert(Identity(r));
    int added = 0, skipped = 0;
    std::ofstream out(file, std::ios::binary | std::ios::app);
    const json list = records.is_array() ? records : json::array({ records });
    for (json r : list)
    {
        if (!r.is_object() || r.value("key", std::string()).empty() || !seen.insert(Identity(r)).second)
        {
            ++skipped;
            continue;
        }
        if (!r.contains("app") && !app.empty())
            r["app"] = app;
        if (!r.contains("time"))
            r["time"] = Now();
        out << r.dump() << "\n";
        ++added;
    }
    out.flush();
    return { { "added", added }, { "skipped", skipped }, { "total", existing.size() + added } };
}

json LessonStore::Query(const std::string& domain, const std::vector<std::string>& keys, const std::string& model, size_t limit)
{
    std::vector<json> all = ReadRecords(dir_ / Widen(domain + ".jsonl"));
    std::set<std::string> want(keys.begin(), keys.end());
    std::vector<json> match;
    for (auto it = all.rbegin(); it != all.rend(); ++it)  // newest first
        if (want.empty() || want.count(it->value("key", "")))
            match.push_back(*it);
    if (!model.empty())
        std::stable_partition(match.begin(), match.end(), [&](const json& r)
            { return r.value("model", "") == model; });
    if (limit && match.size() > limit)
        match.resize(limit);
    return { { "domain", domain }, { "lessons", match } };
}

json LessonStore::Stats(const std::string& domain)
{
    std::set<std::string> domains;
    std::error_code ec;
    if (!domain.empty())
        domains.insert(domain);
    else
        for (const auto& e : fs::directory_iterator(dir_, ec))
            if (e.path().extension() == L".jsonl" || e.path().extension() == L".md")
                domains.insert(PathToUtf8(e.path().stem()));
    json list = json::array();
    for (const auto& d : domains)
    {
        std::map<std::string, int> keys;
        auto records = ReadRecords(dir_ / Widen(d + ".jsonl"));
        for (const auto& r : records)
            ++keys[r.value("key", "")];
        list.push_back({ { "domain", d }, { "records", records.size() }, { "keys", keys }, { "playbook", fs::exists(dir_ / Widen(d + ".md"), ec) } });
    }
    return { { "domains", list }, { "folder", PathToUtf8(dir_) } };
}

std::string LessonStore::Playbook(const std::string& domain)
{
    std::error_code ec;
    const fs::path file = dir_ / Widen(domain + ".md");
    if (!fs::exists(file, ec))
        return {};
    try
    {
        return ReadTextFile(file);
    }
    catch (...)
    {
        return {};
    }
}

json LessonStore::PlaybookAdd(const std::string& domain, const std::string& app, const std::string& section, const std::string& text)
{
    std::error_code ec;
    fs::create_directories(dir_, ec);
    const fs::path file = dir_ / Widen(domain + ".md");
    Lock lock(dir_);
    std::string current = Playbook(domain);
    const std::string body = text;
    if (current.find(body) != std::string::npos)
        return { { "added", false }, { "bytes", current.size() } };
    if (current.empty())
        current = "# " + domain + " lessons\n";
    if (!current.empty() && current.back() != '\n')
        current += "\n";
    current += "\n## " + (section.empty() ? std::string("Notes") : section) + "\n" + body + "\n" +
               (app.empty() ? "" : "\n_(" + app + ", " + Now().substr(0, 10) + ")_\n");
    WriteTextFile(file, current);
    return { { "added", true }, { "bytes", current.size() } };
}

int LessonStore::Seed(const fs::path& seedDir)
{
    std::error_code ec;
    int n = 0;
    if (!fs::is_directory(seedDir, ec))
        return 0;
    fs::create_directories(dir_, ec);
    Lock lock(dir_);
    for (const auto& e : fs::directory_iterator(seedDir, ec))
        if (e.path().extension() == L".md" || e.path().extension() == L".jsonl")
        {
            const fs::path to = dir_ / e.path().filename();
            if (!fs::exists(to, ec) && fs::copy_file(e.path(), to, ec))
                ++n;
        }
    return n;
}

}  // namespace modelhub
