#include "ghostclaw/mcp/client.hpp"

#include "ghostclaw/common/json_util.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

namespace ghostclaw::mcp {

namespace {

constexpr int READ_TIMEOUT_MS = 30000;

std::string build_jsonrpc_request(int id, const std::string &method,
                                   const std::string &params_json) {
  std::ostringstream out;
  out << R"({"jsonrpc":"2.0","id":)" << id << R"(,"method":")" << method << R"(","params":)";
  if (params_json.empty()) {
    out << "{}";
  } else {
    out << params_json;
  }
  out << "}";
  return out.str();
}

std::string build_jsonrpc_notification(const std::string &method) {
  return R"({"jsonrpc":"2.0","method":")" + method + R"(","params":{}})";
}

} // namespace

McpClient::McpClient(config::McpServerConfig config) : config_(std::move(config)) {}

McpClient::~McpClient() { stop(); }

common::Status McpClient::start() {
  if (pid_ != -1) {
    return common::Status::error("MCP client already running");
  }

  int to_child[2] = {-1, -1};
  int from_child[2] = {-1, -1};

  if (pipe(to_child) != 0 || pipe(from_child) != 0) {
    return common::Status::error("failed to create pipes: " + std::string(strerror(errno)));
  }

  const pid_t pid = fork();
  if (pid < 0) {
    close(to_child[0]);
    close(to_child[1]);
    close(from_child[0]);
    close(from_child[1]);
    return common::Status::error("failed to fork: " + std::string(strerror(errno)));
  }

  if (pid == 0) {
    // Child process
    close(to_child[1]);
    close(from_child[0]);
    dup2(to_child[0], STDIN_FILENO);
    dup2(from_child[1], STDOUT_FILENO);
    close(to_child[0]);
    close(from_child[1]);

    // Set environment variables
    for (const auto &[key, val] : config_.env) {
      setenv(key.c_str(), val.c_str(), 1);
    }

    // Build argv
    std::vector<const char *> argv;
    argv.push_back(config_.command.c_str());
    for (const auto &arg : config_.args) {
      argv.push_back(arg.c_str());
    }
    argv.push_back(nullptr);

    execvp(config_.command.c_str(), const_cast<char *const *>(argv.data()));
    _exit(127);
  }

  // Parent process
  close(to_child[0]);
  close(from_child[1]);
  pid_ = pid;
  stdin_fd_ = to_child[1];
  stdout_fd_ = from_child[0];

  // Set stdout non-blocking
  const int flags = fcntl(stdout_fd_, F_GETFL, 0);
  fcntl(stdout_fd_, F_SETFL, flags | O_NONBLOCK);

  // Send initialize request
  const int init_id = next_id_++;
  std::string init_params = R"({"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"ghostclaw","version":"0.1.0"}})";
  auto init_request = build_jsonrpc_request(init_id, "initialize", init_params);
  init_request += '\n';

  const ssize_t written = write(stdin_fd_, init_request.c_str(), init_request.size());
  if (written < 0) {
    stop();
    return common::Status::error("failed to send initialize request");
  }

  // Read initialize response
  auto init_response = read_response(init_id);
  if (!init_response.ok()) {
    stop();
    return common::Status::error("MCP initialize failed: " + init_response.error());
  }

  // Send initialized notification
  auto notification = build_jsonrpc_notification("notifications/initialized");
  notification += '\n';
  (void)write(stdin_fd_, notification.c_str(), notification.size());

  return common::Status::success();
}

void McpClient::stop() {
  if (stdin_fd_ != -1) {
    close(stdin_fd_);
    stdin_fd_ = -1;
  }
  if (stdout_fd_ != -1) {
    close(stdout_fd_);
    stdout_fd_ = -1;
  }
  if (pid_ != -1) {
    kill(pid_, SIGTERM);
    int status = 0;
    waitpid(pid_, &status, 0);
    pid_ = -1;
  }
  read_buffer_.clear();
}

bool McpClient::is_running() const { return pid_ != -1; }

common::Result<std::vector<McpToolInfo>> McpClient::list_tools() {
  std::lock_guard<std::mutex> lock(io_mutex_);

  auto response = send_request("tools/list", "{}");
  if (!response.ok()) {
    return common::Result<std::vector<McpToolInfo>>::failure(response.error());
  }

  // Parse result.tools array from response
  const std::string &json = response.value();
  const std::string result_obj = common::json_get_object(json, "result");
  if (result_obj.empty()) {
    return common::Result<std::vector<McpToolInfo>>::failure("no result in tools/list response");
  }

  const std::string tools_array = common::json_get_array(result_obj, "tools");
  if (tools_array.empty()) {
    return common::Result<std::vector<McpToolInfo>>::success({});
  }

  auto tool_objects = common::json_split_top_level_objects(tools_array);
  std::vector<McpToolInfo> tools;
  tools.reserve(tool_objects.size());

  for (const auto &tool_json : tool_objects) {
    McpToolInfo info;
    info.name = common::json_get_string(tool_json, "name");
    info.description = common::json_get_string(tool_json, "description");
    info.input_schema_json = common::json_get_object(tool_json, "inputSchema");
    if (info.input_schema_json.empty()) {
      info.input_schema_json = R"({"type":"object","properties":{}})";
    }
    if (!info.name.empty()) {
      tools.push_back(std::move(info));
    }
  }

  return common::Result<std::vector<McpToolInfo>>::success(std::move(tools));
}

common::Result<std::string> McpClient::call_tool(const std::string &tool_name,
                                                   const std::string &arguments_json) {
  std::lock_guard<std::mutex> lock(io_mutex_);

  std::string params = R"({"name":")" + common::json_escape(tool_name) + R"(","arguments":)";
  if (arguments_json.empty()) {
    params += "{}";
  } else {
    params += arguments_json;
  }
  params += "}";

  auto response = send_request("tools/call", params);
  if (!response.ok()) {
    return common::Result<std::string>::failure(response.error());
  }

  // Extract result.content[0].text
  const std::string &json = response.value();
  const std::string result_obj = common::json_get_object(json, "result");
  if (result_obj.empty()) {
    return common::Result<std::string>::failure("no result in tools/call response");
  }

  const std::string content_array = common::json_get_array(result_obj, "content");
  if (content_array.empty()) {
    return common::Result<std::string>::success("");
  }

  auto content_objects = common::json_split_top_level_objects(content_array);
  if (content_objects.empty()) {
    return common::Result<std::string>::success("");
  }

  // Check for error flag
  const std::string is_error = common::json_get_string(result_obj, "isError");
  const std::string text = common::json_get_string(content_objects[0], "text");

  if (is_error == "true") {
    return common::Result<std::string>::failure("MCP tool error: " + text);
  }

  return common::Result<std::string>::success(text);
}

common::Result<std::string> McpClient::send_request(const std::string &method,
                                                      const std::string &params_json) {
  if (pid_ == -1 || stdin_fd_ == -1) {
    return common::Result<std::string>::failure("MCP client not running");
  }

  const int id = next_id_++;
  auto request = build_jsonrpc_request(id, method, params_json);
  request += '\n';

  const ssize_t written = write(stdin_fd_, request.c_str(), request.size());
  if (written < 0) {
    return common::Result<std::string>::failure("failed to write to MCP server stdin");
  }

  return read_response(id);
}

common::Result<std::string> McpClient::read_response(int expected_id) {
  if (stdout_fd_ == -1) {
    return common::Result<std::string>::failure("MCP stdout not available");
  }

  const auto deadline_ms = READ_TIMEOUT_MS;
  int elapsed = 0;

  while (elapsed < deadline_ms) {
    // Check if we have a complete line in the buffer
    auto newline_pos = read_buffer_.find('\n');
    if (newline_pos != std::string::npos) {
      std::string line = read_buffer_.substr(0, newline_pos);
      read_buffer_.erase(0, newline_pos + 1);

      // Skip empty lines
      if (line.empty() || line == "\r") {
        continue;
      }
      // Remove trailing \r
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }

      // Check if this is a response (has "id" matching expected_id)
      const std::string id_str = common::json_get_number(line, "id");
      if (!id_str.empty()) {
        int response_id = 0;
        try {
          response_id = std::stoi(id_str);
        } catch (...) {
          // Not our response, skip
          continue;
        }
        if (response_id == expected_id) {
          // Check for error
          const std::string error_obj = common::json_get_object(line, "error");
          if (!error_obj.empty()) {
            const std::string error_msg = common::json_get_string(error_obj, "message");
            return common::Result<std::string>::failure(
                "MCP error: " + (error_msg.empty() ? error_obj : error_msg));
          }
          return common::Result<std::string>::success(line);
        }
      }
      // Not our response (notification or different id), skip
      continue;
    }

    // Need more data
    struct pollfd pfd{};
    pfd.fd = stdout_fd_;
    pfd.events = POLLIN;
    const int poll_result = poll(&pfd, 1, 100);
    elapsed += 100;

    if (poll_result > 0 && (pfd.revents & POLLIN) != 0) {
      std::array<char, 4096> buf{};
      const ssize_t bytes = read(stdout_fd_, buf.data(), buf.size());
      if (bytes > 0) {
        read_buffer_.append(buf.data(), static_cast<std::size_t>(bytes));
      } else if (bytes == 0) {
        return common::Result<std::string>::failure("MCP server closed stdout");
      }
    }
  }

  return common::Result<std::string>::failure("timeout waiting for MCP response");
}

} // namespace ghostclaw::mcp
