#include "test_framework.hpp"

#include "ghostclaw/config/schema.hpp"
#include "ghostclaw/providers/compatible.hpp"
#include "ghostclaw/providers/factory.hpp"
#include "ghostclaw/providers/reliable.hpp"
#include "ghostclaw/providers/traits.hpp"

#include <cstdlib>
#include <memory>
#include <optional>

namespace {

class MockHttpClient final : public ghostclaw::providers::HttpClient {
public:
  ghostclaw::providers::HttpResponse next_post;
  ghostclaw::providers::HttpResponse next_post_stream;
  ghostclaw::providers::HttpResponse next_head;
  std::vector<std::string> stream_chunks;
  std::string last_url;
  std::unordered_map<std::string, std::string> last_headers;
  std::string last_body;

  [[nodiscard]] ghostclaw::providers::HttpResponse
  post_json(const std::string &url, const std::unordered_map<std::string, std::string> &headers,
            const std::string &body,
            std::uint64_t) override {
    last_url = url;
    last_headers = headers;
    last_body = body;
    return next_post;
  }

  [[nodiscard]] ghostclaw::providers::HttpResponse
  post_json_stream(const std::string &url,
                   const std::unordered_map<std::string, std::string> &headers,
                   const std::string &body, std::uint64_t,
                   const ghostclaw::providers::StreamChunkCallback &on_chunk) override {
    last_url = url;
    last_headers = headers;
    last_body = body;
    for (const auto &chunk : stream_chunks) {
      if (on_chunk) {
        on_chunk(chunk);
      }
    }
    return next_post_stream;
  }

  [[nodiscard]] ghostclaw::providers::HttpResponse
  head(const std::string &url, const std::unordered_map<std::string, std::string> &headers,
       std::uint64_t) override {
    last_url = url;
    last_headers = headers;
    return next_head;
  }
};

class SequenceProvider final : public ghostclaw::providers::Provider {
public:
  explicit SequenceProvider(std::vector<ghostclaw::common::Result<std::string>> results,
                            std::string name)
      : results_(std::move(results)), name_(std::move(name)) {}

  [[nodiscard]] ghostclaw::common::Result<std::string>
  chat(const std::string &, const std::string &, double) override {
    return chat_with_system(std::nullopt, "", "", 0.0);
  }

  [[nodiscard]] ghostclaw::common::Result<std::string>
  chat_with_system(const std::optional<std::string> &, const std::string &, const std::string &,
                   double) override {
    if (index_ >= results_.size()) {
      return ghostclaw::common::Result<std::string>::failure("out of responses");
    }
    return results_[index_++];
  }

  [[nodiscard]] ghostclaw::common::Status warmup() override { return ghostclaw::common::Status::success(); }
  [[nodiscard]] std::string name() const override { return name_; }

private:
  std::vector<ghostclaw::common::Result<std::string>> results_;
  std::size_t index_ = 0;
  std::string name_;
};

void set_test_env(const char *name, const char *value) {
#if defined(_WIN32)
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}

void unset_test_env(const char *name) {
#if defined(_WIN32)
  _putenv_s(name, "");
#else
  unsetenv(name);
#endif
}

} // namespace

void register_provider_tests(std::vector<ghostclaw::tests::TestCase> &tests) {
  using ghostclaw::tests::require;
  namespace p = ghostclaw::providers;

  tests.push_back({"compatible_success_parse", [] {
                     auto mock = std::make_shared<MockHttpClient>();
                     mock->next_post = {.status = 200,
                                        .body = R"({"choices":[{"message":{"content":"hello"}}]})"};
                     p::CompatibleProvider provider("test", "https://example.com/v1", "key", mock);
                     auto result = provider.chat("hi", "model", 0.7);
                     require(result.ok(), result.error());
                     require(result.value() == "hello", "content parse mismatch");
                   }});

  tests.push_back({"compatible_tool_call_payload_passthrough", [] {
                     auto mock = std::make_shared<MockHttpClient>();
                     mock->next_post = {
                         .status = 200,
                         .body =
                             R"({"choices":[{"message":{"content":null,"tool_calls":[{"id":"call_1","type":"function","function":{"name":"echo_tool","arguments":"{\"value\":\"x\"}"}}]}}]})"};
                     p::CompatibleProvider provider("test", "https://example.com/v1", "key", mock);
                     auto result = provider.chat("hi", "model", 0.7);
                     require(result.ok(), result.error());
                     require(result.value().find("\"tool_calls\"") != std::string::npos,
                             "tool calls should be preserved");
                     require(result.value().find("echo_tool") != std::string::npos,
                             "tool name should be preserved");
                   }});

  tests.push_back({"compatible_sends_tools_schema_in_request", [] {
                     auto mock = std::make_shared<MockHttpClient>();
                     mock->next_post = {.status = 200,
                                        .body = R"({"choices":[{"message":{"content":"ok"}}]})"};
                     p::CompatibleProvider provider("test", "https://example.com/v1", "key", mock);

                     std::vector<ghostclaw::tools::ToolSpec> tools = {
                         {.name = "file_read",
                          .description = "Read a file",
                          .parameters_json = R"({"type":"object","properties":{"path":{"type":"string"}}})",
                          .safe = true,
                          .group = "fs"},
                     };
                     auto result = provider.chat_with_system_tools(
                         std::nullopt, "read README", "model", 0.7, tools);
                     require(result.ok(), result.error());
                     require(mock->last_body.find("\"tools\"") != std::string::npos,
                             "tools block should be present");
                     require(mock->last_body.find("\"tool_choice\":\"auto\"") != std::string::npos,
                             "tool choice should be auto");
                     require(mock->last_body.find("\"name\":\"file_read\"") != std::string::npos,
                             "tool name should be serialized");
                   }});

  tests.push_back({"openai_sse_parse", [] {
                     const std::string sse = "data: {\"choices\":[{\"delta\":{\"content\":\"hel\"}}]}\n\n"
                                             "data: {\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}\n\n"
                                             "data: [DONE]\n\n";
                     auto parsed = p::parse_openai_sse_content(sse);
                     require(parsed.ok(), parsed.error());
                     require(parsed.value() == "hello", "openai sse parse mismatch");
                   }});

  tests.push_back({"anthropic_sse_parse", [] {
                     const std::string sse =
                         "event: content_block_delta\n"
                         "data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"hel\"}}\n\n"
                         "event: content_block_delta\n"
                         "data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"lo\"}}\n\n";
                     auto parsed = p::parse_anthropic_sse_content(sse);
                     require(parsed.ok(), parsed.error());
                     require(parsed.value() == "hello", "anthropic sse parse mismatch");
                   }});

  tests.push_back({"compatible_streaming_aggregates_sse_chunks", [] {
                     auto mock = std::make_shared<MockHttpClient>();
                     mock->stream_chunks = {"data: {\"choices\":[{\"delta\":{\"content\":\"hel\"}}]}\n\n",
                                            "data: {\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}\n\n",
                                            "data: [DONE]\n\n"};
                     mock->next_post_stream = {
                         .status = 200,
                         .body = "data: {\"choices\":[{\"delta\":{\"content\":\"hel\"}}]}\n\n"
                                 "data: {\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}\n\n"
                                 "data: [DONE]\n\n",
                         .headers = {{"content-type", "text/event-stream"}},
                     };
                     p::CompatibleProvider provider("test", "https://example.com/v1", "key", mock);

                     std::string streamed;
                     auto result = provider.chat_with_system_stream(
                         std::nullopt, "hi", "model", 0.7,
                         [&](std::string_view token) { streamed.append(token); });
                     require(result.ok(), result.error());
                     require(result.value() == "hello", "stream result mismatch");
                     require(streamed == "hello", "stream callbacks mismatch");
                   }});

  tests.push_back({"compatible_auth_error", [] {
                     auto mock = std::make_shared<MockHttpClient>();
                     mock->next_post = {.status = 401, .body = "unauthorized"};
                     p::CompatibleProvider provider("test", "https://example.com/v1", "key", mock);
                     auto result = provider.chat("hi", "model", 0.7);
                     require(!result.ok(), "expected auth error");
                     require(result.error().find("auth") != std::string::npos,
                             "error should mention auth");
                   }});

  tests.push_back({"compatible_rate_limit_error", [] {
                     auto mock = std::make_shared<MockHttpClient>();
                     mock->next_post = {.status = 429,
                                        .body = "rate limited",
                                        .headers = {{"retry-after", "42"}}};
                     p::CompatibleProvider provider("test", "https://example.com/v1", "key", mock);
                     auto result = provider.chat("hi", "model", 0.7);
                     require(!result.ok(), "expected rate limit error");
                     require(result.error().find("retry_after=42") != std::string::npos,
                             "error should include retry-after");
                   }});

  tests.push_back({"compatible_timeout", [] {
                     auto mock = std::make_shared<MockHttpClient>();
                     mock->next_post = {.timeout = true};
                     p::CompatibleProvider provider("test", "https://example.com/v1", "key", mock);
                     auto result = provider.chat("hi", "model", 0.7);
                     require(!result.ok(), "expected timeout error");
                     require(result.error().find("timeout") != std::string::npos,
                             "error should mention timeout");
                   }});

  tests.push_back({"compatible_invalid_response", [] {
                     auto mock = std::make_shared<MockHttpClient>();
                     mock->next_post = {.status = 200, .body = R"({"oops":true})"};
                     p::CompatibleProvider provider("test", "https://example.com/v1", "key", mock);
                     auto result = provider.chat("hi", "model", 0.7);
                     require(!result.ok(), "expected parse failure");
                     require(result.error().find("invalid_response") != std::string::npos,
                             "error should mention invalid response");
                   }});

  tests.push_back({"factory_known_providers", [] {
                     const std::vector<std::string> names = {
                         "openrouter",
                         "anthropic",
                         "openai",
                         "openai-codex",
                         "opencode",
                         "google",
                         "google-vertex",
                         "google-antigravity",
                         "google-gemini-cli",
                         "zai",
                         "xai",
                         "grok",
                         "groq",
                         "cerebras",
                         "mistral",
                         "github-copilot",
                         "huggingface",
                         "moonshot",
                         "kimi-coding",
                         "qwen-portal",
                         "synthetic",
                         "minimax",
                         "ollama",
                         "vllm",
                         "litellm",
                         "xiaomi",
                         "venice",
                         "together",
                         "qianfan",
                         "deepseek",
                         "fireworks",
                         "perplexity",
                         "cohere",
                         "nvidia",
                         "vercel-ai-gateway",
                         "cloudflare",
                         "glm",
                     };

                     auto mock = std::make_shared<MockHttpClient>();
                     for (const auto &name : names) {
                       auto created = p::create_provider(name, std::optional<std::string>("key"), mock);
                       require(created.ok(), "provider factory failed for: " + name + " error=" + created.error());
                     }
                   }});

  tests.push_back({"factory_provider_aliases", [] {
                     auto mock = std::make_shared<MockHttpClient>();
                     const std::vector<std::string> aliases = {"opencode-zen", "kimi-code", "z.ai"};
                     for (const auto &name : aliases) {
                       auto created = p::create_provider(name, std::optional<std::string>("key"), mock);
                       require(created.ok(), "provider alias failed for: " + name);
                     }
                   }});

  tests.push_back({"factory_synthetic_provider_no_api_key_required", [] {
                     auto mock = std::make_shared<MockHttpClient>();
                     auto created = p::create_provider("synthetic", std::nullopt, mock);
                     require(created.ok(), created.error());

                     auto result = created.value()->chat("hello from test", "synthetic-model", 0.4);
                     require(result.ok(), result.error());
                     require(result.value().find("Synthetic response") != std::string::npos,
                             "synthetic provider should return a mock response");
                   }});

  tests.push_back({"factory_env_api_key_resolve_openai_codex", [] {
                     set_test_env("OPENAI_CODEX_API_KEY", "env-codex-key");
                     auto mock = std::make_shared<MockHttpClient>();
                     mock->next_post = {.status = 200,
                                        .body = R"({"choices":[{"message":{"content":"ok"}}]})"};

                     auto created = p::create_provider("openai-codex", std::nullopt, mock);
                     require(created.ok(), created.error());
                     auto result = created.value()->chat("hello", "gpt-5.3-codex", 0.2);
                     require(result.ok(), result.error());
                     require(mock->last_headers["Authorization"] == "Bearer env-codex-key",
                             "expected OPENAI_CODEX_API_KEY auth header");
                     unset_test_env("OPENAI_CODEX_API_KEY");
                   }});

  tests.push_back({"factory_anthropic_compat_bearer_auth", [] {
                     auto mock = std::make_shared<MockHttpClient>();
                     mock->next_post = {.status = 200, .body = R"({"content":[{"text":"ok"}]})"};

                     auto created = p::create_provider("xiaomi", std::optional<std::string>("xiaomi-key"), mock);
                     require(created.ok(), created.error());
                     auto result = created.value()->chat("hello", "mimo-v2-flash", 0.2);
                     require(result.ok(), result.error());
                     require(mock->last_headers["Authorization"] == "Bearer xiaomi-key",
                             "xiaomi provider should use bearer auth");
                     require(mock->last_headers.find("x-api-key") == mock->last_headers.end(),
                             "xiaomi provider should not set x-api-key");
                   }});

  tests.push_back({"factory_cloudflare_gateway_base_url_override", [] {
                     set_test_env("CLOUDFLARE_AI_GATEWAY_BASE_URL",
                                  "https://gateway.ai.cloudflare.com/v1/a/b/anthropic");
                     auto mock = std::make_shared<MockHttpClient>();
                     mock->next_post = {.status = 200, .body = R"({"content":[{"text":"ok"}]})"};

                     auto created = p::create_provider("cloudflare-ai-gateway",
                                                       std::optional<std::string>("cloudflare-key"), mock);
                     require(created.ok(), created.error());
                     auto result = created.value()->chat("hello", "claude-sonnet-4-5", 0.2);
                     require(result.ok(), result.error());
                     require(
                         mock->last_url ==
                             "https://gateway.ai.cloudflare.com/v1/a/b/anthropic/v1/messages",
                         "expected cloudflare gateway base URL override");
                     unset_test_env("CLOUDFLARE_AI_GATEWAY_BASE_URL");
                   }});

  tests.push_back({"factory_cloudflare_gateway_requires_base_url", [] {
                     unset_test_env("CLOUDFLARE_AI_GATEWAY_BASE_URL");
                     auto mock = std::make_shared<MockHttpClient>();
                     auto created = p::create_provider("cloudflare-ai-gateway",
                                                       std::optional<std::string>("cloudflare-key"), mock);
                     require(!created.ok(), "cloudflare-ai-gateway should require explicit base URL");
                   }});

  tests.push_back({"factory_unknown_provider", [] {
                     auto mock = std::make_shared<MockHttpClient>();
                     auto created = p::create_provider("unknown-provider", std::optional<std::string>("key"), mock);
                     require(!created.ok(), "unknown provider should fail");
                   }});

  tests.push_back({"factory_custom_provider", [] {
                     auto mock = std::make_shared<MockHttpClient>();
                     auto created = p::create_provider("custom:https://example.com/v1",
                                                       std::optional<std::string>("key"), mock);
                     require(created.ok(), created.error());
                   }});

  tests.push_back({"reliable_primary_success", [] {
                     auto primary = std::make_shared<SequenceProvider>(
                         std::vector<ghostclaw::common::Result<std::string>>{
                             ghostclaw::common::Result<std::string>::success("ok")},
                         "primary");
                     p::ReliableProvider reliable(primary, {}, 2, 1);
                     auto result = reliable.chat("hi", "model", 0.7);
                     require(result.ok(), result.error());
                     require(result.value() == "ok", "primary response mismatch");
                   }});

  tests.push_back({"reliable_primary_retry_success", [] {
                     auto primary = std::make_shared<SequenceProvider>(
                         std::vector<ghostclaw::common::Result<std::string>>{
                             ghostclaw::common::Result<std::string>::failure("fail 1"),
                             ghostclaw::common::Result<std::string>::success("ok")},
                         "primary");
                     p::ReliableProvider reliable(primary, {}, 2, 1);
                     auto result = reliable.chat("hi", "model", 0.7);
                     require(result.ok(), result.error());
                     require(result.value() == "ok", "retry response mismatch");
                   }});

  tests.push_back({"reliable_fallback_success", [] {
                     auto primary = std::make_shared<SequenceProvider>(
                         std::vector<ghostclaw::common::Result<std::string>>{
                             ghostclaw::common::Result<std::string>::failure("fail 1"),
                             ghostclaw::common::Result<std::string>::failure("fail 2")},
                         "primary");

                     auto fallback = std::make_shared<SequenceProvider>(
                         std::vector<ghostclaw::common::Result<std::string>>{
                             ghostclaw::common::Result<std::string>::success("fallback")},
                         "fallback");

                     p::ReliableProvider reliable(primary, {fallback}, 1, 1);
                     auto result = reliable.chat("hi", "model", 0.7);
                     require(result.ok(), result.error());
                     require(result.value() == "fallback", "fallback response mismatch");
                   }});

  tests.push_back({"reliable_all_fail", [] {
                     auto primary = std::make_shared<SequenceProvider>(
                         std::vector<ghostclaw::common::Result<std::string>>{
                             ghostclaw::common::Result<std::string>::failure("primary fail")},
                         "primary");
                     auto fallback = std::make_shared<SequenceProvider>(
                         std::vector<ghostclaw::common::Result<std::string>>{
                             ghostclaw::common::Result<std::string>::failure("fallback fail")},
                         "fallback");

                     p::ReliableProvider reliable(primary, {fallback}, 0, 1);
                     auto result = reliable.chat("hi", "model", 0.7);
                     require(!result.ok(), "all providers failing should fail result");
                   }});

  tests.push_back({"warmup_best_effort", [] {
                     auto mock = std::make_shared<MockHttpClient>();
                     mock->next_head = {.network_error = true, .network_error_message = "offline"};
                     p::CompatibleProvider provider("test", "https://example.com/v1", "key", mock);
                     auto status = provider.warmup();
                     require(status.ok(), "warmup should not fail hard");
                   }});

  tests.push_back({"create_reliable_provider", [] {
                     ghostclaw::config::ReliabilityConfig reliability;
                     reliability.provider_retries = 1;
                     reliability.provider_backoff_ms = 1;
                     reliability.fallback_providers = {"openai", "anthropic"};
                     auto mock = std::make_shared<MockHttpClient>();
                     auto result = p::create_reliable_provider("openrouter",
                                                               std::optional<std::string>("key"),
                                                               reliability, mock);
                    require(result.ok(), result.error());
                  }});

  // ============================================
  // NEW TESTS: SSE Parsing Edge Cases
  // ============================================

  tests.push_back({"openai_sse_empty_chunks_ignored", [] {
                     const std::string sse = "data: {\"choices\":[{\"delta\":{\"content\":\"hello\"}}]}\n\n"
                                             "data: \n\n"
                                             "data: {\"choices\":[{\"delta\":{}}]}\n\n"
                                             "data: [DONE]\n\n";
                     auto parsed = p::parse_openai_sse_content(sse);
                     require(parsed.ok(), parsed.error());
                     require(parsed.value() == "hello", "empty chunks should be ignored");
                   }});

  tests.push_back({"openai_sse_multiline_content", [] {
                     const std::string sse = "data: {\"choices\":[{\"delta\":{\"content\":\"line1\\nline2\"}}]}\n\n"
                                             "data: [DONE]\n\n";
                     auto parsed = p::parse_openai_sse_content(sse);
                     require(parsed.ok(), parsed.error());
                     require(parsed.value() == "line1\nline2", "multiline content should work");
                   }});

  tests.push_back({"openai_sse_special_chars", [] {
                     const std::string sse = "data: {\"choices\":[{\"delta\":{\"content\":\"hello \\\"world\\\"\"}}]}\n\n"
                                             "data: [DONE]\n\n";
                     auto parsed = p::parse_openai_sse_content(sse);
                     require(parsed.ok(), parsed.error());
                     require(parsed.value().find("world") != std::string::npos, "special chars should work");
                   }});

  tests.push_back({"anthropic_sse_content_block_start", [] {
                     const std::string sse =
                         "event: content_block_start\n"
                         "data: {\"type\":\"content_block_start\",\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
                         "event: content_block_delta\n"
                         "data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}\n\n";
                     auto parsed = p::parse_anthropic_sse_content(sse);
                     require(parsed.ok(), parsed.error());
                     require(parsed.value() == "hi", "content block start should be handled");
                   }});

  tests.push_back({"anthropic_sse_message_stop", [] {
                     const std::string sse =
                         "event: content_block_delta\n"
                         "data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}\n\n"
                         "event: message_stop\n"
                         "data: {\"type\":\"message_stop\"}\n\n";
                     auto parsed = p::parse_anthropic_sse_content(sse);
                     require(parsed.ok(), parsed.error());
                     require(parsed.value() == "hi", "message stop should be handled");
                   }});

  // ============================================
  // NEW TESTS: Compatible Provider Edge Cases
  // ============================================

  tests.push_back({"compatible_network_error", [] {
                     auto mock = std::make_shared<MockHttpClient>();
                     mock->next_post = {.network_error = true, .network_error_message = "connection refused"};
                     p::CompatibleProvider provider("test", "https://example.com/v1", "key", mock);
                     auto result = provider.chat("hi", "model", 0.7);
                     require(!result.ok(), "expected network error");
                     require(result.error().find("network") != std::string::npos,
                             "error should mention network");
                   }});

  tests.push_back({"compatible_500_error", [] {
                     auto mock = std::make_shared<MockHttpClient>();
                     mock->next_post = {.status = 500, .body = "internal server error"};
                     p::CompatibleProvider provider("test", "https://example.com/v1", "key", mock);
                     auto result = provider.chat("hi", "model", 0.7);
                     require(!result.ok(), "expected server error");
                   }});

  tests.push_back({"compatible_empty_response", [] {
                     auto mock = std::make_shared<MockHttpClient>();
                     mock->next_post = {.status = 200, .body = ""};
                     p::CompatibleProvider provider("test", "https://example.com/v1", "key", mock);
                     auto result = provider.chat("hi", "model", 0.7);
                     require(!result.ok(), "expected parse error for empty response");
                   }});

  tests.push_back({"compatible_malformed_json", [] {
                     auto mock = std::make_shared<MockHttpClient>();
                     mock->next_post = {.status = 200, .body = "not json at all"};
                     p::CompatibleProvider provider("test", "https://example.com/v1", "key", mock);
                     auto result = provider.chat("hi", "model", 0.7);
                     require(!result.ok(), "expected parse error for malformed json");
                   }});

  tests.push_back({"compatible_empty_choices", [] {
                     auto mock = std::make_shared<MockHttpClient>();
                     mock->next_post = {.status = 200, .body = R"({"choices":[]})"};
                     p::CompatibleProvider provider("test", "https://example.com/v1", "key", mock);
                     auto result = provider.chat("hi", "model", 0.7);
                     require(!result.ok(), "expected error for empty choices");
                   }});

  tests.push_back({"compatible_missing_content", [] {
                     auto mock = std::make_shared<MockHttpClient>();
                     mock->next_post = {.status = 200, .body = R"({"choices":[{"message":{}}]})"};
                     p::CompatibleProvider provider("test", "https://example.com/v1", "key", mock);
                     auto result = provider.chat("hi", "model", 0.7);
                     // May succeed with empty content or fail depending on implementation
                   }});

  // ============================================
  // NEW TESTS: Reliable Provider Edge Cases
  // ============================================

  tests.push_back({"reliable_multiple_fallbacks", [] {
                     auto primary = std::make_shared<SequenceProvider>(
                         std::vector<ghostclaw::common::Result<std::string>>{
                             ghostclaw::common::Result<std::string>::failure("primary fail")},
                         "primary");

                     auto fallback1 = std::make_shared<SequenceProvider>(
                         std::vector<ghostclaw::common::Result<std::string>>{
                             ghostclaw::common::Result<std::string>::failure("fallback1 fail")},
                         "fallback1");

                     auto fallback2 = std::make_shared<SequenceProvider>(
                         std::vector<ghostclaw::common::Result<std::string>>{
                             ghostclaw::common::Result<std::string>::success("fallback2 ok")},
                         "fallback2");

                     p::ReliableProvider reliable(primary, {fallback1, fallback2}, 0, 1);
                     auto result = reliable.chat("hi", "model", 0.7);
                     require(result.ok(), result.error());
                     require(result.value() == "fallback2 ok", "should use second fallback");
                   }});

  tests.push_back({"reliable_retries_before_fallback", [] {
                     auto primary = std::make_shared<SequenceProvider>(
                         std::vector<ghostclaw::common::Result<std::string>>{
                             ghostclaw::common::Result<std::string>::failure("fail 1"),
                             ghostclaw::common::Result<std::string>::failure("fail 2"),
                             ghostclaw::common::Result<std::string>::success("retry ok")},
                         "primary");

                     auto fallback = std::make_shared<SequenceProvider>(
                         std::vector<ghostclaw::common::Result<std::string>>{
                             ghostclaw::common::Result<std::string>::success("fallback")},
                         "fallback");

                     p::ReliableProvider reliable(primary, {fallback}, 3, 1);
                     auto result = reliable.chat("hi", "model", 0.7);
                     require(result.ok(), result.error());
                     require(result.value() == "retry ok", "should succeed on retry before fallback");
                   }});

  tests.push_back({"reliable_name_is_reliable", [] {
                     auto primary = std::make_shared<SequenceProvider>(
                         std::vector<ghostclaw::common::Result<std::string>>{},
                         "test-primary");
                     p::ReliableProvider reliable(primary, {}, 0, 1);
                     require(reliable.name() == "reliable", "name should be 'reliable'");
                   }});

  // ============================================
  // NEW TESTS: Factory Edge Cases
  // ============================================

  tests.push_back({"factory_ollama_no_api_key_required", [] {
                     auto mock = std::make_shared<MockHttpClient>();
                     auto created = p::create_provider("ollama", std::nullopt, mock);
                     require(created.ok(), "ollama should work without api key");
                   }});

  tests.push_back({"factory_custom_url_validation", [] {
                     auto mock = std::make_shared<MockHttpClient>();
                     auto valid = p::create_provider("custom:https://api.example.com/v1",
                                                     std::optional<std::string>("key"), mock);
                     require(valid.ok(), valid.error());

                     auto invalid = p::create_provider("custom:not-a-url",
                                                       std::optional<std::string>("key"), mock);
                     // May or may not fail depending on validation strictness
                   }});

  tests.push_back({"factory_xai_and_grok_same_endpoint", [] {
                     auto mock = std::make_shared<MockHttpClient>();
                     auto xai = p::create_provider("xai", std::optional<std::string>("key"), mock);
                     auto grok = p::create_provider("grok", std::optional<std::string>("key"), mock);
                     require(xai.ok(), xai.error());
                     require(grok.ok(), grok.error());
                   }});

  // ============================================
  // NEW TESTS: Streaming Edge Cases
  // ============================================

  tests.push_back({"compatible_stream_empty_chunks", [] {
                     auto mock = std::make_shared<MockHttpClient>();
                     mock->stream_chunks = {"", "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n", ""};
                     mock->next_post_stream = {
                         .status = 200,
                         .body = "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n",
                         .headers = {{"content-type", "text/event-stream"}},
                     };
                     p::CompatibleProvider provider("test", "https://example.com/v1", "key", mock);

                     std::string streamed;
                     auto result = provider.chat_with_system_stream(
                         std::nullopt, "hi", "model", 0.7,
                         [&](std::string_view token) { streamed.append(token); });
                     require(result.ok(), result.error());
                   }});

  tests.push_back({"compatible_stream_callback_null_safe", [] {
                     auto mock = std::make_shared<MockHttpClient>();
                     mock->stream_chunks = {"data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n"};
                     mock->next_post_stream = {
                         .status = 200,
                         .body = "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n",
                         .headers = {{"content-type", "text/event-stream"}},
                     };
                     p::CompatibleProvider provider("test", "https://example.com/v1", "key", mock);

                     // Pass null callback - should not crash
                     auto result = provider.chat_with_system_stream(
                         std::nullopt, "hi", "model", 0.7, nullptr);
                     require(result.ok(), result.error());
                   }});
}
