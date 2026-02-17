#include "ghostclaw/providers/factory.hpp"

#include "ghostclaw/auth/oauth.hpp"
#include "ghostclaw/common/fs.hpp"
#include "ghostclaw/providers/anthropic.hpp"
#include "ghostclaw/providers/compatible.hpp"
#include "ghostclaw/providers/ollama.hpp"
#include "ghostclaw/providers/openai.hpp"
#include "ghostclaw/providers/openrouter.hpp"
#include "ghostclaw/providers/reliable.hpp"
#include "ghostclaw/providers/synthetic.hpp"

#include <cctype>
#include <cstdlib>
#include <unordered_map>
#include <vector>

namespace ghostclaw::providers {

namespace {

struct CompatibleRoute {
  CompatibleRoute() = default;
  CompatibleRoute(std::string base, const bool require_key,
                  std::unordered_map<std::string, std::string> headers = {})
      : base_url(std::move(base)), require_api_key(require_key), extra_headers(std::move(headers)) {}

  std::string base_url;
  bool require_api_key = true;
  std::unordered_map<std::string, std::string> extra_headers;
};

struct AnthropicRoute {
  AnthropicRoute() = default;
  AnthropicRoute(std::string base, const bool bearer_auth,
                 std::unordered_map<std::string, std::string> headers = {})
      : base_url(std::move(base)), use_bearer_auth(bearer_auth), extra_headers(std::move(headers)) {}

  std::string base_url;
  bool use_bearer_auth = false;
  std::unordered_map<std::string, std::string> extra_headers;
};

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

std::string normalize_provider_id(const std::string &name) {
  std::string normalized = common::to_lower(common::trim(name));
  if (normalized == "z.ai" || normalized == "z-ai") {
    return "zai";
  }
  if (normalized == "opencode-zen") {
    return "opencode";
  }
  if (normalized == "kimi-code") {
    return "kimi-coding";
  }
  if (normalized == "cloudflare-ai") {
    return "cloudflare-ai-gateway";
  }
  return normalized;
}

std::optional<std::string> resolve_env_api_key(const std::string &provider) {
  static const std::unordered_map<std::string, std::vector<const char *>> env_map = {
      {"openai", {"OPENAI_API_KEY"}},
      {"openai-codex", {"OPENAI_CODEX_API_KEY", "OPENAI_API_KEY"}},
      {"anthropic", {"ANTHROPIC_OAUTH_TOKEN", "ANTHROPIC_API_KEY"}},
      {"opencode", {"OPENCODE_API_KEY", "OPENCODE_ZEN_API_KEY"}},
      {"google", {"GEMINI_API_KEY"}},
      {"google-vertex", {"GOOGLE_VERTEX_API_KEY", "GEMINI_API_KEY"}},
      {"google-antigravity", {"GOOGLE_ANTIGRAVITY_API_KEY", "GEMINI_API_KEY"}},
      {"google-gemini-cli", {"GOOGLE_GEMINI_CLI_API_KEY", "GEMINI_API_KEY"}},
      {"zai", {"ZAI_API_KEY", "Z_AI_API_KEY"}},
      {"openrouter", {"OPENROUTER_API_KEY"}},
      {"vercel-ai-gateway", {"AI_GATEWAY_API_KEY"}},
      {"cloudflare-ai-gateway", {"CLOUDFLARE_AI_GATEWAY_API_KEY"}},
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
      {"synthetic", {"SYNTHETIC_API_KEY"}},
      {"minimax", {"MINIMAX_API_KEY"}},
      {"ollama", {"OLLAMA_API_KEY"}},
      {"vllm", {"VLLM_API_KEY"}},
      {"litellm", {"LITELLM_API_KEY"}},
      {"xiaomi", {"XIAOMI_API_KEY"}},
      {"venice", {"VENICE_API_KEY"}},
      {"together", {"TOGETHER_API_KEY"}},
      {"qianfan", {"QIANFAN_API_KEY"}},
      {"deepseek", {"DEEPSEEK_API_KEY"}},
      {"fireworks", {"FIREWORKS_API_KEY"}},
      {"perplexity", {"PERPLEXITY_API_KEY"}},
      {"cohere", {"COHERE_API_KEY"}},
      {"nvidia", {"NVIDIA_API_KEY"}},
      {"cloudflare", {"CLOUDFLARE_API_KEY", "CLOUDFLARE_API_TOKEN"}},
  };

  const auto it = env_map.find(provider);
  if (it == env_map.end()) {
    return std::nullopt;
  }
  return read_first_env(it->second);
}

std::optional<std::string> resolve_api_key(const std::string &provider,
                                           const std::optional<std::string> &api_key) {
  if (api_key.has_value()) {
    const std::string trimmed = common::trim(*api_key);
    if (!trimmed.empty()) {
      return trimmed;
    }
  }
  return resolve_env_api_key(provider);
}

common::Result<std::shared_ptr<Provider>> make_compatible(
    const std::string &name, const std::string &base_url, const std::optional<std::string> &api_key,
    const std::shared_ptr<HttpClient> &http_client, const bool require_api_key = true,
    std::unordered_map<std::string, std::string> extra_headers = {}) {
  return common::Result<std::shared_ptr<Provider>>::success(std::make_shared<CompatibleProvider>(
      name, base_url, api_key.value_or(""), http_client, require_api_key, std::move(extra_headers)));
}

common::Result<std::shared_ptr<Provider>> make_anthropic(
    const std::string &name, const std::string &base_url, const std::optional<std::string> &api_key,
    const std::shared_ptr<HttpClient> &http_client, const bool use_bearer_auth = false,
    std::unordered_map<std::string, std::string> extra_headers = {}) {
  return common::Result<std::shared_ptr<Provider>>::success(std::make_shared<AnthropicProvider>(
      name, api_key.value_or(""), base_url, http_client, use_bearer_auth, std::move(extra_headers)));
}

} // namespace

common::Result<std::shared_ptr<Provider>>
create_provider(const std::string &name, const std::optional<std::string> &api_key,
                std::shared_ptr<HttpClient> http_client) {
  const std::string normalized = normalize_provider_id(name);
  auto resolved_key = resolve_api_key(normalized, api_key);

  // Fallback to ChatGPT OAuth tokens for OpenAI providers when no API key found
  if (!resolved_key.has_value() &&
      (normalized == "openai" || normalized == "openai-codex")) {
    if (auth::has_valid_tokens()) {
      auto token = auth::get_valid_access_token(*http_client);
      if (token.ok()) {
        resolved_key = token.value();
      }
    }
  }

  if (normalized == "openrouter") {
    return common::Result<std::shared_ptr<Provider>>::success(
        std::make_shared<OpenRouterProvider>(resolved_key.value_or(""), http_client));
  }
  if (normalized == "anthropic") {
    return common::Result<std::shared_ptr<Provider>>::success(
        std::make_shared<AnthropicProvider>(resolved_key.value_or(""), http_client));
  }
  if (normalized == "openai") {
    return common::Result<std::shared_ptr<Provider>>::success(
        std::make_shared<OpenAiProvider>(resolved_key.value_or(""), http_client));
  }
  if (normalized == "ollama") {
    return common::Result<std::shared_ptr<Provider>>::success(
        std::make_shared<OllamaProvider>(http_client));
  }
  if (normalized == "synthetic") {
    return common::Result<std::shared_ptr<Provider>>::success(
        std::make_shared<SyntheticProvider>());
  }

  static const std::unordered_map<std::string, CompatibleRoute> compatible_routes = {
      {"openai-codex", {"https://api.openai.com/v1", true}},
      {"opencode", {"https://opencode.ai/zen/v1", true}},
      {"google", {"https://generativelanguage.googleapis.com/v1beta/openai", true}},
      {"google-vertex", {"https://generativelanguage.googleapis.com/v1beta/openai", true}},
      {"google-antigravity", {"https://generativelanguage.googleapis.com/v1beta/openai", true}},
      {"google-gemini-cli", {"https://generativelanguage.googleapis.com/v1beta/openai", true}},
      {"zai", {"https://api.z.ai/api/paas/v4", true}},
      {"glm", {"https://open.bigmodel.cn/api/paas/v4", true}},
      {"xai", {"https://api.x.ai/v1", true}},
      {"grok", {"https://api.x.ai/v1", true}},
      {"groq", {"https://api.groq.com/openai/v1", true}},
      {"cerebras", {"https://api.cerebras.ai/v1", true}},
      {"mistral", {"https://api.mistral.ai/v1", true}},
      {"huggingface", {"https://router.huggingface.co/v1", true}},
      {"moonshot", {"https://api.moonshot.ai/v1", true}},
      {"qwen-portal", {"https://portal.qwen.ai/v1", true}},
      {"venice", {"https://api.venice.ai/api/v1", true}},
      {"together", {"https://api.together.xyz/v1", true}},
      {"qianfan", {"https://qianfan.baidubce.com/v2", true}},
      {"deepseek", {"https://api.deepseek.com/v1", true}},
      {"fireworks", {"https://api.fireworks.ai/inference/v1", true}},
      {"perplexity", {"https://api.perplexity.ai", true}},
      {"cohere", {"https://api.cohere.ai/v1", true}},
      {"nvidia", {"https://integrate.api.nvidia.com/v1", true}},
      {"github-copilot", {"https://api.githubcopilot.com", true}},
      {"vllm", {"http://127.0.0.1:8000/v1", false}},
      {"litellm", {"http://localhost:4000", false}},
      {"cloudflare", {"https://api.cloudflare.com/client/v4/accounts/{account_id}/ai/v1", true}},
  };

  static const std::unordered_map<std::string, AnthropicRoute> anthropic_routes = {
      {"minimax", {"https://api.minimax.io/anthropic", false}},
      {"xiaomi", {"https://api.xiaomimimo.com/anthropic", true}},
      {"kimi-coding", {"https://api.moonshot.ai/anthropic", false}},
      {"vercel-ai-gateway", {"https://ai-gateway.vercel.sh", false}},
      {"cloudflare-ai-gateway",
       {"https://gateway.ai.cloudflare.com/v1/<account_id>/<gateway_id>/anthropic", false}},
  };

  const auto compatible_it = compatible_routes.find(normalized);
  if (compatible_it != compatible_routes.end()) {
    const auto &route = compatible_it->second;
    const std::string base_url = resolve_base_url(normalized, route.base_url);
    return make_compatible(normalized, base_url, resolved_key, http_client, route.require_api_key,
                           route.extra_headers);
  }

  const auto anthropic_it = anthropic_routes.find(normalized);
  if (anthropic_it != anthropic_routes.end()) {
    const auto &route = anthropic_it->second;
    const std::string base_url = resolve_base_url(normalized, route.base_url);
    if (normalized == "cloudflare-ai-gateway" &&
        (base_url.find("<account_id>") != std::string::npos ||
         base_url.find("<gateway_id>") != std::string::npos)) {
      return common::Result<std::shared_ptr<Provider>>::failure(
          "cloudflare-ai-gateway requires CLOUDFLARE_AI_GATEWAY_BASE_URL "
          "(for example https://gateway.ai.cloudflare.com/v1/<account>/<gateway>/anthropic)");
    }
    return make_anthropic(normalized, base_url, resolved_key, http_client, route.use_bearer_auth,
                          route.extra_headers);
  }

  const std::string trimmed_name = common::trim(name);
  if (common::starts_with(common::to_lower(trimmed_name), "custom:")) {
    const std::string url = common::trim(trimmed_name.substr(7));
    if (url.empty() || (!common::starts_with(url, "http://") && !common::starts_with(url, "https://"))) {
      return common::Result<std::shared_ptr<Provider>>::failure(
          "Custom provider requires URL format custom:https://...");
    }
    return make_compatible("custom", url, resolved_key, http_client, true);
  }

  return common::Result<std::shared_ptr<Provider>>::failure("Unknown provider: " + name);
}

common::Result<std::shared_ptr<Provider>> create_reliable_provider(
    const std::string &primary_name, const std::optional<std::string> &api_key,
    const config::ReliabilityConfig &reliability, std::shared_ptr<HttpClient> http_client) {
  const auto primary = create_provider(primary_name, api_key, http_client);
  if (!primary.ok()) {
    return common::Result<std::shared_ptr<Provider>>::failure(primary.error());
  }

  std::vector<std::shared_ptr<Provider>> fallbacks;
  for (const auto &fallback_name : reliability.fallback_providers) {
    if (normalize_provider_id(fallback_name) == normalize_provider_id(primary_name)) {
      continue;
    }

    const auto fallback = create_provider(fallback_name, std::nullopt, http_client);
    if (fallback.ok()) {
      fallbacks.push_back(fallback.value());
    }
  }

  auto reliable = std::make_shared<ReliableProvider>(primary.value(), fallbacks,
                                                     reliability.provider_retries,
                                                     reliability.provider_backoff_ms);
  return common::Result<std::shared_ptr<Provider>>::success(std::move(reliable));
}

} // namespace ghostclaw::providers
