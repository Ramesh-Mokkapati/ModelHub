/* modelhub.h - ModelHubSdk.dll: shared local models for llama.cpp, in the application's own process.
 *
 * Model selection (catalog, hardware- and workload-based advice), the shared model store (files any
 * application downloaded, wherever they are), resumable SHA-256-verified downloads, custom Hugging Face
 * models, and llama.cpp servers started on demand. The same code runs in the ModelHub service; the store,
 * its manifest and config.json under %LOCALAPPDATA%\ModelHub are shared by every process.
 *
 * A plain C interface (any compiler, any C++ standard): every request and result is UTF-8 JSON. The commands
 * are listed in HubCore.h of the ModelHub sources and in README.md; for example
 *
 *     mh_hub* hub = mh_open("{\"app\":\"MyApp\"}", NULL, NULL, &error);
 *     char* advice = mh_call(hub, "advise", "{\"task\":\"code-conversion\",\"engine\":\"embedded-cpu\"}");
 *     ... parse advice ...
 *     mh_free(advice);
 *     mh_close(hub);
 *
 * Errors are JSON too: {"error": {"message": "...", "type": "..."}, "status": <HTTP-like code>}.
 * C++ applications can use modelhub.hpp, a header-only C++14 wrapper.
 */
#ifndef MODELHUB_H
#define MODELHUB_H

#ifdef MODELHUB_SDK_EXPORTS
#define MH_API __declspec(dllexport)
#else
#define MH_API __declspec(dllimport)
#endif
#define MH_CALL __cdecl

#define MODELHUB_API_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mh_hub mh_hub;

/* Log lines (UTF-8), from any thread. */
typedef void(MH_CALL* mh_log_fn)(void* user, const char* line);
/* Download progress: {"model","phase","done","total","bytes_per_second"}. Return non-zero to cancel. */
typedef int(MH_CALL* mh_progress_fn)(void* user, const char* progress_json);

/* "0.1.0" (static string). */
MH_API const char* MH_CALL mh_version(void);
MH_API int MH_CALL mh_api_version(void);

/* Opens the shared store. options (JSON, may be NULL or "{}"):
 *   app           this application's name (recorded as a user of the models it asks for)
 *   home          default %MODELHUB_HOME%, else %LOCALAPPDATA%\ModelHub
 *   catalog       a catalog file to use instead of <home>\catalog.json / catalog.json next to the DLL
 *   search_dirs   extra model folders for this process (add_folder shares one with everyone)
 *   llama_server  llama-server.exe (default: llama\ next to the DLL, else config.json's)
 *   base_port, idle_minutes, parallel   for llama-servers started by server_start
 *   scan          false: do not check model files in the background now
 * Checking model files runs in the background: "model" answers status 503 while it runs, "wait_scan" waits.
 * On failure returns NULL and, if error is not NULL, a message to release with mh_free. */
MH_API mh_hub* MH_CALL mh_open(const char* options_json, mh_log_fn log, void* log_user, char** error);
/* Stops the llama-servers this handle started and releases it. */
MH_API void MH_CALL mh_close(mh_hub* hub);

/* Runs one command; args_json may be NULL. Returns JSON to release with mh_free. Thread-safe. */
MH_API char* MH_CALL mh_call(mh_hub* hub, const char* method, const char* args_json);
/* Downloads a model into the shared store on the calling thread (blocking; resumes partial files; waits if
 * another process is downloading the same file). Returns the model's JSON (with "path") or an error. */
MH_API char* MH_CALL mh_download(mh_hub* hub, const char* model, mh_progress_fn progress, void* user);

MH_API void MH_CALL mh_free(char* text);

#ifdef __cplusplus
}
#endif

#endif /* MODELHUB_H */
