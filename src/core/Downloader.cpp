#include "Downloader.h"

#include <windows.h>

#include <chrono>
#include <fstream>

#include "Hash.h"
#include "HttpClient.h"
#include "Paths.h"
#include "Text.h"

namespace modelhub {

namespace fs = std::filesystem;

bool DownloadFile(const CatalogFile& f, const fs::path& dir, const std::function<void(const DownloadProgress&)>& onProgress,
                  std::stop_token stop, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path target = dir / Widen(f.file);
    fs::path part = target;
    part += L".part";

    // One download of a file at a time on this PC: another application (or the hub) may be fetching it.
    HANDLE lock = CreateMutexW(nullptr, FALSE, (L"Local\\ModelHub-download-" + Widen(f.sha256.substr(0, 16))).c_str());
    struct Release {
        HANDLE h;
        bool owned = false;
        ~Release() {
            if (h && owned) ReleaseMutex(h);
            if (h) CloseHandle(h);
        }
    } release{lock};
    if (lock) {
        for (bool told = false;;) {
            DWORD w = WaitForSingleObject(lock, 500);
            if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) {
                release.owned = true;
                break;
            }
            if (stop.stop_requested()) return fail("cancelled");
            if (!told && onProgress) onProgress({"waiting", 0, f.size, 0});  // another process is downloading it
            told = true;
        }
    }
    if (fs::is_regular_file(target, ec) && fs::file_size(target, ec) == f.size && Sha256File(target) == f.sha256)
        return true;  // already there (perhaps just finished by another process)

    uint64_t have = fs::exists(part, ec) ? fs::file_size(part, ec) : 0;
    if (have > f.size) {
        fs::remove(part, ec);
        have = 0;
    }
    if (uint64_t freeBytes = FreeDiskBytes(dir); freeBytes && freeBytes + have < f.size + (512ull << 20))
        return fail("not enough disk space in " + PathToUtf8(dir) + ": " + std::to_string((f.size - have) >> 20) +
                    " MB needed, " + std::to_string(freeBytes >> 20) + " MB free");

    auto started = std::chrono::steady_clock::now();
    uint64_t startedAt = have;
    auto report = [&](const std::string& phase, uint64_t done) {
        if (!onProgress) return;
        DownloadProgress p{phase, done, f.size, 0};
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        if (secs > 0.5 && done > startedAt) p.bytesPerSecond = (done - startedAt) / secs;
        onProgress(p);
    };

    for (int attempt = 0; attempt < 6; ++attempt) {
        if (stop.stop_requested()) return fail("cancelled (the partial download resumes next time)");
        have = fs::exists(part, ec) ? fs::file_size(part, ec) : 0;
        if (have == f.size) break;
        HttpHeaders headers;
        if (have) headers.emplace_back("Range", "bytes=" + std::to_string(have) + "-");
        std::ofstream out;
        uint64_t done = have, lastReport = 0;
        bool rangeDone = false;
        HttpResponse r = HttpRequestStream(
            "GET", f.url, {}, headers, 60000,
            [&](std::string_view chunk) {
                out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                if (!out) return false;
                done += chunk.size();
                if (done - lastReport >= (4u << 20)) {
                    lastReport = done;
                    report("downloading", done);
                }
                return !stop.stop_requested();
            },
            stop,
            [&](const HttpResponse& h) {
                if (h.status == 416 && have) {  // nothing left to fetch
                    rangeDone = true;
                    return false;
                }
                if (h.status == 206) {
                    out.open(part, std::ios::binary | std::ios::app);
                } else if (h.status == 200) {
                    done = 0;  // the server ignored the range: start over
                    out.open(part, std::ios::binary | std::ios::trunc);
                } else {
                    return false;
                }
                return static_cast<bool>(out);
            });
        out.close();
        if (rangeDone) break;
        if (r.cancelled && stop.stop_requested()) return fail("cancelled (the partial download resumes next time)");
        if (r.status == 401 || r.status == 403)
            return fail("the server refused the download (HTTP " + std::to_string(r.status) +
                        "): the repository may be gated or private");
        if (r.status == 404) return fail("file not found: " + f.url);
        if (r.error.empty() && (r.status == 200 || r.status == 206) && fs::file_size(part, ec) == f.size) break;
        for (int i = 0; i < 4 * (attempt + 1) && !stop.stop_requested(); ++i)  // back off, then resume
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    have = fs::exists(part, ec) ? fs::file_size(part, ec) : 0;
    if (have != f.size)
        return fail("download incomplete (" + std::to_string(have) + " of " + std::to_string(f.size) +
                    " bytes); it resumes next time");
    report("verifying", have);
    std::string actual = Sha256File(part);
    if (actual != f.sha256) {
        fs::remove(part, ec);
        return fail("SHA-256 mismatch: expected " + f.sha256 + ", got " + actual + " (file removed)");
    }
    fs::rename(part, target, ec);
    if (ec) return fail("cannot move the finished download into place: " + ec.message());
    report("done", have);
    return true;
}

DownloadManager::~DownloadManager() {
    std::map<std::string, std::jthread> threads;
    {
        std::lock_guard lk(mu_);
        threads.swap(threads_);
    }
    for (auto& [_, t] : threads) t.request_stop();
    // jthreads join here
}

bool DownloadManager::Start(const CatalogModel& m, std::string* error) {
    std::lock_guard lk(mu_);
    if (auto it = status_.find(m.id); it != status_.end() && it->second.running) {
        if (error) *error = "already downloading";
        return false;
    }
    if (auto t = threads_.find(m.id); t != threads_.end()) {
        t->second.join();  // a finished thread: reap it
        threads_.erase(t);
    }
    status_[m.id] = Status{m.id, {"starting", 0, m.SizeBytes(), 0}, {}, true};
    CatalogModel model = m;
    threads_.emplace(m.id, std::jthread([this, model](std::stop_token stop) {
        std::string err;
        const CatalogFile& f = model.Primary();
        bool ok = DownloadFile(
            f, dir_,
            [&](const DownloadProgress& p) {
                std::lock_guard lk2(mu_);
                status_[model.id].progress = p;
            },
            stop, &err);
        if (ok && onDone_) onDone_(model, f, dir_ / Widen(f.file));
        std::lock_guard lk2(mu_);
        Status& s = status_[model.id];
        s.running = false;
        s.error = err;
        s.progress.phase = ok ? "done" : stop.stop_requested() ? "cancelled" : "failed";
        if (ok) s.progress.done = s.progress.total;
    }));
    return true;
}

bool DownloadManager::Cancel(const std::string& modelId) {
    std::lock_guard lk(mu_);
    auto t = threads_.find(modelId);
    if (t == threads_.end()) return false;
    t->second.request_stop();
    return true;
}

std::vector<DownloadManager::Status> DownloadManager::List() const {
    std::lock_guard lk(mu_);
    std::vector<Status> v;
    for (const auto& [_, s] : status_) v.push_back(s);
    return v;
}

}  // namespace modelhub
