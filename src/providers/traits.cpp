#include "ghostclaw/providers/traits.hpp"

#include "ghostclaw/common/fs.hpp"

#include <curl/curl.h>

#include <mutex>
#include <regex>
#include <sstream>

namespace ghostclaw::providers {

namespace {

size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
  const auto total = size * nmemb;
  auto *output = static_cast<std::string *>(userdata);
  output->append(ptr, total);
  return total;
}

struct StreamWriteContext {
  std::string *output = nullptr;
  const StreamChunkCallback *on_chunk = nullptr;
};

size_t stream_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
  const auto total = size * nmemb;
  auto *context = static_cast<StreamWriteContext *>(userdata);
  if (context->output != nullptr) {
    context->output->append(ptr, total);
  }
  if (context->on_chunk != nullptr && *context->on_chunk) {
    (*context->on_chunk)(std::string_view(ptr, total));
  }
  return total;
}

size_t header_callback(char *buffer, size_t size, size_t nitems, void *userdata) {
  const auto total = size * nitems;
  std::string header(buffer, total);
  auto *headers = static_cast<std::unordered_map<std::string, std::string> *>(userdata);

  const auto separator = header.find(':');
  if (separator != std::string::npos) {
    const std::string key = common::to_lower(common::trim(header.substr(0, separator)));
    const std::string value = common::trim(header.substr(separator + 1));
    (*headers)[key] = value;
  }

  return total;
}

HttpResponse execute_request(const std::string &url,
                             const std::unordered_map<std::string, std::string> &headers,
                             const std::optional<std::string> &body, const bool use_head,
                             const std::uint64_t timeout_ms,
                             const StreamChunkCallback *on_chunk = nullptr) {
  HttpResponse response;

  CURL *curl = curl_easy_init();
  if (curl == nullptr) {
    response.network_error = true;
    response.network_error_message = "curl_easy_init failed";
    return response;
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
  if (on_chunk != nullptr) {
    StreamWriteContext context{.output = &response.body, .on_chunk = on_chunk};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "GhostClaw/0.1");

    if (body.has_value()) {
      curl_easy_setopt(curl, CURLOPT_POST, 1L);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->c_str());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body->size()));
    }

    if (use_head) {
      curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    }

    struct curl_slist *header_list = nullptr;
    for (const auto &[key, value] : headers) {
      const std::string line = key + ": " + value;
      header_list = curl_slist_append(header_list, line.c_str());
    }
    if (header_list != nullptr) {
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    const CURLcode code = curl_easy_perform(curl);
    if (code != CURLE_OK) {
      response.network_error = true;
      response.network_error_message = curl_easy_strerror(code);
      response.timeout = code == CURLE_OPERATION_TIMEDOUT;
    } else {
      long status = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
      response.status = static_cast<std::uint16_t>(status);
    }

    if (header_list != nullptr) {
      curl_slist_free_all(header_list);
    }
    curl_easy_cleanup(curl);
    return response;
  }

  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "GhostClaw/0.1");

  if (body.has_value()) {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body->size()));
  }

  if (use_head) {
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  }

  struct curl_slist *header_list = nullptr;
  for (const auto &[key, value] : headers) {
    const std::string line = key + ": " + value;
    header_list = curl_slist_append(header_list, line.c_str());
  }
  if (header_list != nullptr) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
  }

  const CURLcode code = curl_easy_perform(curl);
  if (code != CURLE_OK) {
    response.network_error = true;
    response.network_error_message = curl_easy_strerror(code);
    response.timeout = code == CURLE_OPERATION_TIMEDOUT;
  } else {
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    response.status = static_cast<std::uint16_t>(status);
  }

  if (header_list != nullptr) {
    curl_slist_free_all(header_list);
  }
  curl_easy_cleanup(curl);

  return response;
}

std::string extract_json_string_after_key(const std::string &json, const std::string &key) {
  const std::size_t key_pos = json.find(key);
  if (key_pos == std::string::npos) {
    return "";
  }

  std::size_t colon = json.find(':', key_pos + key.size());
  if (colon == std::string::npos) {
    return "";
  }

  std::size_t begin = colon + 1;
  while (begin < json.size()) {
    const char ch = json[begin];
    if (ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t') {
      break;
    }
    ++begin;
  }
  if (begin >= json.size() || json[begin] != '"') {
    return "";
  }

  ++begin;
  std::string value;
  bool escaped = false;
  for (std::size_t i = begin; i < json.size(); ++i) {
    const char ch = json[i];
    if (escaped) {
      switch (ch) {
      case 'n':
        value.push_back('\n');
        break;
      case 't':
        value.push_back('\t');
        break;
      case 'r':
        value.push_back('\r');
        break;
      case '"':
      case '\\':
      case '/':
        value.push_back(ch);
        break;
      default:
        value.push_back(ch);
        break;
      }
      escaped = false;
      continue;
    }

    if (ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '"') {
      return value;
    }
    value.push_back(ch);
  }

  return "";
}

std::size_t skip_json_ws(const std::string &json, std::size_t index) {
  while (index < json.size()) {
    const char ch = json[index];
    if (ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t') {
      break;
    }
    ++index;
  }
  return index;
}

std::string extract_json_bracket_value_after_key(const std::string &json, const std::string &key,
                                                 const char open_bracket,
                                                 const char close_bracket) {
  const std::size_t key_pos = json.find(key);
  if (key_pos == std::string::npos) {
    return "";
  }

  std::size_t colon = json.find(':', key_pos + key.size());
  if (colon == std::string::npos) {
    return "";
  }
  colon = skip_json_ws(json, colon + 1);
  if (colon >= json.size() || json[colon] != open_bracket) {
    return "";
  }

  std::size_t depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (std::size_t i = colon; i < json.size(); ++i) {
    const char ch = json[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        in_string = false;
      }
      continue;
    }

    if (ch == '"') {
      in_string = true;
      continue;
    }
    if (ch == open_bracket) {
      ++depth;
      continue;
    }
    if (ch == close_bracket) {
      if (depth == 0) {
        return "";
      }
      --depth;
      if (depth == 0) {
        return json.substr(colon, i - colon + 1);
      }
    }
  }
  return "";
}

std::vector<std::string> extract_sse_data_events(const std::string &response) {
  std::vector<std::string> events;
  std::istringstream lines(response);
  std::string line;
  std::string current_data;

  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      if (!current_data.empty()) {
        events.push_back(current_data);
        current_data.clear();
      }
      continue;
    }
    if (!common::starts_with(line, "data:")) {
      continue;
    }
    std::string payload = line.substr(5);
    if (!payload.empty() && payload.front() == ' ') {
      payload.erase(payload.begin());
    }
    if (!current_data.empty()) {
      current_data.push_back('\n');
    }
    current_data += payload;
  }

  if (!current_data.empty()) {
    events.push_back(current_data);
  }

  return events;
}

} // namespace

std::string ProviderError::to_string() const {
  std::ostringstream stream;
  stream << "Provider error [";
  switch (code) {
  case ProviderErrorCode::ApiError:
    stream << "api";
    break;
  case ProviderErrorCode::NetworkError:
    stream << "network";
    break;
  case ProviderErrorCode::AuthError:
    stream << "auth";
    break;
  case ProviderErrorCode::RateLimitError:
    stream << "rate_limit";
    break;
  case ProviderErrorCode::ModelNotFound:
    stream << "model_not_found";
    break;
  case ProviderErrorCode::InvalidResponse:
    stream << "invalid_response";
    break;
  case ProviderErrorCode::Timeout:
    stream << "timeout";
    break;
  }
  stream << "]";
  if (status != 0) {
    stream << " status=" << status;
  }
  if (retry_after.has_value()) {
    stream << " retry_after=" << *retry_after;
  }
  if (!message.empty()) {
    stream << " " << message;
  }
  return stream.str();
}

CurlHttpClient::CurlHttpClient() {
  static std::once_flag curl_init_once;
  std::call_once(curl_init_once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

CurlHttpClient::~CurlHttpClient() = default;

HttpResponse CurlHttpClient::post_json(
    const std::string &url, const std::unordered_map<std::string, std::string> &headers,
    const std::string &body, const std::uint64_t timeout_ms) {
  return execute_request(url, headers, body, false, timeout_ms);
}

HttpResponse CurlHttpClient::post_json_stream(
    const std::string &url, const std::unordered_map<std::string, std::string> &headers,
    const std::string &body, const std::uint64_t timeout_ms, const StreamChunkCallback &on_chunk) {
  return execute_request(url, headers, body, false, timeout_ms, &on_chunk);
}

HttpResponse CurlHttpClient::get(const std::string &url,
                                 const std::unordered_map<std::string, std::string> &headers,
                                 const std::uint64_t timeout_ms) {
  return execute_request(url, headers, std::nullopt, false, timeout_ms);
}

HttpResponse CurlHttpClient::head(const std::string &url,
                                  const std::unordered_map<std::string, std::string> &headers,
                                  const std::uint64_t timeout_ms) {
  return execute_request(url, headers, std::nullopt, true, timeout_ms);
}

std::string json_escape(const std::string &value) {
  std::string escaped;
  escaped.reserve(value.size() + 8);
  for (const char ch : value) {
    switch (ch) {
    case '"':
      escaped += "\\\"";
      break;
    case '\\':
      escaped += "\\\\";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      escaped.push_back(ch);
      break;
    }
  }
  return escaped;
}

common::Result<std::string> parse_openai_content(const std::string &response) {
  if (response.find("\"choices\"") == std::string::npos) {
    return common::Result<std::string>::failure("choices field missing");
  }

  const std::string content = extract_json_string_after_key(response, "\"content\"");
  const bool has_empty_content = response.find("\"content\":\"\"") != std::string::npos ||
                                 response.find("\"content\": \"\"") != std::string::npos;
  const std::string tool_calls =
      extract_json_bracket_value_after_key(response, "\"tool_calls\"", '[', ']');
  if (content.empty() && !has_empty_content && tool_calls.empty()) {
    return common::Result<std::string>::failure("choices[0].message.content missing");
  }

  if (!tool_calls.empty()) {
    if (!content.empty()) {
      return common::Result<std::string>::success(content + "\n{\"tool_calls\":" + tool_calls +
                                                  "}");
    }
    return common::Result<std::string>::success("{\"tool_calls\":" + tool_calls + "}");
  }

  return common::Result<std::string>::success(content);
}

common::Result<std::string> parse_openai_sse_event_delta(const std::string &event_data) {
  if (common::trim(event_data) == "[DONE]") {
    return common::Result<std::string>::success("");
  }
  if (event_data.find("\"delta\"") == std::string::npos) {
    return common::Result<std::string>::success("");
  }
  const std::string delta = extract_json_string_after_key(event_data, "\"content\"");
  if (delta.empty() && event_data.find("\"content\":\"\"") == std::string::npos) {
    return common::Result<std::string>::success("");
  }
  return common::Result<std::string>::success(delta);
}

common::Result<std::string> parse_openai_sse_content(const std::string &response) {
  const auto events = extract_sse_data_events(response);
  if (events.empty()) {
    return common::Result<std::string>::failure("sse data missing");
  }

  std::string content;
  bool saw_done = false;
  for (const auto &event_data : events) {
    if (common::trim(event_data) == "[DONE]") {
      saw_done = true;
      break;
    }
    const auto delta = parse_openai_sse_event_delta(event_data);
    if (!delta.ok()) {
      return delta;
    }
    content += delta.value();
  }

  if (!saw_done && content.empty()) {
    return common::Result<std::string>::failure("no text deltas in SSE stream");
  }
  return common::Result<std::string>::success(content);
}

common::Result<std::string> parse_anthropic_content(const std::string &response) {
  if (response.find("\"content\"") == std::string::npos) {
    return common::Result<std::string>::failure("content field missing");
  }

  const std::string text = extract_json_string_after_key(response, "\"text\"");
  if (text.empty() && response.find("\"text\":\"\"") == std::string::npos) {
    return common::Result<std::string>::failure("content[0].text missing");
  }

  return common::Result<std::string>::success(text);
}

common::Result<std::string> parse_anthropic_sse_event_delta(const std::string &event_data) {
  if (event_data.find("\"content_block_delta\"") == std::string::npos &&
      event_data.find("\"text_delta\"") == std::string::npos) {
    return common::Result<std::string>::success("");
  }
  const std::string delta = extract_json_string_after_key(event_data, "\"text\"");
  if (delta.empty() && event_data.find("\"text\":\"\"") == std::string::npos) {
    return common::Result<std::string>::success("");
  }
  return common::Result<std::string>::success(delta);
}

common::Result<std::string> parse_anthropic_sse_content(const std::string &response) {
  const auto events = extract_sse_data_events(response);
  if (events.empty()) {
    return common::Result<std::string>::failure("sse data missing");
  }

  std::string content;
  for (const auto &event_data : events) {
    const auto delta = parse_anthropic_sse_event_delta(event_data);
    if (!delta.ok()) {
      return delta;
    }
    content += delta.value();
  }

  if (content.empty()) {
    return common::Result<std::string>::failure("no text deltas in SSE stream");
  }
  return common::Result<std::string>::success(content);
}

} // namespace ghostclaw::providers
