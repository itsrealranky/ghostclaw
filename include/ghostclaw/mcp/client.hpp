#pragma once

#include "ghostclaw/common/result.hpp"
#include "ghostclaw/config/schema.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include <sys/types.h>

namespace ghostclaw::mcp {

struct McpToolInfo {
  std::string name;
  std::string description;
  std::string input_schema_json;
};

class McpClient {
public:
  explicit McpClient(config::McpServerConfig config);
  ~McpClient();

  McpClient(const McpClient &) = delete;
  McpClient &operator=(const McpClient &) = delete;

  [[nodiscard]] common::Status start();
  void stop();
  [[nodiscard]] bool is_running() const;

  [[nodiscard]] common::Result<std::vector<McpToolInfo>> list_tools();
  [[nodiscard]] common::Result<std::string> call_tool(const std::string &tool_name,
                                                       const std::string &arguments_json);

  [[nodiscard]] const std::string &server_id() const { return config_.id; }

private:
  [[nodiscard]] common::Result<std::string> send_request(const std::string &method,
                                                          const std::string &params_json);
  [[nodiscard]] common::Result<std::string> read_response(int expected_id);

  config::McpServerConfig config_;
  pid_t pid_ = -1;
  int stdin_fd_ = -1;
  int stdout_fd_ = -1;
  std::atomic<int> next_id_{1};
  std::mutex io_mutex_;
  std::string read_buffer_;
};

} // namespace ghostclaw::mcp
