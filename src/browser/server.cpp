#include "ghostclaw/browser/server.hpp"

#include "ghostclaw/common/fs.hpp"
#include "ghostclaw/common/json_util.hpp"
#include "ghostclaw/providers/traits.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ghostclaw::browser {

namespace {

constexpr int kListenBacklog = 64;

std::string trim_copy(const std::string &value) { return common::trim(value); }

std::string json_escape(const std::string &value) { return common::json_escape(value); }

std::string json_string(const std::string &value) {
  return "\"" + common::json_escape(value) + "\"";
}

bool is_loopback_host(const std::string &host) {
  const std::string lowered = common::to_lower(common::trim(host));
  return lowered == "127.0.0.1" || lowered == "localhost" || lowered == "::1" ||
         lowered == "[::1]";
}

std::string normalize_bind_host(const std::string &host) {
  const std::string lowered = common::to_lower(common::trim(host));
  if (lowered == "localhost" || lowered == "::1" || lowered == "[::1]") {
    return "127.0.0.1";
  }
  return common::trim(host);
}

std::string status_text(const int status) {
  switch (status) {
  case 200:
    return "OK";
  case 201:
    return "Created";
  case 400:
    return "Bad Request";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 413:
    return "Payload Too Large";
  case 500:
    return "Internal Server Error";
  default:
    return "OK";
  }
}

BrowserHttpResponse make_json_response(const int status, const std::string &body) {
  BrowserHttpResponse response;
  response.status = status;
  response.content_type = "application/json";
  response.body = body;
  return response;
}

std::unordered_map<std::string, std::string> parse_query_string(const std::string &query) {
  std::unordered_map<std::string, std::string> out;
  std::stringstream stream(query);
  std::string part;
  while (std::getline(stream, part, '&')) {
    if (part.empty()) {
      continue;
    }
    const auto eq = part.find('=');
    if (eq == std::string::npos) {
      out[part] = "";
      continue;
    }
    out[part.substr(0, eq)] = part.substr(eq + 1);
  }
  return out;
}

common::Result<BrowserHttpRequest> parse_http_request(const std::string &raw) {
  const auto header_end = raw.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    return common::Result<BrowserHttpRequest>::failure("incomplete request");
  }

  const std::string headers_part = raw.substr(0, header_end);
  const std::string body = raw.substr(header_end + 4);

  std::istringstream head_stream(headers_part);
  std::string line;
  if (!std::getline(head_stream, line)) {
    return common::Result<BrowserHttpRequest>::failure("missing request line");
  }
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }

  BrowserHttpRequest request;
  std::string http_version;
  std::istringstream req_line(line);
  if (!(req_line >> request.method >> request.raw_path >> http_version)) {
    return common::Result<BrowserHttpRequest>::failure("invalid request line");
  }

  while (std::getline(head_stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    request.headers[common::to_lower(trim_copy(line.substr(0, colon)))] =
        trim_copy(line.substr(colon + 1));
  }

  request.body = body;
  const auto qpos = request.raw_path.find('?');
  if (qpos == std::string::npos) {
    request.path = request.raw_path;
  } else {
    request.path = request.raw_path.substr(0, qpos);
    request.query = parse_query_string(request.raw_path.substr(qpos + 1));
  }

  return common::Result<BrowserHttpRequest>::success(std::move(request));
}

std::string render_http_response(const BrowserHttpResponse &response) {
  std::ostringstream out;
  out << "HTTP/1.1 " << response.status << " " << status_text(response.status) << "\r\n";
  out << "Content-Type: " << response.content_type << "\r\n";
  out << "Content-Length: " << response.body.size() << "\r\n";
  out << "Connection: close\r\n";
  for (const auto &[k, v] : response.headers) {
    out << k << ": " << v << "\r\n";
  }
  out << "\r\n";
  out << response.body;
  return out.str();
}

std::string header_lookup(const BrowserHttpRequest &request, const std::string &key) {
  const auto it = request.headers.find(common::to_lower(key));
  if (it == request.headers.end()) {
    return "";
  }
  return it->second;
}

std::string find_json_string_field(const std::string &json, const std::string &field) {
  return common::json_get_string(json, field);
}

std::string find_json_array_field(const std::string &json, const std::string &field) {
  return common::json_get_array(json, field);
}

JsonMap parse_flat_json_object(const std::string &json) {
  return common::json_parse_flat(json);
}

common::Result<std::vector<std::string>> parse_json_object_array(const std::string &json) {
  if (json.size() < 2 || json.front() != '[' || json.back() != ']') {
    return common::Result<std::vector<std::string>>::failure("actions must be a JSON array");
  }
  std::vector<std::string> objects;
  std::size_t pos = 1;
  while (pos + 1 < json.size()) {
    while (pos < json.size() &&
           (std::isspace(static_cast<unsigned char>(json[pos])) != 0 || json[pos] == ',')) {
      ++pos;
    }
    if (pos >= json.size() || json[pos] == ']') {
      break;
    }
    if (json[pos] != '{') {
      return common::Result<std::vector<std::string>>::failure(
          "actions array entries must be objects");
    }
    const auto end = common::json_find_matching_token(json, pos, '{', '}');
    if (end == std::string::npos) {
      return common::Result<std::vector<std::string>>::failure(
          "invalid actions array object");
    }
    objects.push_back(json.substr(pos, end - pos + 1));
    pos = end + 1;
  }
  return common::Result<std::vector<std::string>>::success(std::move(objects));
}

common::Result<BrowserAction> parse_action_object(const std::string &json) {
  JsonMap fields = parse_flat_json_object(json);
  if (fields.empty()) {
    return common::Result<BrowserAction>::failure("invalid action object");
  }
  auto action_it = fields.find("action");
  if (action_it == fields.end() || common::trim(action_it->second).empty()) {
    action_it = fields.find("name");
  }
  if (action_it == fields.end() || common::trim(action_it->second).empty()) {
    return common::Result<BrowserAction>::failure("action field is required");
  }
  BrowserAction out;
  out.action = common::trim(action_it->second);
  fields.erase(action_it);
  out.params = std::move(fields);
  return common::Result<BrowserAction>::success(std::move(out));
}

int status_for_action_error(const std::string &message) {
  const std::string lowered = common::to_lower(common::trim(message));
  if (lowered.find("missing") != std::string::npos ||
      lowered.find("requires") != std::string::npos ||
      lowered.find("unsupported") != std::string::npos ||
      lowered.find("invalid") != std::string::npos) {
    return 400;
  }
  return 500;
}

std::string encode_json_map(const JsonMap &map) {
  std::ostringstream out;
  out << "{";
  bool first = true;
  for (const auto &[key, value] : map) {
    if (!first) {
      out << ",";
    }
    first = false;
    out << "\"" << json_escape(key) << "\":";
    const std::string trimmed = trim_copy(value);
    if (trimmed == "true" || trimmed == "false" || trimmed == "null") {
      out << trimmed;
    } else {
      bool numeric = !trimmed.empty();
      std::size_t start = (trimmed.front() == '-' || trimmed.front() == '+') ? 1 : 0;
      bool saw_digit = false;
      bool saw_dot = false;
      for (std::size_t i = start; i < trimmed.size(); ++i) {
        const char ch = trimmed[i];
        if (std::isdigit(static_cast<unsigned char>(ch)) != 0) {
          saw_digit = true;
          continue;
        }
        if (ch == '.' && !saw_dot) {
          saw_dot = true;
          continue;
        }
        numeric = false;
        break;
      }
      if (!saw_digit) {
        numeric = false;
      }
      if (numeric || trimmed.starts_with('{') || trimmed.starts_with('[')) {
        out << trimmed;
      } else {
        out << json_string(value);
      }
    }
  }
  out << "}";
  return out.str();
}

std::string encode_action_result(const BrowserActionResult &result) {
  std::ostringstream out;
  out << "{";
  out << "\"success\":" << (result.success ? "true" : "false");
  if (!result.data.empty()) {
    out << ",\"data\":" << encode_json_map(result.data);
  }
  if (!result.error.empty()) {
    out << ",\"error\":" << json_string(result.error);
  }
  out << "}";
  return out.str();
}

} // namespace

BrowserHttpServer::BrowserHttpServer(IBrowserActions &actions) : actions_(actions) {}

BrowserHttpServer::~BrowserHttpServer() { stop(); }

common::Status BrowserHttpServer::start(const BrowserServerOptions &options) {
#ifdef _WIN32
  (void)options;
  return common::Status::error("browser HTTP server is not implemented on Windows");
#else
  if (running_) {
    return common::Status::error("browser HTTP server already running");
  }
  const auto bind_status = validate_bind_address(options.host);
  if (!bind_status.ok()) {
    return bind_status;
  }
  options_ = options;
  if (options_.max_body_bytes == 0) {
    options_.max_body_bytes = 256 * 1024;
  }

  listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    return common::Status::error("failed to create browser listen socket");
  }
  int reuse = 1;
  (void)setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(options_.port);
  const std::string bind_host = normalize_bind_host(options_.host);
  if (inet_pton(AF_INET, bind_host.c_str(), &addr.sin_addr) != 1) {
    close(listen_fd_);
    listen_fd_ = -1;
    return common::Status::error("invalid browser bind host");
  }
  if (bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    const std::string msg = std::strerror(errno);
    close(listen_fd_);
    listen_fd_ = -1;
    return common::Status::error("browser bind failed: " + msg);
  }
  if (listen(listen_fd_, kListenBacklog) != 0) {
    const std::string msg = std::strerror(errno);
    close(listen_fd_);
    listen_fd_ = -1;
    return common::Status::error("browser listen failed: " + msg);
  }
  sockaddr_in actual{};
  socklen_t actual_len = sizeof(actual);
  if (getsockname(listen_fd_, reinterpret_cast<sockaddr *>(&actual), &actual_len) == 0) {
    bound_port_ = ntohs(actual.sin_port);
  } else {
    bound_port_ = options_.port;
  }
  running_ = true;
  accept_thread_ = std::thread([this]() { accept_loop(); });
  return common::Status::success();
#endif
}

void BrowserHttpServer::stop() {
#ifndef _WIN32
  if (!running_ && listen_fd_ < 0) {
    return;
  }
  running_ = false;
  if (listen_fd_ >= 0) {
    shutdown(listen_fd_, SHUT_RDWR);
    close(listen_fd_);
    listen_fd_ = -1;
  }
  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }
  bound_port_ = 0;
#endif
}

bool BrowserHttpServer::is_running() const { return running_.load(); }

std::uint16_t BrowserHttpServer::port() const { return bound_port_; }

common::Status BrowserHttpServer::validate_bind_address(const std::string &host) const {
  if (!is_loopback_host(host)) {
    return common::Status::error(
        "browser HTTP server only supports loopback bind addresses");
  }
  return common::Status::success();
}

BrowserHttpResponse BrowserHttpServer::dispatch_for_test(const BrowserHttpRequest &request) {
  if (request.method == "POST" && request.path == "/navigate") {
    return handle_navigate(request);
  }
  if (request.method == "POST" && request.path == "/screenshot") {
    return handle_screenshot(request);
  }
  if (request.method == "GET" && request.path == "/snapshot") {
    return handle_snapshot(request);
  }
  if (request.method == "GET" && request.path == "/read") {
    return handle_read(request);
  }
  if (request.method == "POST" && request.path == "/act") {
    return handle_act(request);
  }
  if (request.method == "GET" && request.path == "/tabs") {
    return handle_tabs_list(request);
  }
  if (request.method == "POST" && request.path == "/tabs/open") {
    return handle_tabs_open(request);
  }
  if (request.method == "DELETE" && request.path.rfind("/tabs/", 0) == 0) {
    return handle_tabs_close(request);
  }
  return make_json_response(404, R"({"error":"not found"})");
}

BrowserHttpResponse BrowserHttpServer::handle_navigate(const BrowserHttpRequest &request) {
  const std::string url = find_json_string_field(request.body, "url");
  if (url.empty()) {
    return make_json_response(400, R"({"error":"navigate requires url"})");
  }

  std::string tab_id = find_json_string_field(request.body, "tab_id");
  {
    std::lock_guard<std::mutex> lock(tabs_mutex_);
    if (!tab_id.empty()) {
      auto tab_it = tabs_.find(tab_id);
      if (tab_it == tabs_.end()) {
        return make_json_response(404, R"({"error":"tab not found"})");
      }
      active_tab_id_ = tab_it->first;
    } else {
      tab_id = ensure_active_tab_locked();
    }
    for (auto &[id, tab] : tabs_) {
      tab.active = (id == active_tab_id_);
    }
  }

  BrowserAction action;
  action.action = "navigate";
  action.params["url"] = url;
  auto result = actions_.execute(action);
  if (!result.ok()) {
    return make_json_response(status_for_action_error(result.error()),
                              "{\"error\":" + json_string(result.error()) + "}");
  }

  {
    std::lock_guard<std::mutex> lock(tabs_mutex_);
    auto tab_it = tabs_.find(tab_id);
    if (tab_it != tabs_.end()) {
      tab_it->second.url = url;
    }
  }

  std::ostringstream out;
  out << "{";
  out << "\"status\":\"ok\",";
  out << "\"tab_id\":" << json_string(tab_id) << ",";
  out << "\"url\":" << json_string(url) << ",";
  out << "\"result\":" << encode_action_result(result.value());
  out << "}";
  return make_json_response(200, out.str());
}

BrowserHttpResponse BrowserHttpServer::handle_screenshot(const BrowserHttpRequest &request) {
  std::string tab_id = find_json_string_field(request.body, "tab_id");
  {
    std::lock_guard<std::mutex> lock(tabs_mutex_);
    if (!tab_id.empty()) {
      if (!tabs_.contains(tab_id)) {
        return make_json_response(404, R"({"error":"tab not found"})");
      }
      active_tab_id_ = tab_id;
    } else {
      tab_id = ensure_active_tab_locked();
    }
  }

  BrowserAction action;
  action.action = "screenshot";
  const std::string format = find_json_string_field(request.body, "format");
  if (!format.empty()) {
    action.params["format"] = format;
  }
  auto result = actions_.execute(action);
  if (!result.ok()) {
    return make_json_response(status_for_action_error(result.error()),
                              "{\"error\":" + json_string(result.error()) + "}");
  }

  std::ostringstream out;
  out << "{";
  out << "\"status\":\"ok\",";
  out << "\"tab_id\":" << json_string(tab_id) << ",";
  out << "\"result\":" << encode_action_result(result.value());
  out << "}";
  return make_json_response(200, out.str());
}

BrowserHttpResponse BrowserHttpServer::handle_snapshot(const BrowserHttpRequest &request) {
  std::string tab_id;
  {
    std::lock_guard<std::mutex> lock(tabs_mutex_);
    tab_id = ensure_active_tab_locked();
  }

  BrowserAction action;
  action.action = "snapshot";
  // Forward query params to the action
  for (const auto &[key, val] : request.query) {
    action.params[key] = val;
  }
  action.params["tab_id"] = tab_id;
  auto result = actions_.execute(action);
  if (!result.ok()) {
    return make_json_response(status_for_action_error(result.error()),
                              "{\"error\":" + json_string(result.error()) + "}");
  }

  std::ostringstream out;
  out << "{";
  out << "\"status\":\"ok\",";
  out << "\"tab_id\":" << json_string(tab_id) << ",";
  out << "\"result\":" << encode_action_result(result.value());
  out << "}";
  return make_json_response(200, out.str());
}

BrowserHttpResponse BrowserHttpServer::handle_read(const BrowserHttpRequest &request) {
  (void)request;
  std::string tab_id;
  {
    std::lock_guard<std::mutex> lock(tabs_mutex_);
    tab_id = ensure_active_tab_locked();
  }

  BrowserAction action;
  action.action = "read";
  auto result = actions_.execute(action);
  if (!result.ok()) {
    return make_json_response(status_for_action_error(result.error()),
                              "{\"error\":" + json_string(result.error()) + "}");
  }

  std::ostringstream out;
  out << "{";
  out << "\"status\":\"ok\",";
  out << "\"tab_id\":" << json_string(tab_id) << ",";
  out << "\"result\":" << encode_action_result(result.value());
  out << "}";
  return make_json_response(200, out.str());
}

BrowserHttpResponse BrowserHttpServer::handle_act(const BrowserHttpRequest &request) {
  std::string explicit_tab = find_json_string_field(request.body, "tab_id");
  if (!explicit_tab.empty()) {
    std::lock_guard<std::mutex> lock(tabs_mutex_);
    if (!tabs_.contains(explicit_tab)) {
      return make_json_response(404, R"({"error":"tab not found"})");
    }
    active_tab_id_ = explicit_tab;
    for (auto &[id, tab] : tabs_) {
      tab.active = (id == active_tab_id_);
    }
  } else {
    std::lock_guard<std::mutex> lock(tabs_mutex_);
    (void)ensure_active_tab_locked();
  }

  const std::string actions_array = find_json_array_field(request.body, "actions");
  if (!actions_array.empty()) {
    auto parsed_array = parse_json_object_array(actions_array);
    if (!parsed_array.ok()) {
      return make_json_response(400,
                                "{\"error\":" + json_string(parsed_array.error()) + "}");
    }
    std::vector<BrowserAction> actions;
    actions.reserve(parsed_array.value().size());
    for (const auto &entry : parsed_array.value()) {
      auto parsed = parse_action_object(entry);
      if (!parsed.ok()) {
        return make_json_response(400, "{\"error\":" + json_string(parsed.error()) + "}");
      }
      actions.push_back(parsed.value());
    }
    auto batch = actions_.execute_batch(actions);
    if (!batch.ok()) {
      return make_json_response(status_for_action_error(batch.error()),
                                "{\"error\":" + json_string(batch.error()) + "}");
    }

    std::ostringstream out;
    out << "{";
    out << "\"status\":\"ok\",";
    out << "\"count\":" << batch.value().size() << ",";
    out << "\"results\":[";
    for (std::size_t i = 0; i < batch.value().size(); ++i) {
      if (i > 0) {
        out << ",";
      }
      out << encode_action_result(batch.value()[i]);
    }
    out << "]";
    out << "}";
    return make_json_response(200, out.str());
  }

  auto parsed_action = parse_action_object(request.body);
  if (!parsed_action.ok()) {
    return make_json_response(400,
                              "{\"error\":" + json_string(parsed_action.error()) + "}");
  }
  auto result = actions_.execute(parsed_action.value());
  if (!result.ok()) {
    return make_json_response(status_for_action_error(result.error()),
                              "{\"error\":" + json_string(result.error()) + "}");
  }
  return make_json_response(200, "{\"status\":\"ok\",\"result\":" +
                                     encode_action_result(result.value()) + "}");
}

BrowserHttpResponse BrowserHttpServer::handle_tabs_list(const BrowserHttpRequest &request) const {
  (void)request;
  std::vector<BrowserTabInfo> tabs;
  std::string active_tab;
  {
    std::lock_guard<std::mutex> lock(tabs_mutex_);
    active_tab = active_tab_id_;
    tabs.reserve(tabs_.size());
    for (const auto &[id, tab] : tabs_) {
      (void)id;
      tabs.push_back(tab);
    }
  }
  std::sort(tabs.begin(), tabs.end(),
            [](const BrowserTabInfo &a, const BrowserTabInfo &b) { return a.tab_id < b.tab_id; });

  std::ostringstream out;
  out << "{";
  out << "\"tabs\":[";
  for (std::size_t i = 0; i < tabs.size(); ++i) {
    if (i > 0) {
      out << ",";
    }
    out << "{";
    out << "\"id\":" << json_string(tabs[i].tab_id) << ",";
    out << "\"url\":" << json_string(tabs[i].url) << ",";
    out << "\"active\":" << (tabs[i].active ? "true" : "false");
    out << "}";
  }
  out << "]";
  if (!active_tab.empty()) {
    out << ",\"active_tab_id\":" << json_string(active_tab);
  }
  out << "}";
  return make_json_response(200, out.str());
}

BrowserHttpResponse BrowserHttpServer::handle_tabs_open(const BrowserHttpRequest &request) {
  const std::string requested_url = [&]() {
    const std::string body_url = find_json_string_field(request.body, "url");
    if (!body_url.empty()) {
      return body_url;
    }
    auto it = request.query.find("url");
    if (it != request.query.end()) {
      return it->second;
    }
    return std::string("about:blank");
  }();

  std::string tab_id;
  {
    std::lock_guard<std::mutex> lock(tabs_mutex_);
    for (auto &[id, tab] : tabs_) {
      (void)id;
      tab.active = false;
    }
    tab_id = "tab-" + std::to_string(next_tab_id_++);
    BrowserTabInfo tab;
    tab.tab_id = tab_id;
    tab.url = requested_url;
    tab.active = true;
    tabs_[tab_id] = tab;
    active_tab_id_ = tab_id;
  }

  if (!requested_url.empty() && requested_url != "about:blank") {
    BrowserAction action;
    action.action = "navigate";
    action.params["url"] = requested_url;
    auto navigate = actions_.execute(action);
    if (!navigate.ok()) {
      std::lock_guard<std::mutex> lock(tabs_mutex_);
      tabs_.erase(tab_id);
      if (tabs_.empty()) {
        active_tab_id_.clear();
      } else {
        active_tab_id_ = tabs_.begin()->first;
        tabs_[active_tab_id_].active = true;
      }
      return make_json_response(status_for_action_error(navigate.error()),
                                "{\"error\":" + json_string(navigate.error()) + "}");
    }
  }

  std::ostringstream out;
  out << "{";
  out << "\"status\":\"ok\",";
  out << "\"tab\":{";
  out << "\"id\":" << json_string(tab_id) << ",";
  out << "\"url\":" << json_string(requested_url) << ",";
  out << "\"active\":true";
  out << "}";
  out << "}";
  return make_json_response(200, out.str());
}

BrowserHttpResponse BrowserHttpServer::handle_tabs_close(const BrowserHttpRequest &request) {
  const std::string prefix = "/tabs/";
  if (request.path.size() <= prefix.size()) {
    return make_json_response(400, R"({"error":"tab id is required"})");
  }
  const std::string tab_id = request.path.substr(prefix.size());
  bool removed = false;
  std::string new_active;
  {
    std::lock_guard<std::mutex> lock(tabs_mutex_);
    const auto it = tabs_.find(tab_id);
    if (it != tabs_.end()) {
      const bool was_active = it->second.active;
      tabs_.erase(it);
      removed = true;
      if (tabs_.empty()) {
        active_tab_id_.clear();
      } else if (was_active || active_tab_id_ == tab_id) {
        active_tab_id_ = tabs_.begin()->first;
      }
      for (auto &[id, tab] : tabs_) {
        tab.active = (id == active_tab_id_);
      }
      new_active = active_tab_id_;
    }
  }
  if (!removed) {
    return make_json_response(404, R"({"error":"tab not found"})");
  }

  std::ostringstream out;
  out << "{";
  out << "\"status\":\"ok\",";
  out << "\"closed_tab_id\":" << json_string(tab_id);
  if (!new_active.empty()) {
    out << ",\"active_tab_id\":" << json_string(new_active);
  }
  out << "}";
  return make_json_response(200, out.str());
}

std::string BrowserHttpServer::ensure_active_tab_locked() {
  if (!active_tab_id_.empty() && tabs_.contains(active_tab_id_)) {
    for (auto &[id, tab] : tabs_) {
      tab.active = (id == active_tab_id_);
    }
    return active_tab_id_;
  }

  if (!tabs_.empty()) {
    active_tab_id_ = tabs_.begin()->first;
    for (auto &[id, tab] : tabs_) {
      tab.active = (id == active_tab_id_);
    }
    return active_tab_id_;
  }

  const std::string tab_id = "tab-" + std::to_string(next_tab_id_++);
  BrowserTabInfo tab;
  tab.tab_id = tab_id;
  tab.url = "about:blank";
  tab.active = true;
  tabs_[tab_id] = tab;
  active_tab_id_ = tab_id;
  return active_tab_id_;
}

void BrowserHttpServer::accept_loop() {
#ifndef _WIN32
  while (running_) {
    sockaddr_in client_addr{};
    socklen_t len = sizeof(client_addr);
    int client = accept(listen_fd_, reinterpret_cast<sockaddr *>(&client_addr), &len);
    if (client < 0) {
      if (!running_) {
        break;
      }
      continue;
    }
    handle_client(client);
    close(client);
  }
#endif
}

void BrowserHttpServer::handle_client(const int client_fd) {
#ifndef _WIN32
  std::string raw;
  raw.reserve(4096);
  std::array<char, 4096> buf{};

  std::size_t content_length = 0;
  bool header_parsed = false;
  while (raw.size() < (options_.max_body_bytes + 8192)) {
    const ssize_t n = recv(client_fd, buf.data(), buf.size(), 0);
    if (n <= 0) {
      break;
    }
    raw.append(buf.data(), static_cast<std::size_t>(n));

    if (!header_parsed) {
      const auto header_end = raw.find("\r\n\r\n");
      if (header_end != std::string::npos) {
        header_parsed = true;
        auto parsed = parse_http_request(raw.substr(0, header_end + 4));
        if (parsed.ok()) {
          const std::string cl = header_lookup(parsed.value(), "content-length");
          if (!cl.empty()) {
            try {
              content_length = static_cast<std::size_t>(std::stoull(cl));
            } catch (...) {
              content_length = 0;
            }
          }
        }
        if (content_length > options_.max_body_bytes) {
          const auto response =
              render_http_response(make_json_response(413, R"({"error":"request_too_large"})"));
          send(client_fd, response.data(), response.size(), 0);
          return;
        }
      }
    }

    if (header_parsed) {
      const auto header_end = raw.find("\r\n\r\n");
      if (header_end != std::string::npos && raw.size() >= header_end + 4 + content_length) {
        break;
      }
    }
  }

  auto parsed = parse_http_request(raw);
  BrowserHttpResponse response;
  if (!parsed.ok()) {
    response = make_json_response(400, R"({"error":"invalid_request"})");
  } else {
    response = dispatch_for_test(parsed.value());
  }
  const std::string text = render_http_response(response);
  send(client_fd, text.data(), text.size(), 0);
#else
  (void)client_fd;
#endif
}

} // namespace ghostclaw::browser
