#pragma once

#include "ghostclaw/browser/actions.hpp"
#include "ghostclaw/common/result.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace ghostclaw::browser {

struct BrowserServerOptions {
  std::string host = "127.0.0.1";
  std::uint16_t port = 8089;
  std::size_t max_body_bytes = 256 * 1024;
};

struct BrowserHttpRequest {
  std::string method;
  std::string path;
  std::string raw_path;
  std::unordered_map<std::string, std::string> headers;
  std::unordered_map<std::string, std::string> query;
  std::string body;
};

struct BrowserHttpResponse {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
  std::unordered_map<std::string, std::string> headers;
};

struct BrowserTabInfo {
  std::string tab_id;
  std::string url;
  bool active = false;
};

class BrowserHttpServer {
public:
  explicit BrowserHttpServer(IBrowserActions &actions);
  ~BrowserHttpServer();

  [[nodiscard]] common::Status start(const BrowserServerOptions &options);
  void stop();

  [[nodiscard]] bool is_running() const;
  [[nodiscard]] std::uint16_t port() const;

  [[nodiscard]] BrowserHttpResponse dispatch_for_test(const BrowserHttpRequest &request);

private:
  [[nodiscard]] common::Status validate_bind_address(const std::string &host) const;

  [[nodiscard]] BrowserHttpResponse handle_navigate(const BrowserHttpRequest &request);
  [[nodiscard]] BrowserHttpResponse handle_screenshot(const BrowserHttpRequest &request);
  [[nodiscard]] BrowserHttpResponse handle_snapshot(const BrowserHttpRequest &request);
  [[nodiscard]] BrowserHttpResponse handle_read(const BrowserHttpRequest &request);
  [[nodiscard]] BrowserHttpResponse handle_act(const BrowserHttpRequest &request);
  [[nodiscard]] BrowserHttpResponse handle_tabs_list(const BrowserHttpRequest &request) const;
  [[nodiscard]] BrowserHttpResponse handle_tabs_open(const BrowserHttpRequest &request);
  [[nodiscard]] BrowserHttpResponse handle_tabs_close(const BrowserHttpRequest &request);

  [[nodiscard]] std::string ensure_active_tab_locked();

  void accept_loop();
  void handle_client(int client_fd);

  IBrowserActions &actions_;
  BrowserServerOptions options_;

  std::atomic<bool> running_{false};
  int listen_fd_ = -1;
  std::thread accept_thread_;
  std::uint16_t bound_port_ = 0;

  mutable std::mutex tabs_mutex_;
  std::unordered_map<std::string, BrowserTabInfo> tabs_;
  std::string active_tab_id_;
  std::uint64_t next_tab_id_ = 1;
};

} // namespace ghostclaw::browser
