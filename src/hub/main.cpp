// ModelHub.exe - the hub service and its command line.
//
//   ModelHub serve [--background]     run the hub (background: no console window; clients start it this way)
//   ModelHub start | stop             start the hub in the background / stop it
//   ModelHub status                   hardware, store, running llama-servers
//   ModelHub list                     installed models (and where their files are)
//   ModelHub catalog                  all models and tasks
//   ModelHub advise <task> [--engine hub|embedded-cpu] [--metric name=value ...] [--current <model>]
//   ModelHub pull <model>             download with progress (resumable; verified by SHA-256)
//   ModelHub path <model>             the installed file of a model
//   ModelHub delete <model>           delete every copy of a model (to the Recycle Bin)
//   ModelHub add-folder <dir>         also use the models another application keeps in <dir>
//   ModelHub rescan
#include <windows.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "HubServer.h"
#include "Paths.h"
#include "Text.h"
#include "modelhub/modelhub_client.h"

namespace {

using nlohmann::json;
namespace mc = modelhub::client;

int Usage() {
    std::fputs(
        "ModelHub - shared local models for llama.cpp\n\n"
        "  ModelHub serve [--background]\n"
        "  ModelHub start | stop | status | list | catalog | rescan\n"
        "  ModelHub advise <task> [--engine hub|embedded-cpu] [--metric name=value ...] [--current <model>]\n"
        "  ModelHub pull <model>\n"
        "  ModelHub path <model>\n"
        "  ModelHub delete <model>\n"
        "  ModelHub add-folder <dir>\n",
        stderr);
    return 2;
}

int Serve(bool background) {
    if (background) FreeConsole();
    modelhub::HubOptions opt;
    opt.home = modelhub::HubHome();
    opt.config = modelhub::LoadHubConfig(modelhub::ConfigPath());
    // Next to the program (build output), or one folder up (the SDK and release layout: bin/catalog.json).
    opt.catalogFiles = {opt.home / L"catalog.json", modelhub::ExeDir() / L"catalog.json", modelhub::ExeDir().parent_path() / L"catalog.json"};
    modelhub::Hub hub(opt);
    std::string err;
    if (!hub.Start(&err)) {
        std::fprintf(stderr, "ModelHub: %s\n", err.c_str());
        return 1;
    }
    static modelhub::Hub* running = &hub;
    SetConsoleCtrlHandler(
        [](DWORD) -> BOOL {
            running->Stop();
            return TRUE;
        },
        TRUE);
    hub.Wait();
    return 0;
}

int Print(const mc::Response& r) {
    if (r.status == 0) {
        std::fprintf(stderr, "ModelHub: %s\n", r.error.c_str());
        return 1;
    }
    json j = json::parse(r.body, nullptr, false);
    std::cout << (j.is_discarded() ? r.body : j.dump(2)) << "\n";
    return r.Ok() ? 0 : 1;
}

bool Connect(mc::Client& c, bool start) {
    std::string err;
    if (c.Connect(start, 30000, &err)) return true;
    std::fprintf(stderr, "ModelHub: %s\n", err.c_str());
    return false;
}

// Waits until the hub has checked the model files on disk, so installed states are final.
void WaitForScan(mc::Client& c) {
    bool told = false;
    for (int i = 0; i < 1200; ++i) {
        json s = json::parse(c.Status().body, nullptr, false);
        if (!s.is_object() || !s.value("scanning", false)) break;
        if (!told) std::fprintf(stderr, "waiting for ModelHub to check the model files (%s)...\n", s.value("scan", "").c_str());
        told = true;
        Sleep(500);
    }
}

int Pull(mc::Client& c, const std::string& model) {
    mc::Response r = c.StartDownload(model);
    if (!r.Ok() && r.status != 409) return Print(r);
    if (r.body.find("\"installed\"") != std::string::npos) {
        std::cout << model << " is already installed\n";
        return 0;
    }
    json started = json::parse(r.body, nullptr, false);
    std::string id = started.is_object() ? started.value("model", model) : model;
    for (;;) {
        Sleep(1000);
        json d = json::parse(c.Downloads().body, nullptr, false);
        if (!d.is_object()) return 1;
        for (const auto& s : d.value("downloads", json::array())) {
            if (s.value("model", "") != id) continue;
            double done = s.value("done", 0.0), total = s.value("total", 0.0), bps = s.value("bytes_per_second", 0.0);
            std::fprintf(stderr, "\r%-12s %6.1f%%  %8.1f / %.1f MB  %6.1f MB/s   ", s.value("phase", "").c_str(),
                         total > 0 ? 100.0 * done / total : 0.0, done / 1048576.0, total / 1048576.0, bps / 1048576.0);
            if (!s.value("running", false)) {
                std::fputs("\n", stderr);
                if (s.value("phase", "") == "done") {
                    std::cout << id << " downloaded and verified\n";
                    return 0;
                }
                std::fprintf(stderr, "%s: %s\n", id.c_str(), s.value("error", "").c_str());
                return 1;
            }
        }
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> a;
    for (int i = 1; i < argc; ++i) a.push_back(modelhub::Narrow(argv[i]));
    if (a.empty()) return Usage();
    const std::string& cmd = a[0];
    if (cmd == "serve") return Serve(a.size() > 1 && a[1] == "--background");

    mc::Client c("ModelHub CLI");
    if (cmd == "start") {
        if (!Connect(c, true)) return 1;
        std::cout << "ModelHub is running at http://127.0.0.1:" << c.Port() << "\n";
        return 0;
    }
    if (cmd == "add-folder" && a.size() == 2) {
        auto cfgPath = modelhub::ConfigPath();
        modelhub::HubConfig cfg = modelhub::LoadHubConfig(cfgPath);
        auto dir = std::filesystem::absolute(modelhub::Utf8ToPath(a[1]));
        cfg.searchDirs.push_back(dir);
        modelhub::SaveHubConfig(cfgPath, cfg);
        std::cout << "added " << modelhub::PathToUtf8(dir) << " to " << modelhub::PathToUtf8(cfgPath)
                  << "; restart the hub (ModelHub stop, ModelHub start) to use it\n";
        return 0;
    }
    if (!Connect(c, cmd != "stop")) return cmd == "stop" ? 0 : 1;
    if (cmd == "stop") return Print(c.Post("/hub/v1/shutdown", "{}"));
    if (cmd == "status") return Print(c.Status());
    if (cmd == "list") {
        WaitForScan(c);
        return Print(c.Installed());
    }
    if (cmd == "catalog") return Print(c.Catalog());
    if (cmd == "rescan") return Print(c.Rescan());
    if (cmd == "path" && a.size() == 2) {
        std::string err;
        std::wstring p = c.ModelPath(a[1], &err);
        if (p.empty()) {
            std::fprintf(stderr, "%s: %s\n", a[1].c_str(), err.c_str());
            return 1;
        }
        std::cout << modelhub::Narrow(p) << "\n";
        return 0;
    }
    if (cmd == "pull" && a.size() == 2) return Pull(c, a[1]);
    if (cmd == "delete" && a.size() == 2) {
        WaitForScan(c);
        return Print(c.DeleteModel(a[1]));
    }
    if (cmd == "advise" && a.size() >= 2) {
        std::string engine = "hub", current;
        json metrics = json::object();
        for (size_t i = 2; i + 1 < a.size(); i += 2) {
            if (a[i] == "--engine") engine = a[i + 1];
            else if (a[i] == "--current") current = a[i + 1];
            else if (a[i] == "--metric" && a[i + 1].find('=') != std::string::npos)
                metrics[a[i + 1].substr(0, a[i + 1].find('='))] = std::atof(a[i + 1].substr(a[i + 1].find('=') + 1).c_str());
            else return Usage();
        }
        WaitForScan(c);
        mc::Response r = c.Advise(a[1], engine, metrics.empty() ? "" : metrics.dump(), "", current);
        json j = json::parse(r.body, nullptr, false);
        if (r.Ok() && j.is_object()) {
            std::cout << "Recommended: " << j.value("recommended", "") << "\n";
            for (const auto& n : j.value("notes", json::array())) std::cout << "  " << n.get<std::string>() << "\n";
            std::cout << "\n  model                      fit        needs      installed\n";
            for (const auto& o : j.value("options", json::array())) {
                char line[160];
                std::snprintf(line, sizeof line, "  %-26s %-10s %6.1f GB  %s\n", o.value("id", "").c_str(),
                              o.value("fit", "").c_str(), o.value("needs_mb", 0.0) / 1024.0,
                              o.value("installed", false) ? "yes" : "");
                std::cout << line;
            }
            return 0;
        }
        return Print(r);
    }
    return Usage();
}
