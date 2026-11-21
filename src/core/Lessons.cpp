#include "Lessons.h"

#include <windows.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "Hash.h"
#include "LessonGeneral.h"
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
        {
            WaitForSingleObject(mutex_, 30000);
        }
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
        {
            continue;
        }

        json j = json::parse(line, nullptr, false);
        if (j.is_object() && !j.value("key", std::string()).empty())
        {
            out.push_back(std::move(j));
        }
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
    {
        return false;
    }

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
    {
        seen.insert(Identity(r));
    }

    int added = 0, skipped = 0;
    std::ofstream out(file, std::ios::binary | std::ios::app);
    const json list = records.is_array() ? records : json::array({ records });
    for (json r : list)
    {
        if (r.is_object())
        {
            r = GeneralLesson(r);  // what the fix did, not whose code it was
        }

        if (!r.is_object() || r.value("key", std::string()).empty() || !seen.insert(Identity(r)).second)
        {
            ++skipped;
            continue;
        }

        if (!r.contains("app") && !app.empty())
        {
            r["app"] = app;
        }

        if (!r.contains("time"))
        {
            r["time"] = Now();
        }

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
    {
        if (want.empty() || want.count(it->value("key", "")))
        {
            match.push_back(*it);
        }
    }

    if (!model.empty())
    {
        std::stable_partition(match.begin(), match.end(), [&](const json& r)
            { return r.value("model", "") == model; });
    }

    if (limit && match.size() > limit)
    {
        match.resize(limit);
    }

    return { { "domain", domain }, { "lessons", match } };
}

json LessonStore::Stats(const std::string& domain)
{
    std::set<std::string> domains;
    std::error_code ec;
    if (!domain.empty())
    {
        domains.insert(domain);
    }
    else
    {
        for (const auto& e : fs::directory_iterator(dir_, ec))
        {
            if (e.path().extension() == L".jsonl" || e.path().extension() == L".md")
            {
                domains.insert(PathToUtf8(e.path().stem()));
            }
        }
    }

    json list = json::array();
    for (const auto& d : domains)
    {
        std::map<std::string, int> keys;
        auto records = ReadRecords(dir_ / Widen(d + ".jsonl"));
        for (const auto& r : records)
        {
            ++keys[r.value("key", "")];
        }

        list.push_back({ { "domain", d }, { "records", records.size() }, { "keys", keys }, { "playbook", fs::exists(dir_ / Widen(d + ".md"), ec) } });
    }

    return { { "domains", list }, { "folder", PathToUtf8(dir_) } };
}

std::string LessonStore::Playbook(const std::string& domain)
{
    std::error_code ec;
    const fs::path file = dir_ / Widen(domain + ".md");
    if (!fs::exists(file, ec))
    {
        return {};
    }

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
    {
        return { { "added", false }, { "bytes", current.size() } };
    }

    if (current.empty())
    {
        current = "# " + domain + " lessons\n";
    }

    if (!current.empty() && current.back() != '\n')
    {
        current += "\n";
    }

    current += "\n## " + (section.empty() ? std::string("Notes") : section) + "\n" + body + "\n" +
               (app.empty() ? "" : "\n_(" + app + ", " + Now().substr(0, 10) + ")_\n");
    WriteTextFile(file, current);
    return { { "added", true }, { "bytes", current.size() } };
}

namespace
{

// A playbook's "## " sections, each with its heading line; what comes before the first one (the title) is left out.
std::vector<std::string> Sections(const std::string& text)
{
    std::vector<std::string> out;
    size_t at = text.rfind("## ", 0) == 0 ? 0 : text.find("\n## ");
    while (at != std::string::npos)
    {
        if (text[at] == '\n')
        {
            ++at;
        }

        size_t next = text.find("\n## ", at);
        out.push_back(text.substr(at, next == std::string::npos ? std::string::npos : next - at));
        at = next;
    }

    return out;
}

}  // namespace

int LessonStore::Seed(const fs::path& seedDir)
{
    std::error_code ec;
    int n = 0;
    if (!fs::is_directory(seedDir, ec))
    {
        return 0;
    }

    fs::create_directories(dir_, ec);
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(seedDir, ec))
    {
        if (e.path().extension() == L".md" || e.path().extension() == L".jsonl")
        {
            files.push_back(e.path());
        }
    }

    std::sort(files.begin(), files.end());
    for (const auto& from : files)
    {
        const fs::path to = dir_ / from.filename();
        {
            Lock lock(dir_);
            if (from.extension() == L".md" && !fs::exists(to, ec))
            {
                if (fs::copy_file(from, to, ec))
                {
                    ++n;
                }

                continue;
            }
        }

        if (from.extension() == L".jsonl")
        {
            // the records this install lacks (all of them for a domain it does not have yet); Add puts them in their general
            // form, skips the ones it has and keeps each record's own "app" and "time"
            json records = json::array();
            for (auto& r : ReadRecords(from))
            {
                records.push_back(std::move(r));
            }

            if (!records.empty())
            {
                n += Add(PathToUtf8(from.stem()), "", records).value("added", 0);
            }

            continue;
        }

        Lock lock(dir_);
        std::string current, shipped;
        try
        {
            current = ReadTextFile(to);
            shipped = ReadTextFile(from);
        }
        catch (...)
        {
            continue;
        }

        bool changed = false;
        for (const auto& section : Sections(shipped))
        {
            const std::string heading = section.substr(0, section.find('\n'));
            const std::string body = Trim(section);
            if (current.find("\n" + heading + "\n") != std::string::npos || current.rfind(heading + "\n", 0) == 0 ||
                current.find(body) != std::string::npos)
            {
                continue;
            }

            if (!current.empty() && current.back() != '\n')
            {
                current += "\n";
            }

            current += "\n" + body + "\n";
            changed = true;
            ++n;
        }

        if (changed)
        {
            WriteTextFile(to, current);
        }
    }

    return n;
}

json LessonStore::Clean(const std::string& domain, size_t maxLines, bool keepFailed)
{
    std::error_code ec;
    std::vector<std::string> domains;
    if (!domain.empty())
    {
        domains.push_back(domain);
    }
    else
    {
        for (const auto& e : fs::directory_iterator(dir_, ec))
        {
            if (e.path().extension() == L".jsonl")
            {
                domains.push_back(PathToUtf8(e.path().stem()));
            }
        }
    }

    json list = json::array();
    Lock lock(dir_);
    for (const auto& d : domains)
    {
        const fs::path file = dir_ / Widen(d + ".jsonl");
        if (!fs::exists(file, ec))
        {
            continue;
        }

        const std::vector<json> before = ReadRecords(file);
        std::set<std::string> seen;
        std::string text;
        size_t after = 0;
        for (const auto& old : before)
        {
            if (!keepFailed && old.value("outcome", std::string()) == "failed")
            {
                continue;
            }

            if (maxLines && LessonLines(old) > maxLines)
            {
                continue;
            }

            const json r = GeneralLesson(old);
            if (!seen.insert(Identity(r)).second)
            {
                continue;
            }

            text += r.dump() + "\n";
            ++after;
        }

        WriteTextFile(file, text);
        list.push_back({ { "domain", d }, { "before", before.size() }, { "after", after } });
    }

    return { { "domains", list } };
}

}  // namespace modelhub
