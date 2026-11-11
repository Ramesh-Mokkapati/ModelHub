"""Builds config/catalog.json from the model catalogs of AppAssist and RevEng Studio.

Run once when adopting an application's catalog (it is not part of the build):
    python scripts/merge_catalogs.py <AppAssist>/config/models.json <RevEng>/config/models.json > config/catalog.json

Rules:
- Models are matched by family and size; the AppAssist entry (official publisher files, licence data) is the
  primary file, and a different file of the same model from RevEng becomes an "alternate", so an existing
  download of either is reused.
- RevEng's Ollama-style ids ("qwen2.5-coder:7b") become aliases.
- RevEng's roles become task profiles that prefer the largest model fitting on the GPU (start_small: false).
"""
import json
import sys

# RevEng id -> AppAssist id for the same model (identical or alternate file).
SAME = {
    "qwen2.5-coder:3b": "qwen2.5-coder-3b",
    "qwen2.5-coder:7b": "qwen2.5-coder-7b",
    "qwen2.5-coder:14b": "qwen2.5-coder-14b",
    "qwen2.5-coder:32b": "qwen2.5-coder-32b",
    "qwen3-coder:30b": "qwen3-coder-30b-a3b",
    "llama3.1:8b": "llama-3.1-8b",
    "deepseek-coder-v2:16b": "deepseek-coder-v2-lite",
}
# Transformer blocks, for partial GPU offload (from the models' configs).
LAYERS = {
    "qwen2.5-coder-1.5b": 28, "qwen2.5-coder-3b": 36, "qwen2.5-coder-7b": 28, "qwen2.5-coder-14b": 48,
    "qwen2.5-coder-32b": 64, "qwen3-coder-30b-a3b": 48, "phi-4-mini": 32, "phi-4": 40, "gemma-3-4b": 34,
    "gemma-3-12b": 48, "llama-3.1-8b": 32, "deepseek-coder-v2-lite": 27,
}
TOOLS = {"phi-4-mini": True, "gemma-3-4b": False, "gemma-3-12b": False, "phi-4": False, "qwen2.5-coder-1.5b": True}
LICENSE = {
    "devstral:24b": "Apache-2.0", "gpt-oss:20b": "Apache-2.0", "qwen3:8b": "Apache-2.0", "qwen3:14b": "Apache-2.0",
    "nomic-embed-text": "Apache-2.0", "qwen2.5-coder:0.5b": "Apache-2.0",
}
NEW_IDS = {"devstral:24b": "devstral-small-24b", "gpt-oss:20b": "gpt-oss-20b", "qwen3:8b": "qwen3-8b",
           "qwen3:14b": "qwen3-14b", "nomic-embed-text": "nomic-embed-text-v1.5", "qwen2.5-coder:0.5b": "qwen2.5-coder-0.5b"}


def tier_for(params):
    return 1 if params <= 5 else 2 if params <= 16 else 3


def main(appassist_path, reveng_path):
    a = json.load(open(appassist_path, encoding="utf-8"))
    r = json.load(open(reveng_path, encoding="utf-8"))
    models = {}
    for m in a["models"]:
        e = {
            "id": m["id"], "aliases": [], "name": m["name"], "family": m["id"].rsplit("-", 1)[0],
            "file": m["file"], "url": m["url"], "size": m["size"], "sha256": m["sha256"],
            "params_b": m["params_b"], "active_params_b": m.get("active_params_b", m["params_b"]), "tier": m["tier"],
            "layers": LAYERS.get(m["id"], 0), "context_k": 32, "kv_cache_mb_8k": m.get("kv_cache_mb_8k", 500),
            "tools": TOOLS.get(m["id"], True), "license": m.get("license", ""), "summary": m.get("summary", ""),
            "alternates": [],
        }
        models[m["id"]] = e
    roles = {}
    for m in r["models"]:
        g = m.get("gguf", {})
        url = "https://huggingface.co/%s/resolve/main/%s" % (g["repo"], g["file"])
        target = SAME.get(m["id"])
        if target:
            e = models[target]
            e["aliases"].append(m["id"])
            e["context_k"] = m.get("contextK", e["context_k"])
            e["tools"] = e["tools"] or m.get("tools", False)
            if g["sha256"] != e["sha256"]:
                e["alternates"].append({"file": g["file"], "url": url, "size": g["bytes"], "sha256": g["sha256"]})
            mid = target
        else:
            mid = NEW_IDS[m["id"]]
            params = m["paramsB"]
            models[mid] = {
                "id": mid, "aliases": [m["id"]], "name": m["name"] + " (Q4_K_M)" if "Q4_K_M" in g["file"] else m["name"],
                "family": m.get("family", ""), "file": g["file"], "url": url, "size": g["bytes"], "sha256": g["sha256"],
                "params_b": params, "active_params_b": 3.6 if mid == "gpt-oss-20b" else params,
                "tier": tier_for(params), "layers": m.get("layers", 0), "context_k": m.get("contextK", 32),
                "kv_cache_mb_8k": max(64, int(params * 64)), "tools": m.get("tools", False),
                "embeddings": "embeddings" in m.get("roles", []), "license": LICENSE.get(m["id"], ""),
                "summary": m.get("notes", ""), "alternates": [],
            }
        for role in m.get("roles", []):
            roles.setdefault(role, []).append(mid)

    def by_size_desc(ids):
        return sorted(set(ids), key=lambda i: -models[i]["size"])

    tasks = list(a["tasks"])
    tasks.append({
        "id": "binary-renaming", "name": "Naming decompiled functions (RevEng Studio refiner)",
        "workload": "binary", "context_tokens": 16384, "parallel": 3, "needs_tools": True, "start_small": False,
        "tiers": [{"label": "", "explain": "Renaming quality grows with model size: the largest model that runs on "
                   "the GPU is suggested.", "prefer": [i for i in by_size_desc(roles["refiner"]) if models[i]["tools"]]}],
    })
    tasks.append({
        "id": "rename-review", "name": "Reviewing proposed names (RevEng Studio reviewer)",
        "workload": "binary", "context_tokens": 16384, "parallel": 1, "needs_tools": True, "start_small": False,
        "tiers": [{"label": "", "explain": "A second model gives an independent opinion.",
                   "prefer": [i for i in by_size_desc(roles["reviewer"]) if models[i]["tools"]]}],
    })
    tasks.append({
        "id": "triage-planning", "name": "Triage and planning (RevEng Studio planner)",
        "workload": "binary", "context_tokens": 8192, "parallel": 1, "needs_tools": True, "start_small": True,
        "tiers": [{"label": "", "explain": "A small, fast model is enough to plan work.",
                   "prefer": sorted(set(roles["planner"]), key=lambda i: models[i]["size"])}],
    })
    tasks.append({
        "id": "embeddings", "name": "Similarity search (embeddings)",
        "workload": "code", "context_tokens": 2048, "parallel": 1, "start_small": True,
        "tiers": [{"label": "", "explain": "Embeddings for finding similar code.", "prefer": roles["embeddings"]}],
    })
    for m in models.values():
        if not m["alternates"]:
            del m["alternates"]
        if not m["aliases"]:
            del m["aliases"]
        if not m.get("embeddings"):
            m.pop("embeddings", None)
    out = {
        "format": "modelhub-catalog", "version": 1,
        "comment": "Local GGUF models ModelHub can suggest, download and serve. Each file is pinned by exact size "
                   "and SHA-256; 'alternates' are other files of the same model that are accepted when already "
                   "present. Tasks say which models suit a job (see README). A copy at %LOCALAPPDATA%\\ModelHub\\"
                   "catalog.json overrides this one.",
        "models": sorted(models.values(), key=lambda m: m["size"]),
        "tasks": tasks,
    }
    json.dump(out, sys.stdout, indent=2, ensure_ascii=False)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
