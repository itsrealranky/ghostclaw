<div align="center">

<img src="ghostclaw.jpg" alt="GhostClaw Logo" width="400">

# GhostClaw 🦀

</div>

<div align="center">

**Ghost Protocol. Claw Execution. Zero Compromise. 100% C++. 100% Agnostic.**

🦴 **Grandfather of OpenClaw**

⚡ Runs on $10 hardware with <5MB RAM: That's 99% less memory than OpenClaw and 98% cheaper than a Mac mini!

[![CI](https://github.com/sudiprokaya/GhostClaw/actions/workflows/ci.yml/badge.svg)](https://github.com/sudiprokaya/GhostClaw/actions)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

</div>

<div align="center">

Fast, small, and fully autonomous AI assistant infrastructure — deploy anywhere, swap anything.

`~1.9MB binary` · `~15ms warm start` · `30+ providers` · `10 traits` · `Pluggable everything`

</div>

## ✨ Features

<div align="center">

| 🪶 **Ultra-Lightweight** | ⚡ **Lightning Fast** | 🚀 **True Portability** |
|:---:|:---:|:---:|
| ~2MB peak footprint<br>~10MB RSS | 15ms warm start<br>235ms cold start | Single binary<br>ARM + x86 |

| 🔄 **Fully Swappable** | 🔓 **No Lock-in** | 🔒 **Secure by Design** |
|:---:|:---:|:---:|
| Trait-based architecture<br>Zero code changes | 30+ AI providers<br>OpenAI-compatible | Pairing + sandboxing<br>Workspace scoping |

</div>

---

## 🎯 Why Teams Pick GhostClaw

<table>
<tr>
<td width="50%">

### 🏗️ **Production Ready**
- ⚡ Small C++ binary, fast startup
- 🔒 Pairing, strict sandboxing, allowlists
- 📦 Single self-contained binary
- 🔄 Trait-based, fully swappable

</td>
<td width="50%">

### 🚀 **Feature Rich**
- 🤖 30+ AI provider integrations
- 💬 10+ real messaging channels
- 🌐 Browser automation built-in
- 🎨 Canvas, voice, TTS support

</td>
</tr>
</table>

---

## 📊 Benchmark Snapshot

<div align="center">

*Measured on MacBook Pro M3 Pro (18GB RAM), macOS 26.2, Feb 2026. Release build, `-DCMAKE_BUILD_TYPE=Release`.*

|                            | **OpenClaw** | **NanoBot** | **PicoClaw** | **ZeroClaw** | **GhostClaw** ✨ |
|----------------------------|:------------:|:-----------:|:------------:|:------------:|:----------------:|
| **Language**               | TypeScript   | Python      | Go           | Rust         | **C++**          |
| **RSS Memory**             | > 1GB        | > 100MB     | < 10MB       | < 5MB        | **~10MB** 🏆     |
| **Peak Footprint**         | —            | —           | —            | —            | **~2MB** 🏆      |
| **Warm Start**             | > 500ms      | > 30ms      | < 100ms      | ~10ms        | **~15ms** 🚀     |
| **Cold Start**             | > 5s         | > 1s        | < 1s         | ~440ms       | **~235ms** 🚀    |
| **Binary Size (stripped)** | ~28MB (dist) | N/A         | ~8MB         | ~3.9MB       | **1.9MB** 🏆     |

</div>

<details>
<summary>📈 <b>View Detailed Results</b></summary>

### GhostClaw Detailed Measurements

```
Binary:       1,989,792 bytes stripped  (1.9 MB)
              2,406,968 bytes unstripped (2.3 MB)

Warm start:   ~15ms  (range: 13.5–17.4ms across 20 runs)
Cold start:   ~235ms (range: 226–245ms across 10 runs, cache-flushed)

RSS memory:   ~9.5 MB  (--version, includes shared system dylibs)
              ~14.7 MB (doctor, with config + diagnostics loaded)
Peak footprint: ~1.9 MB  (--version, memory attributed to process only)
                ~4.9 MB  (doctor)
```

### 🔍 Cold vs Warm Start Explained

- **Warm Start**: Binary and shared libraries already in OS page cache. This is what you get on repeated runs.
- **Cold Start**: Binary not in page cache (simulated via cache pressure). Dominated by dylib loading (libcurl, libssl, libsqlite3). This is closer to first-run-after-reboot.

Most interactive usage (running `ghostclaw agent`, `ghostclaw status`, etc.) hits warm-cache paths after the first invocation.

### 🔬 Reproduce Locally

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Binary size
strip -o ghostclaw-stripped build/ghostclaw
ls -lh ghostclaw-stripped

# Memory + timing
/usr/bin/time -lp ./build/ghostclaw --version

# Warm start (run a few times first to warm cache, then measure)
for i in {1..20}; do /usr/bin/time -lp ./build/ghostclaw --version 2>&1 | grep "^real"; done

# Cold start (requires sudo for cache purge)
sudo purge && /usr/bin/time -lp ./build/ghostclaw --version
```

</details>

> **How we measured**: `ghostclaw --version`, release build, `/usr/bin/time -lp` for RSS/footprint, custom `gettimeofday()` wrapper for sub-ms startup timing. Warm = 20 consecutive runs (median). Cold = fresh binary copy + 512MB cache-pressure write between runs (median of 10). Other tools' numbers are from their own published benchmarks or our local reproduction where possible.

---

## 🚀 Quick Start

```bash
git clone https://github.com/sudiprokaya/GhostClaw.git
cd GhostClaw
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Just run it — the setup wizard launches automatically on first run
./build/ghostclaw
```

<div align="center">

**The wizard walks you through provider, model, API key, memory, channels, and skills in 7 steps,<br>then offers to drop you straight into the agent.**

</div>

### 💡 Usage Examples

```bash
# Skip the wizard entirely with flags
ghostclaw onboard --provider openrouter --api-key sk-...

# Single message
ghostclaw agent -m "Hello, GhostClaw!"

# Interactive chat
ghostclaw agent

# Start the gateway (webhook server)
ghostclaw gateway

# Start full autonomous daemon
ghostclaw daemon

# Diagnostics
ghostclaw status
ghostclaw doctor
```

### 🐳 Docker

```bash
docker build -t ghostclaw .
docker run --rm ghostclaw --help
docker run -d --name ghostclaw-daemon -p 8080:8080 ghostclaw daemon --host 0.0.0.0 --port 8080
```

---

## 🏗️ Architecture

<div align="center">

**Every subsystem is a trait — swap implementations with a config change, zero code changes.**

</div>

| Subsystem | Trait | Ships with | Extend |
|-----------|-------|------------|--------|
| 🤖 **AI Models** | Provider | 30+ providers (OpenRouter, Anthropic, OpenAI, Google, Ollama, Groq, Cerebras, Mistral, xAI, DeepSeek, Together, Fireworks, NVIDIA, Cloudflare, HuggingFace, etc.) | `custom:https://your-api.com` — any OpenAI-compatible API |
| 💬 **Channels** | Channel | CLI, Telegram, Discord, Slack, iMessage, Matrix, WhatsApp, Signal, Webhook | Any messaging API |
| 🧠 **Memory** | Memory | SQLite with hybrid search (FTS5 + vector cosine similarity), Markdown | Any persistence backend |
| 🛠️ **Tools** | Tool | shell, file_read, file_write, memory_store, memory_recall, memory_forget, browser_open, web_search, canvas | Any capability |
| 📊 **Observability** | Observer | Noop, Log, Multi | Prometheus, OTel |
| ⚙️ **Runtime** | RuntimeAdapter | Native (Mac/Linux) | Docker (planned) |
| 🔒 **Security** | SecurityPolicy | Gateway pairing, sandbox, allowlists, rate limits, filesystem scoping, encrypted secrets | — |
| 👤 **Identity** | IdentityConfig | OpenClaw (markdown), AIEOS v1.1 (JSON) | Any identity format |
| 🌐 **Tunnel** | Tunnel | None, Cloudflare, Tailscale, ngrok, Custom | Any tunnel binary |
| 🎯 **Skills** | Loader | TOML manifests + SKILL.md instructions | Community skill packs |

---

## 🧠 Memory System

<div align="center">

**All custom, zero external dependencies — no Pinecone, no Elasticsearch, no LangChain**

</div>

| Layer | Implementation |
|-------|----------------|
| **Vector DB** | Embeddings stored as BLOB in SQLite, cosine similarity search |
| **Keyword Search** | FTS5 virtual tables with BM25 scoring |
| **Hybrid Merge** | Custom weighted merge function |
| **Embeddings** | EmbeddingProvider trait — OpenAI, local, or noop |
| **Chunking** | Line-based markdown chunker with heading preservation |
| **Caching** | SQLite embedding_cache table with LRU eviction |

```toml
[memory]
backend = "sqlite"          # "sqlite", "markdown", "none"
auto_save = true
embedding_provider = "openai"
vector_weight = 0.7
keyword_weight = 0.3
```

---

## 🔒 Security

<div align="center">

**GhostClaw enforces security at every layer**

</div>

| # | Item | Status | How |
|---|------|:------:|-----|
| 1 | Gateway not publicly exposed | ✅ | Binds 127.0.0.1 by default. Refuses 0.0.0.0 without tunnel or explicit `allow_public_bind = true` |
| 2 | Pairing required | ✅ | 6-digit one-time code on startup. Exchange via POST /pair for bearer token |
| 3 | Filesystem scoped | ✅ | `workspace_only = true` by default. System dirs blocked. Symlink escape detection |
| 4 | Access via tunnel only | ✅ | Gateway refuses public bind without active tunnel |

---

## ⚙️ Configuration

<div align="center">

Config: `~/.ghostclaw/config.toml` (created automatically on first run)

</div>

<details>
<summary>📝 <b>View Example Configuration</b></summary>

```toml
api_key = "sk-..."
default_provider = "openrouter"
default_model = "anthropic/claude-sonnet-4-20250514"
default_temperature = 0.7

[memory]
backend = "sqlite"
auto_save = true
embedding_provider = "openai"

[gateway]
require_pairing = true
allow_public_bind = false

[autonomy]
level = "supervised"            # "readonly", "supervised", "full"
workspace_only = true
allowed_commands = ["git", "npm", "cargo", "ls", "cat", "grep"]

[tunnel]
provider = "none"               # "none", "cloudflare", "tailscale", "ngrok", "custom"

[secrets]
encrypt = true

[browser]
enabled = false
allowed_domains = ["docs.rs"]
```

</details>

<div align="center">

📚 **Full reference:** [docs/CONFIGURATION.md](docs/CONFIGURATION.md)

</div>

---

## 🎮 Commands

<table>
<tr>
<td width="50%">

### Core Commands

| Command | Description |
|---------|-------------|
| *(no args, first run)* | Auto-launches 7-step wizard |
| `onboard` | Re-run setup wizard |
| `onboard --provider X` | Non-interactive setup |
| `agent` | Interactive chat mode |
| `agent -m "..."` | Single message mode |
| `gateway` | Start HTTP gateway |
| `daemon` | Start autonomous daemon |

</td>
<td width="50%">

### Management Commands

| Command | Description |
|---------|-------------|
| `service install/start/stop` | Manage background service |
| `doctor` | Run diagnostics checks |
| `status` | Show system status |
| `channel doctor` | Check channel health |
| `cron` | Manage scheduler jobs |
| `skills` | Manage skill packages |
| `tts` | Text-to-speech providers |
| `voice` | Wake-word utilities |
| `integrations` | Browse registry |

</td>
</tr>
</table>

---

## 🛠️ Development

```bash
# Debug build + tests
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure

# Release build
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j

# Run benchmarks
./build/benches/ghostclaw_benchmarks
```

---

## 🤝 Contributing

<div align="center">

See [CONTRIBUTING.md](CONTRIBUTING.md). Implement a trait, submit a PR:

</div>

<table>
<tr>
<td width="50%">

- 🤖 **New Provider** → `src/providers/`
- 💬 **New Channel** → `src/channels/`
- 📊 **New Observer** → `src/observability/`
- 🛠️ **New Tool** → `src/tools/`

</td>
<td width="50%">

- 🧠 **New Memory** → `src/memory/`
- 🌐 **New Tunnel** → `src/tunnel/`
- 🎯 **New Skill** → `~/.ghostclaw/workspace/skills/<name>/`

</td>
</tr>
</table>

---

## 📄 License

<div align="center">

MIT — see [LICENSE](LICENSE)

</div>

---

<div align="center">

**GhostClaw — Ghost Protocol. Claw Execution. Zero Compromise.** 🦀

</div>
