# ModelHub

Everything local-LLM applications on a Windows PC share about models and llama.cpp, in one place.

- **One store.** Models live in `%LOCALAPPDATA%\ModelHub\models`. Folders applications already use are
  searched too, and files found there are used in place. Each model is downloaded once per machine.
- **One catalog.** Every model is pinned by exact size and SHA-256. Task profiles say which models suit which
  job. Applications can add custom models (for example a Hugging Face GGUF) for everyone.
- **Advice.** Which model fits this PC's graphics memory, RAM and disk, and the size of the job.
- **Downloads.** They resume after interruptions, are verified before use, and never run twice for the same
  file on one PC.
- **llama.cpp.** The pinned, verified `llama-server` build, and servers started on demand (GPU layers chosen
  from the free graphics memory).

It comes in four parts, all built from one core (`src/core`, `HubCore`):

| Part | What it is for |
| --- | --- |
| `ModelHubManager.exe` | For people: an icon in the notification area to start, stop and restart the service, a window that suggests models for this PC and for each task, and downloads and deletes them for every application; and a Hugging Face browser that finds any GGUF model, says what it is best for, and downloads the file that suits this PC. |
| `ModelHubSdk.dll` | In the application's own process: model selection, store, downloads, custom models, llama.cpp servers. A plain C interface (`include/modelhub/modelhub.h`, JSON in and out) with a header-only C++14 wrapper (`modelhub.hpp`). |
| `ModelHub.exe` | The service: the same core over HTTP on `127.0.0.1`, plus a command line (see below). |
| llama.cpp | `llama-server` b11938, downloaded and verified at configure time. |

### What the service (`ModelHub.exe`) does

`ModelHub.exe serve` runs in the background with no window. Applications reach it at `http://127.0.0.1:39281`
with the token in `%LOCALAPPDATA%\ModelHub\hub.json`. It is optional: applications that use the DLL work
without it.

- **It shares running models.** It starts a `llama-server` for a model when an application first asks for it,
  lets every application use that one server, and stops it after 10 idle minutes. Two applications that both
  use Qwen2.5-Coder 7B load it into graphics memory once, not twice.
- **It speaks HTTP.** It has an OpenAI-compatible `/v1` (chat, completions, embeddings, model list) and
  `/hub/v1/...` for status, the catalog, advice, downloads and deleting. Programs in any language can use it
  without the DLL.
- **Starting it.** Applications start it when they need it (`modelhub_client.h`'s `Connect`). You can start,
  stop and restart it yourself from `ModelHubManager.exe`'s tray icon, or with `ModelHub start` / `ModelHub stop`.

## ModelHub Manager

`ModelHubManager.exe` (next to `ModelHub.exe`) puts the ModelHub icon in the notification area. The dot shows
the service: green while it runs, grey when stopped, amber while it starts, stops or a download runs.

- **Tray menu:**
  - *Open ModelHub Manager*. Clicking the icon does the same.
  - *Start service*, *Stop service*, *Restart service*. Stopping or restarting asks first if models are loaded,
    because applications using them get errors until the service runs again.
  - *Start with Windows*: starts the manager in the tray at sign-in, and the service with it.
  - *Exit*. Running downloads stop and resume later.
- **The window:**
  - What this PC has (graphics card and memory, RAM, cores) and the service's state.
  - **Suggest models for**:
    - *All models*: every catalog model, how it would run here (graphics card, graphics card + CPU, CPU, or
      too large), its speed, and the tasks it is suggested for.
    - A task (log diagnosis, code conversion, naming decompiled functions, ...): only that task's models, in
      the order ModelHub ranks them, with *Recommended*, *Start with this* and *Best for large work*, and the
      reasons.
  - For each model: size, status (installed, partly downloaded, downloading with speed and time left) and the
    applications that use it.
  - **Download** (or double-click) downloads into the shared store, with progress. It warns first if the model
    is too large for this PC or the disk is short of space.
  - **Delete...** lists every copy of the model, the applications that use it, and copies in applications'
    own folders, then asks. The files go to the Recycle Bin. If the service is running, it deletes through the
    service, which first stops its `llama-server` of that model. A file that another application has open is
    reported, not deleted.
  - **Browse Hugging Face...** opens the model browser (below).
  - **Add folder...** shares a folder of GGUF files with every application.
- **Browse Hugging Face** finds any GGUF model on Hugging Face and downloads the file that suits this PC:
  - **Search** by name, or paste `owner/repo`, `owner/repo/file.gguf` or a huggingface.co address. Filter by kind
    (chat and writing, coding, embeddings) and sort by downloads, trend, likes or date. *Only models that run on
    this PC* hides gated models, models too large for this PC (judged on a typical Q4_K_M file), and speech,
    audio and image models, which llama-server does not run.
  - **Each model shows what it is best for:** chat and writing, coding, reasoning, maths, embeddings, vision +
    text or base model, with a sentence about its size and whether it can call tools. The description comes from
    the original model's card, not the GGUF repackager's notes. You also see its parameters, architecture,
    context length, licence and downloads.
  - **Each of its GGUF files** shows its quantisation, size, quality, whether it runs on the graphics card, the
    graphics card and CPU, or the CPU, and how fast.
    - **Not for this PC:** files for ARM processors (`Q4_0_4_4/4_8/8_8`), split files, files without a SHA-256,
      and gated repositories are marked as such.
    - **★ Best for this PC:** the best quality that runs fully on the graphics card (up to Q8_0), or Q4_K_M on a PC
      without one.
  - **Download** adds the file as a custom model for every application and downloads it like any other (verified
    by SHA-256, resumable); the main window shows the progress. Models are judged with an 8K context; layers and
    KV cache are estimated from the parameters.
- **How it runs:** it does its model work in its own process, with the same core as the DLL, so it works
  whether or not the service runs. Everything it changes (the store, custom models, folders) is shared with
  every application. Only one copy runs; starting it again shows the window. Pass `--tray` to start without the
  window, and `--start-service` to start the service.

RevEng Studio and AppAssist use the DLL and have no model code of their own any more (see
*Applications using ModelHub* below).

## Build

Needs Visual Studio 2022 or 2026 with C++ and its bundled vcpkg (`VCPKG_ROOT`).

```bash
cmake --preset vs2026-x64
```

```bash
cmake --build --preset release
```

- Configuring downloads llama.cpp `b11938` (Vulkan build by default).
- Other backends: `-DMODELHUB_LLAMA_BACKEND=cuda` or `cpu`.
- To use your own llama.cpp, pass `-DMODELHUB_BUNDLE_LLAMA=OFF` and set `llama_server` in `config.json`.
- Archives are cached in `.downloads\` (`-DMODELHUB_DOWNLOAD_CACHE=...`).
- Every build stages the **SDK** in `build\sdk`: what applications build against.

  | Path | Contents |
  | --- | --- |
  | `include\modelhub\` | `modelhub.h` (C interface), `modelhub.hpp` (C++14 wrapper), `modelhub_client.h` (service client) |
  | `lib\<Config>\ModelHubSdk.lib` | import library |
  | `bin\<Config>\` | `ModelHubSdk.dll`, `ModelHub.exe`, `ModelHubManager.exe` |
  | `bin\catalog.json` | the catalog |
  | `bin\llama\` | llama.cpp's server |
  | `ModelHubConfig.cmake` | for `find_package(ModelHub CONFIG PATHS <ModelHub>/build/sdk)` |

Running the tests:

```bash
build\bin\Release\ModelHubTests.exe
```

They cover:

- the DLL through its C interface and the C++14 wrapper: two "applications" sharing one store; downloads that
  resume, are cancelled or wait for another process; custom models; deleting every copy of a model;
- the HTTP service, including deleting over HTTP;
- the Hugging Face browser's rules (best use, quantisations, model cards, which file suits a PC) and its
  search, judge, add and download steps against a local stand-in for huggingface.co, so they need no network.

The public headers are also compiled as C++14. `HubIntegration.ChatThroughTheHub` runs a real model end to
end: set `MODELHUB_IT_MODEL` to a `.gguf` file from the catalog to enable it. `ModelHubManager.exe` (the windows
and the tray icon) has no automated tests.

## Versions and releases

`.github/workflows/build.yml` builds and tests every pull request and every push to `main`. A push to `main`
also releases:

1. `scripts/next_version.ps1` works out the next version from the last `vMAJOR.MINOR.PATCH` tag and the commit
   messages since then (Conventional Commits):

   | Commits since the last tag | Next version |
   | --- | --- |
   | `feat!: ...`, `fix(store)!: ...` or `BREAKING CHANGE:` in a message | major: 1.4.2 → 2.0.0 |
   | `feat: ...` or `feat(scope): ...` | minor: 1.4.2 → 1.5.0 |
   | anything else (`fix:`, `docs:`, `chore:`, ...) | patch: 1.4.2 → 1.4.3 |

   - Before the first tag, the version is `MODELHUB_VERSION` in `CMakeLists.txt` (0.1.0).
   - Raising `MODELHUB_VERSION` above the last tag makes the next release that version.
   - `[skip release]` in the latest commit message builds and tests without releasing.
2. The build stamps that version (and the run number as the fourth part of the file version) into
   `ModelHubSdk.dll`, `ModelHub.exe`, `ModelHubManager.exe`, `mh_version()` and `ModelHubConfigVersion.cmake`,
   and checks it.
3. The tests run, and CPack zips the SDK as `ModelHub-<version>-win64.zip`, with a `.sha256` file.
4. The workflow creates the tag `v<version>` on that commit and a GitHub release with the zip and notes
   generated from the commits.

The zip has the same layout as `build\sdk`, plus the Visual C++ runtime DLLs, so applications can build
against an unzipped release instead of a local ModelHub build:

```cmake
find_package(ModelHub 0.1 CONFIG REQUIRED PATHS C:/sdk/ModelHub-0.1.0-win64)
```

Local builds are version 0.1.0 unless you pass `-DMODELHUB_VERSION=<version>`.

## Repository layout

| Path | Contents |
| --- | --- |
| `src/core/` | `HubCore` and everything it uses: catalog, advisor, hardware, store, downloader, llama.cpp servers, Hugging Face browsing (`HfBrowse`) |
| `src/sdk/` | `ModelHubSdk.dll` (the C interface over `HubCore`) |
| `src/hub/` | `ModelHub.exe` (the HTTP service over `HubCore`) |
| `src/manager/` | `ModelHubManager.exe` (tray icon, model manager and Hugging Face browser, Win32); icons in `res/` (`scripts/make_icons.py`) |
| `include/modelhub/` | the public headers (C, C++14 wrapper, service client) |
| `config/catalog.json` | the catalog: models and task profiles |
| `cmake/` | llama.cpp download and the `ModelHubConfig.cmake` template |
| `scripts/` | `next_version.ps1` (release versions), `make_icons.py`, `merge_catalogs.py` |
| `tests/` | GoogleTest tests |
| `.github/workflows/` | build, test and release |

## Using the DLL

```cmake
find_package(ModelHub CONFIG REQUIRED PATHS ../ModelHub/build/sdk)
target_link_libraries(MyApp PRIVATE ModelHub::Sdk)
modelhub_deploy(MyApp)  # copies ModelHubSdk.dll and catalog.json next to MyApp.exe on every build
```

```cpp
#include "modelhub/modelhub.hpp"  // C++14 or later

modelhub::Sdk hub;
std::string error;
if (hub.Open("{\"app\":\"MyApp\"}", nullptr, &error)) {
    std::string advice = hub.Call("advise", R"({"task":"log-diagnosis","engine":"embedded-cpu",
                                               "workload":{"metrics":{"lines":42000}}})");
    std::string model = hub.Download("qwen2.5-coder-7b", [](const std::string& progress) { return true; });
    std::string server = hub.Call("server_start", R"({"model":"qwen2.5-coder-7b","context_tokens":16384})");
}
```

- **Results** are JSON text: parse them with the JSON library the application already uses. Errors look like
  `{"error": {"message", "type"}, "status": <HTTP-like code>}`.
- **The C interface** is `mh_open`, `mh_call`, `mh_download`, `mh_free` and `mh_close`, usable from any
  compiler or language. The DLL can be delay-loaded, so an application still starts without it.
- **`mh_open` options:**
  - `app`: shown as a user of the models it asks for.
  - `home`: overrides `%MODELHUB_HOME%` / `%LOCALAPPDATA%\ModelHub`.
  - `catalog`: a catalog file to use.
  - `search_dirs`: extra model folders for this process.
  - `llama_server`, `base_port`, `idle_minutes`, `parallel`: for the servers it starts.
  - `scan: false`: don't check model files in the background now.
  - `hf_base`: another Hugging Face address (tests use a local one).

Commands (`mh_call(hub, method, args)`; the same commands serve the HTTP service):

| Command | Arguments | Result |
| --- | --- | --- |
| `status`, `hardware` | | GPU, graphics memory, RAM (total and free), cores, AVX2, store folders, scan state, servers |
| `catalog` | | every model (installed state, path, partial download size, tasks) and every task profile |
| `models` | | installed models |
| `model` | `id`, `app` | one model with its file path (503 while the files are still being checked) |
| `advise` | `task`, `engine` (`hub` or `embedded-cpu`), `workload: {metrics, summary}`, `current`, `context_tokens`, `parallel` | first model, recommended model, notes, and every option with fit, memory, speed, installed |
| `fits` | `context_tokens`, `parallel`, `engine` | fit, memory, GPU layers and speed of every model |
| `add_folder` | `path`, `persist` | shares a folder of GGUF files with every application (kept in `config.json`) |
| `rescan`, `wait_scan` | `timeout_ms` | check model files again / wait for the check |
| `resolve_hf` | `spec` | size and SHA-256 of `owner/repo/file.gguf` |
| `add_model` | `spec`, or `file`, `url`, `size`, `sha256`, plus `id`, `name`, `params_b`, `tools` | adds a custom model (`custom_models.json`) for every application |
| `start_download`, `cancel_download`, `downloads` | `model` | background downloads (the service uses these) |
| `server_start` | `model`, `context_tokens` or `task`, `parallel`, `pin` | starts or reuses a llama-server and answers when it is ready; `pin` keeps it until `server_stop` or `mh_close` |
| `server_stop`, `servers` | `model` | |
| `hf_search` | `query`, `kind` (`text`, `code`, `embeddings`), `sort` (`downloads`, `trending`, `likes`, `updated`), `limit` | GGUF models on Hugging Face, each with category, best use, parameters, architecture, licence and how a Q4_K_M file would run here |
| `hf_repo` | `repo` | one repository: best use, description, notes, every GGUF file judged for this PC (fit, speed, quality, compatible or why not, installed), and the recommended file |
| `hf_add` | `repo`, `file` | adds that file as a custom model (then `mh_download` it) |
| `delete_model` | `model`, `recycle` (default true) | deletes every copy of the model and its partial download, after stopping this process's servers of it; refuses while it downloads here; lists what was deleted, what failed (a file in use) and who used it |

`mh_download(hub, model, progress, user)` downloads on the calling thread. It reports progress as
`{"phase","done","total","bytes_per_second"}`, and the callback returns non-zero to cancel. If another
process is downloading the same file, it waits for that download; if the files are still being checked, it
waits for the check rather than download something another application already has.

## Command line

```text
ModelHub serve [--background]      run the hub (clients start it with --background)
ModelHub start | stop
ModelHub status                    hardware, store, running llama-servers
ModelHub list                      installed models and where their files are
ModelHub catalog                   all models and tasks (JSON)
ModelHub advise <task> [--engine hub|embedded-cpu] [--metric name=value ...] [--current <model>]
ModelHub pull <model>              download with progress
ModelHub path <model>              the installed file of a model
ModelHub delete <model>            delete every copy of a model (to the Recycle Bin)
ModelHub add-folder <dir>          also use models another application keeps in <dir>
ModelHub rescan
```

For example, `ModelHub advise log-diagnosis --engine embedded-cpu --metric lines=200000` on a 64 GB PC
without a GPU recommends `qwen3-coder-30b-a3b`.

## Files

`%LOCALAPPDATA%\ModelHub` holds everything (override it with `MODELHUB_HOME`):

| File | What it is |
| --- | --- |
| `models\` | Models downloaded through ModelHub. |
| `config.json` | Shared settings: service port (default 39281), `search_dirs`, `llama_server`, `idle_minutes` (10), `parallel` slots (2), `base_port` (39300), `llama_args`. |
| `catalog.json` | Optional. Overrides the catalog shipped next to `ModelHub.exe` / `ModelHubSdk.dll`. |
| `custom_models.json` | Models applications added (`add_model`). |
| `manifest.json` | Every model file found, with its hash, whether it is verified, and which applications use it. Shared by every process; changes are made under a machine-wide lock after re-reading it, and each process picks up the others' changes. |
| `hub.json` | The running hub's port and access token. It is removed when the hub stops. |
| `install.json` | Where `ModelHub.exe` is, so clients can start it. |
| `hub.log` | The hub's log. |

The service and every application using the DLL share this folder. The default `search_dirs` are
`%LOCALAPPDATA%\RevEngStudio\models` and `%LOCALAPPDATA%\AppAssist\models`.
Files in them are recognised by catalog file name and exact size, then hashed once. The result is cached by
path, size and modification time, so a 20 GB file is not re-hashed at every start. These files are never
moved, renamed or deleted.

## HTTP API

Every request except `GET /health` needs `Authorization: Bearer <token from hub.json>`. Only requests whose
`Host` is `127.0.0.1` or `localhost` are answered, so web pages in a browser cannot use the hub.

| Request | What it does |
| --- | --- |
| `GET /hub/v1/status` | Hardware, store folders, scan state, running servers. |
| `GET /hub/v1/catalog` | Models (with installed state and path) and tasks. |
| `POST /hub/v1/advise` | `{"task", "engine": "hub" or "embedded-cpu", "workload": {"metrics": {...}, "summary"}, "current"}` returns the recommended model, the notes explaining why, and every option with its fit. |
| `GET /hub/v1/models` | Installed models. |
| `GET /hub/v1/models/{id}?app=Name` | One model, including its `path`. Answers 503 with `Retry-After` while the store is being scanned. |
| `POST /hub/v1/downloads` | `{"model"}`: starts a download. |
| `GET /hub/v1/downloads` | Download progress. |
| `DELETE /hub/v1/downloads/{id}` | Cancels a download; the partial file resumes next time. |
| `POST /hub/v1/folders` | `{"path"}`: also searches an application's own model folder. Its files are verified once and shared with every application; the folder is kept in `config.json`. |
| `POST /hub/v1/rescan` | Looks for model files again. |
| `POST /hub/v1/fits`, `/hub/v1/resolve`, `/hub/v1/custom-models` | The `fits`, `resolve_hf` and `add_model` commands. |
| `POST /hub/v1/hf/search`, `GET /hub/v1/hf/models/{owner}/{repo}`, `POST /hub/v1/hf/add` | The `hf_search`, `hf_repo` and `hf_add` commands. |
| `DELETE /hub/v1/models/{id}` | Deletes every copy of a model (`?recycle=false`: delete instead of recycling); stops its llama-server first. |
| `POST /hub/v1/servers/{id}/start` | `{"task" or "context_tokens", "parallel"}`: loads the model now and answers when it is ready, so the first chat request is not slow. |
| `POST /hub/v1/servers/{id}/stop` | Stops a model's idle llama-server. |
| `POST /hub/v1/shutdown` | Stops the hub. |
| `GET /v1/models`, `POST /v1/chat/completions`, `/v1/completions`, `/v1/embeddings` | OpenAI-compatible; see below. |

How the OpenAI-compatible requests work:
- `model` is a catalog id, an alias (`qwen2.5-coder:7b`) or a file name.
- Streaming (`"stream": true`) is passed through as server-sent events.
- Optional headers:
  - `X-ModelHub-Task`: use that task profile's context size and parallel slots.
  - `X-ModelHub-Context`: tokens per request.
  - `X-ModelHub-Parallel`: parallel slots.
  - `X-ModelHub-App`: your application's name, shown in `status` and recorded in the manifest.

**Servers.** There is one `llama-server` per model and context size, on ports from 39300. GPU layers are
chosen from the graphics memory still free. When a new model needs room, idle servers are stopped first
(least recently used). A server stops after `idle_minutes` without requests.

## Using the service from an application

Applications that use the DLL don't need the service. It is for sharing running llama.cpp servers between
applications, and for applications that would rather speak HTTP.

### C++ (any standard from C++14)

`include/modelhub/modelhub_client.h` is header-only, uses WinHTTP, and needs nothing else.

```cpp
#include "modelhub/modelhub_client.h"

modelhub::client::Client hub("MyApp");
std::string err;
if (hub.Connect(/*startIfNeeded=*/true, 20000, &err)) {
    hub.EnsureLoaded("qwen2.5-coder-7b", "", 16384, 2);  // loads it now
    // OpenAI requests to hub.BaseUrl() with "Authorization: Bearer " + hub.Token().
}
```

`Connect` starts the service when needed. It looks for `ModelHub.exe` in `install.json`, then
`%MODELHUB_EXE%`, then `PATH`.

### Any language

1. Read `port` and `token` from `%LOCALAPPDATA%\ModelHub\hub.json`.
2. Call the HTTP API.
3. Any OpenAI client library works with `base_url = http://127.0.0.1:<port>/v1` and `api_key = <token>`.

## Applications using ModelHub

- **RevEng Studio** (C++20).
  - Its `ModelRuntime` uses the DLL for the catalog, role suggestions (roles map to tasks), the store,
    downloads, custom Hugging Face models and llama.cpp servers.
  - It uses the service for the servers when it is running (`llm.modelHub` `"auto"`) or always
    (`"service"`).
  - Its build takes llama.cpp and `ModelHub.exe` from the SDK.
  - It no longer has a catalog, downloader or llama-server manager of its own.
- **AppAssist** (C++14).
  - Its `ModelAdvisor` uses the DLL for the catalog, task profiles, `embedded-cpu` advice, the store and
    downloads, and shares its model folders.
  - It still runs models in-process with its embedded llama.cpp, using the file path.
  - It no longer has a catalog, advisor rules or downloader of its own.

## The catalog

`config/catalog.json` (format `modelhub-catalog`) merges the catalogs of AppAssist and RevEng Studio. It was
generated once with `scripts/merge_catalogs.py` and is edited by hand from then on.

**Model fields:**
- `id` and `aliases`.
- `file`, `url`, `size` and `sha256`: the file that gets downloaded.
- `alternates`: other files of the same model, accepted when already present. This is how RevEng's and
  AppAssist's different Qwen2.5-Coder 3B, 7B and 14B downloads are both recognised.
- `params_b`, `active_params_b`, `tier` (1–3), `layers`, `context_k`, `kv_cache_mb_8k`, `tools`,
  `embeddings`, `license`, `summary`.

**Tasks:**

| Task | From | Context | Rule |
| --- | --- | --- | --- |
| `log-diagnosis` | AppAssist | 8K | Starts small; grows with lines and symbols. |
| `code-conversion` | AppAssist | 16K | Sized by the largest file. |
| `binary-renaming` | RevEng refiner | 16K × 3 slots, tool calling | Largest model on the GPU. |
| `rename-review` | RevEng reviewer | 16K, tool calling | Largest model on the GPU. |
| `triage-planning` | RevEng planner | 8K | Small and fast. |
| `embeddings` | RevEng | 2K | `nomic-embed-text`. |

**Fit rules:** a model needs its file size × 1.15, plus the KV cache for the task's context and parallel
slots, plus 1 GB.
- **On the GPU (`gpu`):** up to 92 % of graphics memory.
- **Split across GPU and CPU (`gpu+cpu`):** graphics memory plus up to 60 % of RAM.
- **On the CPU only (`cpu`):** up to 75 % of RAM; up to 90 % is `cpu-tight`.

The recommendation is the first model in the workload tier's preferred list that fits comfortably and fits on
disk. Failing that, it is the first one that fits at all.

**Adding a model:** take `size` and `sha256` from
`https://huggingface.co/api/models/<org>/<repo>/tree/main` (`lfs.size`, `lfs.oid`), then add the model's id
to the `prefer` lists of the tasks that should use it. Use single-file, non-gated repositories.

## Licences

ModelHub is released under the [MIT License](LICENSE).

Each model's licence is in the catalog and shown in its summary; some of them, for example Qwen2.5-Coder 3B,
do not allow commercial use. llama.cpp, cpp-httplib and nlohmann/json are MIT-licensed; their notices are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), which ships in the release zip.
