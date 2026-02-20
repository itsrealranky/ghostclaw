#pragma once

#include "ghostclaw/common/result.hpp"
#include "ghostclaw/tools/tool.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ghostclaw::providers {

enum class ProviderErrorCode {
  ApiError,
  NetworkError,
  AuthError,
  RateLimitError,
  ModelNotFound,
  InvalidResponse,
  Timeout,
};

struct ProviderError {
  ProviderErrorCode code = ProviderErrorCode::ApiError;
  std::uint16_t status = 0;
  std::string message;
  std::optional<std::uint64_t> retry_after;

  [[nodiscard]] std::string to_string() const;
};

struct ChatMessage {
  std::string role;
  std::string content;
};

struct ChatRequest {
  std::string model;
  std::vector<ChatMessage> messages;
  double temperature = 0.7;
  std::optional<std::uint32_t> max_tokens;
};

struct HttpResponse {
  std::uint16_t status = 0;
  std::string body;
  std::unordered_map<std::string, std::string> headers;
  bool timeout = false;
  bool network_error = false;
  std::string network_error_message;
};

using StreamChunkCallback = std::function<void(std::string_view)>;

class HttpClient {
public:
  virtual ~HttpClient() = default;
  [[nodiscard]] virtual HttpResponse
  post_json(const std::string &url, const std::unordered_map<std::string, std::string> &headers,
            const std::string &body, std::uint64_t timeout_ms) = 0;
  [[nodiscard]] virtual HttpResponse
  post_json_stream(const std::string &url,
                   const std::unordered_map<std::string, std::string> &headers,
                   const std::string &body, std::uint64_t timeout_ms,
                   const StreamChunkCallback &on_chunk) = 0;
  [[nodiscard]] virtual HttpResponse
  get(const std::string &url, const std::unordered_map<std::string, std::string> &headers,
      std::uint64_t timeout_ms) {
    (void)url;
    (void)headers;
    (void)timeout_ms;
    HttpResponse response;
    response.network_error = true;
    response.network_error_message = "GET not implemented";
    return response;
  }
  [[nodiscard]] virtual HttpResponse
  head(const std::string &url, const std::unordered_map<std::string, std::string> &headers,
       std::uint64_t timeout_ms) = 0;
};

class CurlHttpClient final : public HttpClient {
public:
  CurlHttpClient();
  ~CurlHttpClient() override;

  [[nodiscard]] HttpResponse
  post_json(const std::string &url, const std::unordered_map<std::string, std::string> &headers,
            const std::string &body, std::uint64_t timeout_ms) override;
  [[nodiscard]] HttpResponse
  post_json_stream(const std::string &url,
                   const std::unordered_map<std::string, std::string> &headers,
                   const std::string &body, std::uint64_t timeout_ms,
                   const StreamChunkCallback &on_chunk) override;
  [[nodiscard]] HttpResponse
  get(const std::string &url, const std::unordered_map<std::string, std::string> &headers,
      std::uint64_t timeout_ms) override;
  [[nodiscard]] HttpResponse
  head(const std::string &url, const std::unordered_map<std::string, std::string> &headers,
       std::uint64_t timeout_ms) override;
};

class Provider {
public:
  virtual ~Provider() = default;

  [[nodiscard]] virtual common::Result<std::string>
  chat(const std::string &message, const std::string &model, double temperature) = 0;

  [[nodiscard]] virtual common::Result<std::string>
  chat_with_system(const std::optional<std::string> &system_prompt, const std::string &message,
                   const std::string &model, double temperature) = 0;

  [[nodiscard]] virtual common::Result<std::string> chat_with_system_tools(
      const std::optional<std::string> &system_prompt, const std::string &message,
      const std::string &model, double temperature,
      const std::vector<tools::ToolSpec> &tools) {
    (void)tools;
    return chat_with_system(system_prompt, message, model, temperature);
  }

  [[nodiscard]] virtual common::Result<std::string>
  chat_stream(const std::string &message, const std::string &model, double temperature,
              const StreamChunkCallback &on_chunk) {
    return chat_with_system_stream(std::nullopt, message, model, temperature, on_chunk);
  }
  [[nodiscard]] virtual common::Result<std::string>
  chat_with_system_stream(const std::optional<std::string> &system_prompt,
                          const std::string &message, const std::string &model,
                          double temperature, const StreamChunkCallback &on_chunk) {
    auto result = chat_with_system(system_prompt, message, model, temperature);
    if (!result.ok()) {
      return result;
    }
    if (on_chunk) {
      std::istringstream stream(result.value());
      std::string token;
      bool emitted = false;
      while (stream >> token) {
        on_chunk(token);
        emitted = true;
      }
      if (!emitted && !result.value().empty()) {
        on_chunk(result.value());
      }
    }
    return result;
  }

  [[nodiscard]] virtual common::Status warmup() = 0;
  [[nodiscard]] virtual std::string name() const = 0;
};

[[nodiscard]] std::string json_escape(const std::string &value);
[[nodiscard]] common::Result<std::string> parse_openai_content(const std::string &response);
[[nodiscard]] common::Result<std::string> parse_anthropic_content(const std::string &response);
[[nodiscard]] common::Result<std::string> parse_openai_sse_event_delta(const std::string &event_data);
[[nodiscard]] common::Result<std::string> parse_openai_sse_content(const std::string &response);
[[nodiscard]] common::Result<std::string> parse_anthropic_sse_event_delta(const std::string &event_data);
[[nodiscard]] common::Result<std::string> parse_anthropic_sse_content(const std::string &response);

} // namespace ghostclaw::providers
