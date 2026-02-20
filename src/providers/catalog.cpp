#include "ghostclaw/providers/catalog.hpp"

#include "ghostclaw/auth/oauth.hpp"
#include "ghostclaw/common/fs.hpp"
#include "ghostclaw/common/json_util.hpp"
#include "ghostclaw/config/config.hpp"
#include "ghostclaw/providers/traits.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <unordered_map>

namespace ghostclaw::providers {

namespace {

constexpr std::int64_t MODEL_CACHE_TTL_SECONDS = 24 * 60 * 60;
constexpr std::uint64_t MODEL_FETCH_TIMEOUT_MS = 20'000;
constexpr std::size_t MAX_MODELS_IN_CATALOG = 1024;

std::int64_t now_unix_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::chrono::system_clock::time_point from_unix_seconds(const std::int64_t value) {
  return std::chrono::system_clock::time_point(std::chrono::seconds(value));
}

std::int64_t to_unix_seconds(const std::chrono::system_clock::time_point time_point) {
  return std::chrono::duration_cast<std::chrono::seconds>(time_point.time_since_epoch()).count();
}

std::string normalize_id(const std::string &value) {
  std::string out = common::to_lower(common::trim(value));
  if (out == "z.ai" || out == "z-ai") {
    return "zai";
  }
  if (out == "opencode-zen") {
    return "opencode";
  }
  if (out == "kimi-code") {
    return "kimi-coding";
  }
  if (out == "cloudflare-ai") {
    return "cloudflare-ai-gateway";
  }
  if (out == "copilot") {
    return "github-copilot";
  }
  return out;
}

std::string provider_env_prefix(const std::string &provider) {
  std::string prefix;
  prefix.reserve(provider.size() + 4);
  for (const char ch : provider) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
      prefix.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
      continue;
    }
    prefix.push_back('_');
  }
  return prefix;
}

std::optional<std::string> read_env(const char *name) {
  if (name == nullptr || *name == '\0') {
    return std::nullopt;
  }
  const char *value = std::getenv(name);
  if (value == nullptr) {
    return std::nullopt;
  }
  const std::string trimmed = common::trim(value);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  return trimmed;
}

std::optional<std::string> read_first_env(const std::vector<const char *> &names) {
  for (const auto *name : names) {
    const auto value = read_env(name);
    if (value.has_value()) {
      return value;
    }
  }
  return std::nullopt;
}

const std::unordered_map<std::string, std::vector<const char *>> &provider_env_keys() {
  static const std::unordered_map<std::string, std::vector<const char *>> keys = {
      {"openai", {"OPENAI_API_KEY"}},
      {"openai-codex", {"OPENAI_CODEX_API_KEY", "OPENAI_API_KEY"}},
      {"anthropic", {"ANTHROPIC_OAUTH_TOKEN", "ANTHROPIC_API_KEY"}},
      {"openrouter", {"OPENROUTER_API_KEY"}},
      {"opencode", {"OPENCODE_API_KEY", "OPENCODE_ZEN_API_KEY"}},
      {"google", {"GEMINI_API_KEY"}},
      {"google-vertex", {"GOOGLE_VERTEX_API_KEY", "GEMINI_API_KEY"}},
      {"google-antigravity", {"GOOGLE_ANTIGRAVITY_API_KEY", "GEMINI_API_KEY"}},
      {"google-gemini-cli", {"GOOGLE_GEMINI_CLI_API_KEY", "GEMINI_API_KEY"}},
      {"zai", {"ZAI_API_KEY", "Z_AI_API_KEY"}},
      {"glm", {"GLM_API_KEY"}},
      {"xai", {"XAI_API_KEY"}},
      {"grok", {"XAI_API_KEY"}},
      {"groq", {"GROQ_API_KEY"}},
      {"cerebras", {"CEREBRAS_API_KEY"}},
      {"mistral", {"MISTRAL_API_KEY"}},
      {"github-copilot", {"COPILOT_GITHUB_TOKEN", "GH_TOKEN", "GITHUB_TOKEN"}},
      {"huggingface", {"HUGGINGFACE_HUB_TOKEN", "HF_TOKEN"}},
      {"moonshot", {"MOONSHOT_API_KEY"}},
      {"kimi-coding", {"KIMI_API_KEY", "KIMICODE_API_KEY"}},
      {"qwen-portal", {"QWEN_OAUTH_TOKEN", "QWEN_PORTAL_API_KEY"}},
      {"minimax", {"MINIMAX_API_KEY"}},
      {"xiaomi", {"XIAOMI_API_KEY"}},
      {"venice", {"VENICE_API_KEY"}},
      {"together", {"TOGETHER_API_KEY"}},
      {"qianfan", {"QIANFAN_API_KEY"}},
      {"deepseek", {"DEEPSEEK_API_KEY"}},
      {"fireworks", {"FIREWORKS_API_KEY"}},
      {"perplexity", {"PERPLEXITY_API_KEY"}},
      {"cohere", {"COHERE_API_KEY"}},
      {"nvidia", {"NVIDIA_API_KEY"}},
      {"vercel-ai-gateway", {"AI_GATEWAY_API_KEY"}},
      {"cloudflare", {"CLOUDFLARE_API_KEY", "CLOUDFLARE_API_TOKEN"}},
      {"cloudflare-ai-gateway", {"CLOUDFLARE_AI_GATEWAY_API_KEY"}},
      {"ollama", {"OLLAMA_API_KEY"}},
      {"vllm", {"VLLM_API_KEY"}},
      {"litellm", {"LITELLM_API_KEY"}},
  };
  return keys;
}

std::string resolve_base_url(const std::string &provider, const std::string &default_base_url) {
  const std::string prefix = provider_env_prefix(provider);
  const std::string local_var = prefix + "_BASE_URL";
  const std::string global_var = "GHOSTCLAW_" + prefix + "_BASE_URL";
  if (const auto local = read_env(local_var.c_str()); local.has_value()) {
    return *local;
  }
  if (const auto global = read_env(global_var.c_str()); global.has_value()) {
    return *global;
  }
  return default_base_url;
}

std::optional<std::string> resolve_api_key(const config::Config &config, const std::string &provider) {
  const std::string normalized_provider = normalize_id(provider);
  const std::string normalized_default_provider = normalize_id(config.default_provider);

  if (normalized_provider == normalized_default_provider && config.api_key.has_value()) {
    const std::string configured_key = common::trim(*config.api_key);
    if (!configured_key.empty()) {
      return configured_key;
    }
  }

  const auto env_it = provider_env_keys().find(normalized_provider);
  if (env_it != provider_env_keys().end()) {
    const auto env_key = read_first_env(env_it->second);
    if (env_key.has_value()) {
      return env_key;
    }
  }

  if (normalized_provider == "openai" || normalized_provider == "openai-codex") {
    auto http = std::make_shared<CurlHttpClient>();
    if (auth::has_valid_tokens()) {
      auto token = auth::get_valid_access_token(*http);
      if (token.ok()) {
        return token.value();
      }
    }
  }

  return std::nullopt;
}

const std::unordered_map<std::string, std::vector<std::string>> &builtin_model_catalogs() {
  static const std::unordered_map<std::string, std::vector<std::string>> catalogs = {
      {"openrouter",
       {"openai/gpt-4o", "openai/gpt-4o-mini", "anthropic/claude-sonnet-4-5-20250929",
        "google/gemini-2.0-flash-exp", "meta-llama/llama-3.1-70b-instruct"}},
      {"openai", {"gpt-4o", "gpt-4o-mini", "o1", "o1-mini"}},
      {"openai-codex", {"gpt-4o", "gpt-4o-mini", "o1-mini"}},
      {"anthropic",
       {"claude-sonnet-4-5-20250929", "claude-opus-4-6", "claude-3-haiku-20240307"}},
      {"google", {"gemini-2.0-flash-exp", "gemini-1.5-pro", "gemini-1.5-flash"}},
      {"google-vertex", {"gemini-2.0-flash-exp", "gemini-1.5-pro"}},
      {"grok", {"grok-2-latest", "grok-2-mini"}},
      {"xai", {"grok-2-latest", "grok-2-mini"}},
      {"groq", {"llama-3.1-70b-versatile", "llama-3.1-8b-instant", "mixtral-8x7b-32768"}},
      {"cerebras", {"llama3.1-70b", "llama3.1-8b"}},
      {"mistral", {"mistral-large-latest", "mistral-medium-latest", "mistral-small-latest"}},
      {"deepseek", {"deepseek-chat", "deepseek-coder"}},
      {"perplexity", {"llama-3.1-sonar-large-128k-online", "llama-3.1-sonar-small-128k-online"}},
      {"cohere", {"command-r-plus", "command-r"}},
      {"fireworks",
       {"accounts/fireworks/models/llama-v3p1-70b-instruct",
        "accounts/fireworks/models/mixtral-8x7b-instruct"}},
      {"together",
       {"meta-llama/Meta-Llama-3.1-70B-Instruct-Turbo",
        "mistralai/Mixtral-8x7B-Instruct-v0.1"}},
      {"nvidia", {"meta/llama-3.1-70b-instruct", "meta/llama-3.1-8b-instruct"}},
      {"moonshot", {"moonshot-v1-128k", "moonshot-v1-32k"}},
      {"qwen-portal", {"qwen-max", "qwen-plus", "qwen-turbo"}},
      {"minimax", {"abab6.5s-chat", "abab5.5-chat"}},
      {"glm", {"glm-4", "glm-3-turbo"}},
      {"ollama", {"llama3.1:8b", "codellama:13b", "mistral:7b"}},
      {"vllm", {"meta-llama/Llama-3.1-8B-Instruct"}},
      {"litellm", {"gpt-4o"}},
      {"huggingface", {"meta-llama/Meta-Llama-3.1-70B-Instruct"}},
      {"cloudflare", {"@cf/meta/llama-3.1-8b-instruct"}},
      {"opencode", {"opencode-chat", "opencode-coder"}},
      {"zai", {"glm-4.5", "glm-4.5-air"}},
      {"venice", {"venice-large", "venice-fast"}},
      {"qianfan", {"ernie-4.0-8k", "ernie-3.5-8k"}},
      {"github-copilot", {"gpt-4o", "claude-3.5-sonnet", "o1-mini"}},
      {"kimi-coding", {"kimi-k2", "kimi-k1.5"}},
      {"xiaomi", {"mim-2", "mim-2-lite"}},
      {"cloudflare-ai-gateway", {"claude-3-7-sonnet", "claude-3-5-haiku"}},
      {"vercel-ai-gateway", {"claude-sonnet-4", "gpt-4o-mini"}},
      {"synthetic", {"synthetic-default"}},
  };
  return catalogs;
}

struct CachedCatalog {
  std::vector<std::string> models;
  std::chrono::system_clock::time_point updated_at{};
};

std::filesystem::path model_cache_path(const std::filesystem::path &workspace,
                                       const std::string &provider) {
  return workspace / "models" / (provider + ".cache");
}

common::Result<void> write_cache(const std::filesystem::path &path,
                                 const std::vector<std::string> &models,
                                 const std::chrono::system_clock::time_point updated_at) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    return common::Result<void>::failure("failed to create model cache dir: " + ec.message());
  }

  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    return common::Result<void>::failure("failed to write model cache file");
  }

  out << "updated=" << to_unix_seconds(updated_at) << "\n";
  for (const auto &model : models) {
    out << model << "\n";
  }

  if (!out) {
    return common::Result<void>::failure("failed to flush model cache file");
  }
  return common::Result<void>::success();
}

std::optional<CachedCatalog> read_cache(const std::filesystem::path &path) {
  if (!std::filesystem::exists(path)) {
    return std::nullopt;
  }

  std::ifstream in(path);
  if (!in) {
    return std::nullopt;
  }

  std::string first_line;
  if (!std::getline(in, first_line)) {
    return std::nullopt;
  }

  if (!common::starts_with(first_line, "updated=")) {
    return std::nullopt;
  }

  std::int64_t updated = 0;
  try {
    updated = std::stoll(first_line.substr(std::string("updated=").size()));
  } catch (...) {
    return std::nullopt;
  }

  CachedCatalog cache;
  cache.updated_at = from_unix_seconds(updated);

  std::string line;
  while (std::getline(in, line)) {
    line = common::trim(line);
    if (!line.empty()) {
      cache.models.push_back(line);
    }
  }

  if (cache.models.empty()) {
    return std::nullopt;
  }
  return cache;
}

enum class CatalogAuthMode {
  None,
  Bearer,
  Anthropic,
};

struct CatalogRoute {
  std::string url;
  CatalogAuthMode auth_mode = CatalogAuthMode::Bearer;
  bool supports_live = true;
};

std::optional<CatalogRoute> resolve_catalog_route(const std::string &provider_id) {
  const std::string provider = normalize_id(provider_id);

  if (provider == "ollama") {
    return CatalogRoute{.url = resolve_base_url(provider, "http://127.0.0.1:11434") + "/api/tags",
                        .auth_mode = CatalogAuthMode::None,
                        .supports_live = true};
  }
  if (provider == "vllm") {
    return CatalogRoute{.url = resolve_base_url(provider, "http://127.0.0.1:8000/v1") + "/models",
                        .auth_mode = CatalogAuthMode::None,
                        .supports_live = true};
  }
  if (provider == "litellm") {
    return CatalogRoute{.url = resolve_base_url(provider, "http://localhost:4000") + "/v1/models",
                        .auth_mode = CatalogAuthMode::None,
                        .supports_live = true};
  }

  if (provider == "synthetic" || provider == "cloudflare-ai-gateway" ||
      provider == "vercel-ai-gateway" || provider == "google-antigravity" ||
      provider == "google-gemini-cli") {
    return CatalogRoute{.supports_live = false};
  }

  static const std::unordered_map<std::string, std::string> base_urls = {
      {"openrouter", "https://openrouter.ai/api/v1"},
      {"openai", "https://api.openai.com/v1"},
      {"openai-codex", "https://api.openai.com/v1"},
      {"opencode", "https://opencode.ai/zen/v1"},
      {"google", "https://generativelanguage.googleapis.com/v1beta/openai"},
      {"google-vertex", "https://generativelanguage.googleapis.com/v1beta/openai"},
      {"zai", "https://api.z.ai/api/paas/v4"},
      {"glm", "https://open.bigmodel.cn/api/paas/v4"},
      {"xai", "https://api.x.ai/v1"},
      {"grok", "https://api.x.ai/v1"},
      {"groq", "https://api.groq.com/openai/v1"},
      {"cerebras", "https://api.cerebras.ai/v1"},
      {"mistral", "https://api.mistral.ai/v1"},
      {"huggingface", "https://router.huggingface.co/v1"},
      {"moonshot", "https://api.moonshot.ai/v1"},
      {"qwen-portal", "https://portal.qwen.ai/v1"},
      {"venice", "https://api.venice.ai/api/v1"},
      {"together", "https://api.together.xyz/v1"},
      {"qianfan", "https://qianfan.baidubce.com/v2"},
      {"deepseek", "https://api.deepseek.com/v1"},
      {"fireworks", "https://api.fireworks.ai/inference/v1"},
      {"perplexity", "https://api.perplexity.ai"},
      {"cohere", "https://api.cohere.ai/v1"},
      {"nvidia", "https://integrate.api.nvidia.com/v1"},
      {"github-copilot", "https://api.githubcopilot.com"},
      {"cloudflare", "https://api.cloudflare.com/client/v4/accounts/{account_id}/ai/v1"},
      {"anthropic", "https://api.anthropic.com/v1"},
  };

  const auto route_it = base_urls.find(provider);
  if (route_it == base_urls.end()) {
    return std::nullopt;
  }

  const std::string base_url = resolve_base_url(provider, route_it->second);
  if (base_url.find("{account_id}") != std::string::npos ||
      base_url.find("<account_id>") != std::string::npos ||
      base_url.find("<gateway_id>") != std::string::npos) {
    return CatalogRoute{.supports_live = false};
  }

  CatalogRoute route;
  route.url = base_url + "/models";
  route.auth_mode = provider == "anthropic" ? CatalogAuthMode::Anthropic : CatalogAuthMode::Bearer;
  route.supports_live = true;
  return route;
}

void append_model(std::vector<std::string> &models, const std::string &raw_model) {
  std::string model = common::trim(raw_model);
  if (model.empty()) {
    return;
  }
  if (model.size() > 256) {
    return;
  }
  models.push_back(std::move(model));
}

std::vector<std::string> models_from_array_objects(const std::string &array_json,
                                                   const std::vector<std::string> &preferred_fields) {
  std::vector<std::string> models;
  for (const auto &item : common::json_split_top_level_objects(array_json)) {
    for (const auto &field : preferred_fields) {
      const std::string value = common::json_get_string(item, field);
      if (!value.empty()) {
        append_model(models, value);
        break;
      }
    }
  }
  return models;
}

std::vector<std::string> extract_models_from_response(const std::string &provider_id,
                                                      const std::string &response_body) {
  std::vector<std::string> models;

  const std::string data_array = common::json_get_array(response_body, "data");
  if (!data_array.empty()) {
    auto extracted = models_from_array_objects(data_array, {"id", "model", "name"});
    models.insert(models.end(), extracted.begin(), extracted.end());
  }

  const std::string models_array = common::json_get_array(response_body, "models");
  if (!models_array.empty()) {
    if (normalize_id(provider_id) == "ollama") {
      auto extracted = models_from_array_objects(models_array, {"name", "model", "id"});
      models.insert(models.end(), extracted.begin(), extracted.end());
    } else {
      auto extracted = models_from_array_objects(models_array, {"id", "model", "name"});
      models.insert(models.end(), extracted.begin(), extracted.end());
    }
  }

  const std::string trimmed = common::trim(response_body);
  if (!trimmed.empty() && trimmed.front() == '[' && trimmed.back() == ']') {
    auto extracted = models_from_array_objects(trimmed, {"id", "model", "name"});
    models.insert(models.end(), extracted.begin(), extracted.end());
  }

  if (models.empty()) {
    static const std::regex id_pattern(R"PATTERN("id"\s*:\s*"([^"]+)")PATTERN");
    for (std::sregex_iterator it(response_body.begin(), response_body.end(), id_pattern), end;
         it != end; ++it) {
      append_model(models, (*it)[1].str());
      if (models.size() >= MAX_MODELS_IN_CATALOG) {
        break;
      }
    }
  }

  std::sort(models.begin(), models.end());
  models.erase(std::unique(models.begin(), models.end()), models.end());
  if (models.size() > MAX_MODELS_IN_CATALOG) {
    models.resize(MAX_MODELS_IN_CATALOG);
  }
  return models;
}

common::Result<std::vector<std::string>> fetch_live_models(const config::Config &config,
                                                           const std::string &provider_id) {
  const auto route = resolve_catalog_route(provider_id);
  if (!route.has_value() || !route->supports_live) {
    return common::Result<std::vector<std::string>>::failure(
        "live catalog is not available for provider: " + provider_id);
  }

  std::unordered_map<std::string, std::string> headers = {
      {"Accept", "application/json"},
      {"User-Agent", "GhostClaw/0.1"},
  };

  const auto api_key = resolve_api_key(config, provider_id);
  if (route->auth_mode == CatalogAuthMode::Bearer) {
    if (!api_key.has_value()) {
      return common::Result<std::vector<std::string>>::failure(
          "missing API key for provider: " + provider_id);
    }
    headers["Authorization"] = "Bearer " + *api_key;
  } else if (route->auth_mode == CatalogAuthMode::Anthropic) {
    if (!api_key.has_value()) {
      return common::Result<std::vector<std::string>>::failure(
          "missing API key for provider: " + provider_id);
    }
    headers["x-api-key"] = *api_key;
    headers["anthropic-version"] = "2023-06-01";
  }

  auto http = std::make_shared<CurlHttpClient>();
  const auto response = http->get(route->url, headers, MODEL_FETCH_TIMEOUT_MS);

  if (response.timeout) {
    return common::Result<std::vector<std::string>>::failure("model catalog request timed out");
  }
  if (response.network_error) {
    return common::Result<std::vector<std::string>>::failure("model catalog request failed: " +
                                                              response.network_error_message);
  }
  if (response.status < 200 || response.status >= 300) {
    return common::Result<std::vector<std::string>>::failure(
        "model catalog request failed with status " + std::to_string(response.status));
  }

  auto models = extract_models_from_response(provider_id, response.body);
  if (models.empty()) {
    return common::Result<std::vector<std::string>>::failure(
        "provider response does not include model identifiers");
  }
  return common::Result<std::vector<std::string>>::success(std::move(models));
}

} // namespace

const std::vector<ProviderInfo> &provider_catalog() {
  static const std::vector<ProviderInfo> catalog = {
      {.id = "openrouter", .display_name = "OpenRouter", .aliases = {}, .supports_model_catalog = true},
      {.id = "openai", .display_name = "OpenAI", .aliases = {}, .supports_model_catalog = true},
      {.id = "openai-codex", .display_name = "OpenAI Codex", .aliases = {}, .supports_model_catalog = true},
      {.id = "anthropic", .display_name = "Anthropic", .aliases = {}, .supports_model_catalog = true},
      {.id = "google", .display_name = "Google Gemini", .aliases = {}, .supports_model_catalog = true},
      {.id = "google-vertex", .display_name = "Google Vertex", .aliases = {}, .supports_model_catalog = true},
      {.id = "google-antigravity", .display_name = "Google Antigravity", .aliases = {}, .supports_model_catalog = false},
      {.id = "google-gemini-cli", .display_name = "Google Gemini CLI", .aliases = {}, .supports_model_catalog = false},
      {.id = "zai", .display_name = "Z AI", .aliases = {"z.ai", "z-ai"}, .supports_model_catalog = true},
      {.id = "glm", .display_name = "GLM", .aliases = {}, .supports_model_catalog = true},
      {.id = "xai", .display_name = "xAI", .aliases = {}, .supports_model_catalog = true},
      {.id = "grok", .display_name = "Grok", .aliases = {}, .supports_model_catalog = true},
      {.id = "groq", .display_name = "Groq", .aliases = {}, .supports_model_catalog = true},
      {.id = "cerebras", .display_name = "Cerebras", .aliases = {}, .supports_model_catalog = true},
      {.id = "mistral", .display_name = "Mistral", .aliases = {}, .supports_model_catalog = true},
      {.id = "huggingface", .display_name = "Hugging Face", .aliases = {}, .supports_model_catalog = true},
      {.id = "moonshot", .display_name = "Moonshot", .aliases = {}, .supports_model_catalog = true},
      {.id = "qwen-portal", .display_name = "Qwen Portal", .aliases = {}, .supports_model_catalog = true},
      {.id = "venice", .display_name = "Venice", .aliases = {}, .supports_model_catalog = true},
      {.id = "together", .display_name = "Together", .aliases = {}, .supports_model_catalog = true},
      {.id = "qianfan", .display_name = "Qianfan", .aliases = {}, .supports_model_catalog = true},
      {.id = "deepseek", .display_name = "DeepSeek", .aliases = {}, .supports_model_catalog = true},
      {.id = "fireworks", .display_name = "Fireworks", .aliases = {}, .supports_model_catalog = true},
      {.id = "perplexity", .display_name = "Perplexity", .aliases = {}, .supports_model_catalog = true},
      {.id = "cohere", .display_name = "Cohere", .aliases = {}, .supports_model_catalog = true},
      {.id = "nvidia", .display_name = "NVIDIA NIM", .aliases = {}, .supports_model_catalog = true},
      {.id = "github-copilot", .display_name = "GitHub Copilot", .aliases = {"copilot"}, .supports_model_catalog = true},
      {.id = "opencode", .display_name = "OpenCode", .aliases = {"opencode-zen"}, .supports_model_catalog = true},
      {.id = "minimax", .display_name = "MiniMax", .aliases = {}, .supports_model_catalog = true},
      {.id = "kimi-coding", .display_name = "Kimi Coding", .aliases = {"kimi-code"}, .supports_model_catalog = true},
      {.id = "xiaomi", .display_name = "Xiaomi MiLM", .aliases = {}, .supports_model_catalog = true},
      {.id = "ollama", .display_name = "Ollama", .aliases = {}, .local = true, .supports_model_catalog = true},
      {.id = "vllm", .display_name = "vLLM", .aliases = {}, .local = true, .supports_model_catalog = true},
      {.id = "litellm", .display_name = "LiteLLM", .aliases = {}, .local = true, .supports_model_catalog = true},
      {.id = "cloudflare", .display_name = "Cloudflare", .aliases = {}, .supports_model_catalog = true},
      {.id = "cloudflare-ai-gateway", .display_name = "Cloudflare AI Gateway", .aliases = {"cloudflare-ai"}, .supports_model_catalog = true},
      {.id = "vercel-ai-gateway", .display_name = "Vercel AI Gateway", .aliases = {}, .supports_model_catalog = true},
      {.id = "synthetic", .display_name = "Synthetic", .aliases = {}, .local = true, .supports_model_catalog = true},
  };
  return catalog;
}

std::optional<ProviderInfo> find_provider(const std::string &id_or_alias) {
  const std::string normalized = normalize_id(id_or_alias);
  for (const auto &entry : provider_catalog()) {
    if (entry.id == normalized) {
      return entry;
    }
    for (const auto &alias : entry.aliases) {
      if (normalize_id(alias) == normalized) {
        return entry;
      }
    }
  }
  return std::nullopt;
}

common::Result<ModelCatalog> refresh_model_catalog(const config::Config &config,
                                                   const std::string &provider,
                                                   const bool force_refresh) {
  const auto provider_info = find_provider(provider);
  if (!provider_info.has_value()) {
    return common::Result<ModelCatalog>::failure("unknown provider: " + provider);
  }

  const auto workspace = config::workspace_dir();
  if (!workspace.ok()) {
    return common::Result<ModelCatalog>::failure(workspace.error());
  }

  const std::string provider_id = provider_info->id;
  const std::filesystem::path cache_path = model_cache_path(workspace.value(), provider_id);

  const auto cached = read_cache(cache_path);
  if (!force_refresh && cached.has_value()) {
    const auto age = now_unix_seconds() - to_unix_seconds(cached->updated_at);
    if (age <= MODEL_CACHE_TTL_SECONDS) {
      ModelCatalog out;
      out.provider = provider_id;
      out.models = cached->models;
      out.updated_at = cached->updated_at;
      out.from_cache = true;
      return common::Result<ModelCatalog>::success(std::move(out));
    }
  }

  std::string live_error;
  auto live_models = fetch_live_models(config, provider_id);
  if (live_models.ok()) {
    const auto updated_at = std::chrono::system_clock::now();
    auto saved = write_cache(cache_path, live_models.value(), updated_at);
    if (!saved.ok()) {
      return common::Result<ModelCatalog>::failure(saved.error());
    }

    ModelCatalog out;
    out.provider = provider_id;
    out.models = std::move(live_models.value());
    out.updated_at = updated_at;
    out.from_cache = false;
    return common::Result<ModelCatalog>::success(std::move(out));
  }
  live_error = live_models.error();

  const auto models_it = builtin_model_catalogs().find(provider_id);
  if (models_it != builtin_model_catalogs().end() && !models_it->second.empty()) {
    auto models = models_it->second;
    std::sort(models.begin(), models.end());
    models.erase(std::unique(models.begin(), models.end()), models.end());

    const auto updated_at = std::chrono::system_clock::now();
    auto saved = write_cache(cache_path, models, updated_at);
    if (!saved.ok()) {
      return common::Result<ModelCatalog>::failure(saved.error());
    }

    ModelCatalog out;
    out.provider = provider_id;
    out.models = std::move(models);
    out.updated_at = updated_at;
    out.from_cache = false;
    return common::Result<ModelCatalog>::success(std::move(out));
  }

  if (cached.has_value()) {
    ModelCatalog out;
    out.provider = provider_id;
    out.models = cached->models;
    out.updated_at = cached->updated_at;
    out.from_cache = true;
    return common::Result<ModelCatalog>::success(std::move(out));
  }

  return common::Result<ModelCatalog>::failure(
      "provider does not expose a model catalog in this build: " + provider_id +
      " (live refresh failed: " + live_error + ")");
}

common::Result<std::vector<ModelCatalog>>
refresh_model_catalogs(const config::Config &config, const bool force_refresh) {
  std::vector<ModelCatalog> catalogs;
  for (const auto &provider : provider_catalog()) {
    if (!provider.supports_model_catalog) {
      continue;
    }
    auto refreshed = refresh_model_catalog(config, provider.id, force_refresh);
    if (!refreshed.ok()) {
      continue;
    }
    catalogs.push_back(std::move(refreshed.value()));
  }

  if (catalogs.empty()) {
    return common::Result<std::vector<ModelCatalog>>::failure("no model catalogs available");
  }

  std::sort(catalogs.begin(), catalogs.end(),
            [](const ModelCatalog &lhs, const ModelCatalog &rhs) {
              return lhs.provider < rhs.provider;
            });
  return common::Result<std::vector<ModelCatalog>>::success(std::move(catalogs));
}

} // namespace ghostclaw::providers
