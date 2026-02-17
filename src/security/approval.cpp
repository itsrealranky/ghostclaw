#include "ghostclaw/security/approval.hpp"

#include "ghostclaw/common/fs.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <poll.h>
#include <regex>
#include <sstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace ghostclaw::security {

namespace {

std::string normalize_text(const std::string &value) {
  return common::to_lower(common::trim(value));
}

std::string first_token(const std::string &command) {
  const std::string trimmed = common::trim(command);
  if (trimmed.empty()) {
    return "";
  }
  const auto pos = trimmed.find(' ');
  const std::string token = pos == std::string::npos ? trimmed : trimmed.substr(0, pos);
  const auto slash = token.find_last_of('/');
  return normalize_text(slash == std::string::npos ? token : token.substr(slash + 1));
}

bool has_glob(std::string_view pattern) {
  return pattern.find('*') != std::string_view::npos || pattern.find('?') != std::string_view::npos;
}

std::string glob_to_regex(std::string_view pattern) {
  std::string out = "^";
  out.reserve(pattern.size() * 2 + 2);
  for (char ch : pattern) {
    switch (ch) {
    case '*':
      out += ".*";
      break;
    case '?':
      out += '.';
      break;
    case '.':
    case '+':
    case '^':
    case '$':
    case '(': case ')':
    case '[': case ']':
    case '{': case '}':
    case '|':
    case '\\':
      out += '\\';
      out += ch;
      break;
    default:
      out += ch;
      break;
    }
  }
  out += '$';
  return out;
}

std::string sanitize_protocol_token(std::string value) {
  for (char &ch : value) {
    if (ch == '\t' || ch == '\n' || ch == '\r') {
      ch = ' ';
    }
  }
  return value;
}

ApprovalDecision fail_safe_decision() { return ApprovalDecision::Deny; }

} // namespace

std::string exec_security_to_string(const ExecSecurity value) {
  switch (value) {
  case ExecSecurity::Deny:
    return "deny";
  case ExecSecurity::Allowlist:
    return "allowlist";
  case ExecSecurity::Full:
    return "full";
  }
  return "deny";
}

std::string exec_ask_to_string(const ExecAsk value) {
  switch (value) {
  case ExecAsk::Off:
    return "off";
  case ExecAsk::OnMiss:
    return "on-miss";
  case ExecAsk::Always:
    return "always";
  }
  return "off";
}

std::string approval_decision_to_string(const ApprovalDecision value) {
  switch (value) {
  case ApprovalDecision::AllowOnce:
    return "allow-once";
  case ApprovalDecision::AllowAlways:
    return "allow-always";
  case ApprovalDecision::Deny:
    return "deny";
  }
  return "deny";
}

common::Result<ExecSecurity> exec_security_from_string(const std::string &value) {
  const std::string normalized = normalize_text(value);
  if (normalized == "deny") {
    return common::Result<ExecSecurity>::success(ExecSecurity::Deny);
  }
  if (normalized == "allowlist") {
    return common::Result<ExecSecurity>::success(ExecSecurity::Allowlist);
  }
  if (normalized == "full") {
    return common::Result<ExecSecurity>::success(ExecSecurity::Full);
  }
  return common::Result<ExecSecurity>::failure("unknown ExecSecurity: " + value);
}

common::Result<ExecAsk> exec_ask_from_string(const std::string &value) {
  const std::string normalized = normalize_text(value);
  if (normalized == "off") {
    return common::Result<ExecAsk>::success(ExecAsk::Off);
  }
  if (normalized == "on-miss" || normalized == "on_miss" || normalized == "onmiss") {
    return common::Result<ExecAsk>::success(ExecAsk::OnMiss);
  }
  if (normalized == "always") {
    return common::Result<ExecAsk>::success(ExecAsk::Always);
  }
  return common::Result<ExecAsk>::failure("unknown ExecAsk: " + value);
}

common::Result<ApprovalDecision> approval_decision_from_string(const std::string &value) {
  const std::string normalized = normalize_text(value);
  if (normalized == "allow-once" || normalized == "allow_once") {
    return common::Result<ApprovalDecision>::success(ApprovalDecision::AllowOnce);
  }
  if (normalized == "allow-always" || normalized == "allow_always") {
    return common::Result<ApprovalDecision>::success(ApprovalDecision::AllowAlways);
  }
  if (normalized == "deny") {
    return common::Result<ApprovalDecision>::success(ApprovalDecision::Deny);
  }
  return common::Result<ApprovalDecision>::failure("unknown ApprovalDecision: " + value);
}

ApprovalStore::ApprovalStore(std::filesystem::path path)
    : path_(common::expand_path(path.string())) {}

common::Status ApprovalStore::load() {
  std::lock_guard<std::mutex> lock(mutex_);

  entries_.clear();
  std::ifstream in(path_);
  if (!in) {
    return common::Status::success();
  }

  std::string line;
  while (std::getline(in, line)) {
    const std::string normalized = normalize_command(line);
    if (normalized.empty()) {
      continue;
    }
    if (std::find(entries_.begin(), entries_.end(), normalized) == entries_.end()) {
      entries_.push_back(normalized);
    }
  }
  return common::Status::success();
}

common::Status ApprovalStore::save() const {
  std::lock_guard<std::mutex> lock(mutex_);

  std::error_code ec;
  std::filesystem::create_directories(path_.parent_path(), ec);
  if (ec) {
    return common::Status::error("failed to create approval dir: " + ec.message());
  }

  std::ofstream out(path_, std::ios::trunc);
  if (!out) {
    return common::Status::error("failed to open approval store file");
  }

  for (const auto &entry : entries_) {
    out << entry << "\n";
  }
  return out ? common::Status::success()
             : common::Status::error("failed to write approval store file");
}

bool ApprovalStore::contains(const std::string &command) const {
  const std::string normalized = normalize_command(command);
  if (normalized.empty()) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  return std::find(entries_.begin(), entries_.end(), normalized) != entries_.end();
}

std::vector<std::string> ApprovalStore::entries() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_;
}

void ApprovalStore::add(const std::string &command) {
  const std::string normalized = normalize_command(command);
  if (normalized.empty()) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (std::find(entries_.begin(), entries_.end(), normalized) == entries_.end()) {
    entries_.push_back(normalized);
  }
}

std::string ApprovalStore::normalize_command(const std::string &command) {
  return normalize_text(command);
}

ApprovalSocketClient::ApprovalSocketClient(std::filesystem::path socket_path)
    : socket_path_(common::expand_path(socket_path.string())) {}

common::Result<ApprovalDecision> ApprovalSocketClient::request(const ApprovalRequest &request) const {
  const std::string command = sanitize_protocol_token(common::trim(request.command));
  if (command.empty()) {
    return common::Result<ApprovalDecision>::failure("approval request command is empty");
  }

  const std::string socket = socket_path_.string();
  if (socket.empty()) {
    return common::Result<ApprovalDecision>::failure("approval socket path is empty");
  }

  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return common::Result<ApprovalDecision>::failure("failed to create unix socket");
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (socket.size() >= sizeof(addr.sun_path)) {
    close(fd);
    return common::Result<ApprovalDecision>::failure("approval socket path is too long");
  }
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket.c_str());

  if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return common::Result<ApprovalDecision>::failure("failed to connect to approval socket");
  }

  std::ostringstream request_line;
  request_line << "REQUEST\t" << request.timeout.count() << "\t"
               << sanitize_protocol_token(request.session_id) << "\t" << command << "\n";

  const std::string payload = request_line.str();
  const ssize_t sent = ::write(fd, payload.data(), payload.size());
  if (sent < 0 || static_cast<std::size_t>(sent) != payload.size()) {
    close(fd);
    return common::Result<ApprovalDecision>::failure("failed to send approval request");
  }

  std::string response;
  response.reserve(256);

  const auto deadline = std::chrono::steady_clock::now() + request.timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    const int timeout_ms =
        std::max(1, static_cast<int>(std::min<std::chrono::milliseconds>(remaining, std::chrono::milliseconds(250)).count()));

    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;

    const int ready = poll(&pfd, 1, timeout_ms);
    if (ready <= 0) {
      continue;
    }

    std::array<char, 256> chunk{};
    const ssize_t bytes = ::read(fd, chunk.data(), chunk.size());
    if (bytes <= 0) {
      break;
    }
    response.append(chunk.data(), static_cast<std::size_t>(bytes));
    if (response.find('\n') != std::string::npos) {
      break;
    }
  }

  close(fd);

  const auto line_end = response.find('\n');
  const std::string line = common::trim(response.substr(0, line_end));
  if (line.empty()) {
    return common::Result<ApprovalDecision>::failure("approval request timed out");
  }

  const std::string prefix = "DECISION\t";
  if (line.rfind(prefix, 0) != 0) {
    return common::Result<ApprovalDecision>::failure("approval socket returned malformed response");
  }

  auto parsed = approval_decision_from_string(line.substr(prefix.size()));
  if (!parsed.ok()) {
    return common::Result<ApprovalDecision>::failure(parsed.error());
  }
  return common::Result<ApprovalDecision>::success(parsed.value());
}

ApprovalSocketServer::ApprovalSocketServer(std::filesystem::path socket_path, Handler handler)
    : socket_path_(common::expand_path(socket_path.string())), handler_(std::move(handler)) {}

ApprovalSocketServer::~ApprovalSocketServer() { stop(); }

common::Status ApprovalSocketServer::start() {
  if (running_.exchange(true)) {
    return common::Status::success();
  }

  std::error_code ec;
  std::filesystem::create_directories(socket_path_.parent_path(), ec);
  if (ec) {
    running_.store(false);
    return common::Status::error("failed to create approval socket dir: " + ec.message());
  }

  const std::string socket = socket_path_.string();
  if (socket.size() >= sizeof(sockaddr_un::sun_path)) {
    running_.store(false);
    return common::Status::error("approval socket path is too long");
  }

  ::unlink(socket.c_str());

  listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    running_.store(false);
    return common::Status::error("failed to create approval server socket");
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket.c_str());

  if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    close(listen_fd_);
    listen_fd_ = -1;
    running_.store(false);
    return common::Status::error("failed to bind approval socket");
  }

  if (::listen(listen_fd_, 16) != 0) {
    close(listen_fd_);
    listen_fd_ = -1;
    running_.store(false);
    return common::Status::error("failed to listen on approval socket");
  }

  worker_ = std::thread([this]() { run_loop(); });
  return common::Status::success();
}

void ApprovalSocketServer::stop() {
  if (!running_.exchange(false)) {
    return;
  }

  if (listen_fd_ >= 0) {
    close(listen_fd_);
    listen_fd_ = -1;
  }

  if (worker_.joinable()) {
    worker_.join();
  }

  ::unlink(socket_path_.c_str());
}

void ApprovalSocketServer::run_loop() {
  while (running_.load()) {
    const int client = ::accept(listen_fd_, nullptr, nullptr);
    if (client < 0) {
      if (running_.load()) {
        continue;
      }
      break;
    }

    std::string line;
    line.reserve(512);
    std::array<char, 256> chunk{};
    while (true) {
      const ssize_t bytes = ::read(client, chunk.data(), chunk.size());
      if (bytes <= 0) {
        break;
      }
      line.append(chunk.data(), static_cast<std::size_t>(bytes));
      if (line.find('\n') != std::string::npos) {
        break;
      }
    }

    ApprovalRequest request;
    ApprovalDecision decision = ApprovalDecision::Deny;

    const std::string trimmed = common::trim(line);
    if (!trimmed.empty()) {
      std::vector<std::string> fields;
      std::stringstream stream(trimmed);
      std::string token;
      while (std::getline(stream, token, '\t')) {
        fields.push_back(token);
      }

      if (fields.size() >= 4 && normalize_text(fields[0]) == "request") {
        try {
          request.timeout = std::chrono::seconds(std::max(0, std::stoi(fields[1])));
        } catch (...) {
          request.timeout = std::chrono::seconds(120);
        }
        request.session_id = fields[2];
        request.command = fields[3];
        if (handler_) {
          decision = handler_(request);
        }
      }
    }

    const std::string response = "DECISION\t" + approval_decision_to_string(decision) + "\n";
    (void)::write(client, response.data(), response.size());
    close(client);
  }
}

ApprovalManager::ApprovalManager(ApprovalPolicy policy, std::filesystem::path store_path,
                                 std::filesystem::path socket_path)
    : policy_(std::move(policy)), store_(std::move(store_path)), client_(std::move(socket_path)) {
  (void)store_.load();
}

void ApprovalManager::set_policy(ApprovalPolicy policy) { policy_ = std::move(policy); }

const ApprovalPolicy &ApprovalManager::policy() const { return policy_; }

bool ApprovalManager::is_allowlisted(const std::string &command) const {
  return matches_allowlist(command, policy_.allowlist) || store_.contains(command);
}

bool ApprovalManager::needs_approval(const ApprovalRequest &request) const {
  const bool allowlisted = is_allowlisted(request.command);

  if (policy_.security == ExecSecurity::Deny) {
    return false;
  }
  if (policy_.ask == ExecAsk::Always) {
    return true;
  }
  if (policy_.ask == ExecAsk::Off) {
    return false;
  }

  return !allowlisted;
}

common::Result<ApprovalDecision> ApprovalManager::authorize(const ApprovalRequest &request) {
  const bool allowlisted = is_allowlisted(request.command);

  if (policy_.security == ExecSecurity::Deny) {
    return common::Result<ApprovalDecision>::success(ApprovalDecision::Deny);
  }

  if (policy_.security == ExecSecurity::Allowlist && !allowlisted && policy_.ask == ExecAsk::Off) {
    return common::Result<ApprovalDecision>::success(ApprovalDecision::Deny);
  }

  if (!needs_approval(request)) {
    if (policy_.security == ExecSecurity::Allowlist && !allowlisted) {
      return common::Result<ApprovalDecision>::success(ApprovalDecision::Deny);
    }
    return common::Result<ApprovalDecision>::success(ApprovalDecision::AllowOnce);
  }

  auto decision = client_.request(request);
  if (!decision.ok()) {
    return common::Result<ApprovalDecision>::success(fail_safe_decision());
  }

  if (decision.value() == ApprovalDecision::AllowAlways) {
    store_.add(request.command);
    (void)store_.save();
  }

  return decision;
}

bool ApprovalManager::matches_allowlist(const std::string &command,
                                        const std::vector<std::string> &allowlist) const {
  const std::string normalized_command = normalize_text(command);
  const std::string executable = first_token(normalized_command);

  for (const auto &entry : allowlist) {
    const std::string pattern = normalize_text(entry);
    if (pattern.empty()) {
      continue;
    }

    if (!has_glob(pattern)) {
      if (normalized_command == pattern || executable == pattern) {
        return true;
      }
      continue;
    }

    try {
      const std::regex re(glob_to_regex(pattern));
      if (std::regex_match(normalized_command, re) || std::regex_match(executable, re)) {
        return true;
      }
    } catch (const std::exception &) {
      continue;
    }
  }

  return false;
}

} // namespace ghostclaw::security
