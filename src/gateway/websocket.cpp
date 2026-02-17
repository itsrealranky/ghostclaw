#include "ghostclaw/gateway/websocket.hpp"

#include "ghostclaw/common/fs.hpp"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <limits>
#include <sstream>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ghostclaw::gateway {

namespace {

constexpr std::size_t kMaxHandshakeBytes = 8 * 1024;
constexpr std::size_t kMaxFramePayloadBytes = 1024 * 1024;
constexpr int kListenBacklog = 64;
constexpr std::string_view kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

#ifndef _WIN32
ssize_t write_bytes(const int fd, SSL *ssl, const std::uint8_t *data,
                    const std::size_t size) {
  if (ssl != nullptr) {
    return static_cast<ssize_t>(SSL_write(ssl, data, static_cast<int>(size)));
  }
  return send(fd, data, size, 0);
}

ssize_t read_bytes(const int fd, SSL *ssl, std::uint8_t *data, const std::size_t size) {
  if (ssl != nullptr) {
    return static_cast<ssize_t>(SSL_read(ssl, data, static_cast<int>(size)));
  }
  return recv(fd, data, size, 0);
}

bool send_all(const int fd, SSL *ssl, const std::uint8_t *data, std::size_t size) {
  std::size_t sent = 0;
  while (sent < size) {
    const auto remaining = size - sent;
    const ssize_t n = write_bytes(fd, ssl, data + sent, remaining);
    if (n <= 0) {
      return false;
    }
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

bool recv_exact(const int fd, SSL *ssl, std::uint8_t *data, std::size_t size) {
  std::size_t received = 0;
  while (received < size) {
    const auto remaining = size - received;
    const ssize_t n = read_bytes(fd, ssl, data + received, remaining);
    if (n <= 0) {
      return false;
    }
    received += static_cast<std::size_t>(n);
  }
  return true;
}

std::string lower_trimmed(const std::string &value) {
  return common::to_lower(common::trim(value));
}

std::string normalize_bind_host(const std::string &host) {
  const std::string lowered = lower_trimmed(host);
  if (lowered == "localhost" || lowered == "::1" || lowered == "[::1]") {
    return "127.0.0.1";
  }
  return common::trim(host);
}

std::unordered_map<std::string, std::string> parse_headers(const std::string &request) {
  std::unordered_map<std::string, std::string> headers;
  std::istringstream lines(request);
  std::string line;
  bool first = true;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      break;
    }
    if (first) {
      headers[":request-line"] = line;
      first = false;
      continue;
    }
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    const std::string key = lower_trimmed(line.substr(0, colon));
    const std::string value = common::trim(line.substr(colon + 1));
    headers[key] = value;
  }
  return headers;
}

std::string websocket_accept(const std::string &client_key) {
  const std::string source = client_key + std::string(kWebSocketGuid);
  std::array<unsigned char, SHA_DIGEST_LENGTH> digest{};
  SHA1(reinterpret_cast<const unsigned char *>(source.data()), source.size(), digest.data());

  const int output_len = 4 * static_cast<int>((digest.size() + 2) / 3);
  std::string output(static_cast<std::size_t>(output_len), '\0');
  EVP_EncodeBlock(reinterpret_cast<unsigned char *>(output.data()), digest.data(),
                  static_cast<int>(digest.size()));
  return output;
}

std::string openssl_error_string() {
  const auto code = ERR_get_error();
  if (code == 0) {
    return "unknown openssl error";
  }
  std::array<char, 256> buffer{};
  ERR_error_string_n(code, buffer.data(), buffer.size());
  return std::string(buffer.data());
}

bool send_http_response(const int fd, SSL *ssl, const int status, const std::string &status_text,
                        const std::vector<std::pair<std::string, std::string>> &headers,
                        const std::string &body = "") {
  std::ostringstream response;
  response << "HTTP/1.1 " << status << " " << status_text << "\r\n";
  for (const auto &[k, v] : headers) {
    response << k << ": " << v << "\r\n";
  }
  response << "Content-Length: " << body.size() << "\r\n";
  response << "\r\n";
  response << body;
  const std::string text = response.str();
  return send_all(fd, ssl, reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
}

bool read_next_frame(const int fd, SSL *ssl, std::uint8_t &opcode, std::string &payload) {
  std::array<std::uint8_t, 2> header{};
  if (!recv_exact(fd, ssl, header.data(), header.size())) {
    return false;
  }

  const bool fin = (header[0] & 0x80u) != 0;
  opcode = static_cast<std::uint8_t>(header[0] & 0x0Fu);
  const bool masked = (header[1] & 0x80u) != 0;
  std::uint64_t payload_len = static_cast<std::uint64_t>(header[1] & 0x7Fu);

  if (!fin) {
    return false;
  }

  if (payload_len == 126u) {
    std::array<std::uint8_t, 2> ext{};
    if (!recv_exact(fd, ssl, ext.data(), ext.size())) {
      return false;
    }
    payload_len = (static_cast<std::uint64_t>(ext[0]) << 8u) | static_cast<std::uint64_t>(ext[1]);
  } else if (payload_len == 127u) {
    std::array<std::uint8_t, 8> ext{};
    if (!recv_exact(fd, ssl, ext.data(), ext.size())) {
      return false;
    }
    payload_len = 0;
    for (const auto byte : ext) {
      payload_len = (payload_len << 8u) | static_cast<std::uint64_t>(byte);
    }
  }

  if (!masked || payload_len > kMaxFramePayloadBytes ||
      payload_len > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return false;
  }

  std::array<std::uint8_t, 4> mask{};
  if (!recv_exact(fd, ssl, mask.data(), mask.size())) {
    return false;
  }

  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(payload_len));
  if (!bytes.empty() && !recv_exact(fd, ssl, bytes.data(), bytes.size())) {
    return false;
  }

  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] ^= mask[i % mask.size()];
  }

  payload.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  return true;
}

bool send_frame(const int fd, SSL *ssl, const std::uint8_t opcode, const std::string &payload) {
  std::vector<std::uint8_t> frame;
  frame.reserve(payload.size() + 16);
  frame.push_back(static_cast<std::uint8_t>(0x80u | (opcode & 0x0Fu)));

  const auto size = payload.size();
  if (size <= 125u) {
    frame.push_back(static_cast<std::uint8_t>(size));
  } else if (size <= 65535u) {
    frame.push_back(126u);
    frame.push_back(static_cast<std::uint8_t>((size >> 8u) & 0xFFu));
    frame.push_back(static_cast<std::uint8_t>(size & 0xFFu));
  } else {
    frame.push_back(127u);
    for (int shift = 56; shift >= 0; shift -= 8) {
      frame.push_back(static_cast<std::uint8_t>((size >> static_cast<std::size_t>(shift)) & 0xFFu));
    }
  }

  frame.insert(frame.end(), payload.begin(), payload.end());
  return send_all(fd, ssl, frame.data(), frame.size());
}
#endif

} // namespace

WebSocketServer::WebSocketServer() = default;

WebSocketServer::~WebSocketServer() { stop(); }

common::Status WebSocketServer::start(const WebSocketOptions &options) {
#ifdef _WIN32
  (void)options;
  return common::Status::error("websocket server is not implemented on Windows");
#else
  if (running_) {
    return common::Status::error("websocket server already running");
  }
  if (options.port == 0) {
    return common::Status::error("websocket port must be non-zero");
  }

  const auto host_status = validate_bind_address(options.host);
  if (!host_status.ok()) {
    return host_status;
  }

  listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    return common::Status::error("failed to create websocket listen socket");
  }
  int reuse = 1;
  (void)setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(options.port);
  const std::string bind_host = normalize_bind_host(options.host);
  if (inet_pton(AF_INET, bind_host.c_str(), &addr.sin_addr) != 1) {
    close(listen_fd_);
    listen_fd_ = -1;
    return common::Status::error("invalid websocket bind host");
  }
  if (bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    const std::string message = std::strerror(errno);
    close(listen_fd_);
    listen_fd_ = -1;
    return common::Status::error("websocket bind failed: " + message);
  }
  if (listen(listen_fd_, kListenBacklog) != 0) {
    const std::string message = std::strerror(errno);
    close(listen_fd_);
    listen_fd_ = -1;
    return common::Status::error("websocket listen failed: " + message);
  }

  sockaddr_in actual{};
  socklen_t actual_len = sizeof(actual);
  if (getsockname(listen_fd_, reinterpret_cast<sockaddr *>(&actual), &actual_len) == 0) {
    bound_port_ = ntohs(actual.sin_port);
  } else {
    bound_port_ = options.port;
  }

  options_ = options;
  if (options_.tls_enabled) {
    if (options_.tls_cert_file.empty() || options_.tls_key_file.empty()) {
      close(listen_fd_);
      listen_fd_ = -1;
      bound_port_ = 0;
      return common::Status::error("websocket TLS requires cert and key file paths");
    }

    tls_ctx_ = SSL_CTX_new(TLS_server_method());
    if (tls_ctx_ == nullptr) {
      close(listen_fd_);
      listen_fd_ = -1;
      bound_port_ = 0;
      return common::Status::error("failed to initialize websocket TLS context: " +
                                   openssl_error_string());
    }
    SSL_CTX_set_min_proto_version(tls_ctx_, TLS1_2_VERSION);
    if (SSL_CTX_use_certificate_file(tls_ctx_, options_.tls_cert_file.c_str(), SSL_FILETYPE_PEM) <=
        0) {
      const std::string message = openssl_error_string();
      SSL_CTX_free(tls_ctx_);
      tls_ctx_ = nullptr;
      close(listen_fd_);
      listen_fd_ = -1;
      bound_port_ = 0;
      return common::Status::error("failed loading websocket TLS certificate: " + message);
    }
    if (SSL_CTX_use_PrivateKey_file(tls_ctx_, options_.tls_key_file.c_str(), SSL_FILETYPE_PEM) <=
        0) {
      const std::string message = openssl_error_string();
      SSL_CTX_free(tls_ctx_);
      tls_ctx_ = nullptr;
      close(listen_fd_);
      listen_fd_ = -1;
      bound_port_ = 0;
      return common::Status::error("failed loading websocket TLS private key: " + message);
    }
    if (SSL_CTX_check_private_key(tls_ctx_) != 1) {
      const std::string message = openssl_error_string();
      SSL_CTX_free(tls_ctx_);
      tls_ctx_ = nullptr;
      close(listen_fd_);
      listen_fd_ = -1;
      bound_port_ = 0;
      return common::Status::error("websocket TLS private key does not match certificate: " +
                                   message);
    }
  }

  running_ = true;
  accept_thread_ = std::thread([this]() { accept_loop(); });
  return common::Status::success();
#endif
}

void WebSocketServer::stop() {
#ifndef _WIN32
  if (!running_ && listen_fd_ < 0 && tls_ctx_ == nullptr) {
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

  std::vector<std::shared_ptr<ClientState>> clients;
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (const auto &[fd, client] : clients_) {
      (void)fd;
      clients.push_back(client);
    }
    clients_.clear();
  }
  for (const auto &client : clients) {
    if (client->ssl != nullptr) {
      SSL_shutdown(client->ssl);
      SSL_free(client->ssl);
      client->ssl = nullptr;
    }
    if (client->fd >= 0) {
      shutdown(client->fd, SHUT_RDWR);
      close(client->fd);
      client->fd = -1;
    }
  }
  if (tls_ctx_ != nullptr) {
    SSL_CTX_free(tls_ctx_);
    tls_ctx_ = nullptr;
  }
  bound_port_ = 0;
#endif
}

bool WebSocketServer::is_running() const { return running_.load(); }

std::uint16_t WebSocketServer::port() const { return bound_port_; }

WebSocketStats WebSocketServer::stats() const {
  std::lock_guard<std::mutex> lock(clients_mutex_);
  WebSocketStats out;
  out.connected_clients = clients_.size();
  for (const auto &[fd, client] : clients_) {
    (void)fd;
    out.total_subscriptions += client->sessions.size();
  }
  return out;
}

std::size_t WebSocketServer::publish_session_event(const std::string &session,
                                                   const RpcMap &payload) {
#ifdef _WIN32
  (void)session;
  (void)payload;
  return 0;
#else
  WsServerMessage message{.type = "event", .session = session, .payload = payload};
  std::vector<std::shared_ptr<ClientState>> recipients;
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    recipients.reserve(clients_.size());
    for (const auto &[fd, client] : clients_) {
      (void)fd;
      if (client->sessions.contains(session)) {
        recipients.push_back(client);
      }
    }
  }

  std::size_t delivered = 0;
  for (const auto &client : recipients) {
    if (send_server_message(client, message)) {
      ++delivered;
    } else {
      remove_client(client->fd);
    }
  }
  return delivered;
#endif
}

common::Status WebSocketServer::validate_bind_address(const std::string &host) const {
  if (common::trim(host).empty()) {
    return common::Status::error("websocket host is empty");
  }
  return common::Status::success();
}

void WebSocketServer::accept_loop() {
#ifndef _WIN32
  while (running_) {
    sockaddr_in client_addr{};
    socklen_t len = sizeof(client_addr);
    const int client_fd = accept(listen_fd_, reinterpret_cast<sockaddr *>(&client_addr), &len);
    if (client_fd < 0) {
      if (!running_) {
        break;
      }
      continue;
    }

    auto client = std::make_shared<ClientState>();
    client->fd = client_fd;
    if (tls_ctx_ != nullptr) {
      client->ssl = SSL_new(tls_ctx_);
      if (client->ssl == nullptr) {
        shutdown(client_fd, SHUT_RDWR);
        close(client_fd);
        continue;
      }
      SSL_set_fd(client->ssl, client_fd);
    }
    {
      std::lock_guard<std::mutex> lock(clients_mutex_);
      if (clients_.size() >= options_.max_clients) {
        const std::string body = R"({"error":"too_many_websocket_clients"})";
        (void)send_http_response(client_fd, client->ssl, 503, "Service Unavailable",
                                 {{"Content-Type", "application/json"}}, body);
        if (client->ssl != nullptr) {
          SSL_shutdown(client->ssl);
          SSL_free(client->ssl);
          client->ssl = nullptr;
        }
        shutdown(client_fd, SHUT_RDWR);
        close(client_fd);
        continue;
      }
      clients_[client_fd] = client;
    }
    try {
      std::thread([this, client]() { client_loop(client); }).detach();
    } catch (const std::system_error &) {
      // Thread creation or detach may fail during shutdown; safe to ignore.
    }
  }
#endif
}

void WebSocketServer::client_loop(const std::shared_ptr<ClientState> client) {
#ifndef _WIN32
  if (client->ssl != nullptr && SSL_accept(client->ssl) <= 0) {
    remove_client(client->fd);
    return;
  }

  if (!perform_handshake(client)) {
    remove_client(client->fd);
    return;
  }

  (void)send_server_message(client, WsServerMessage{.type = "hello", .payload = {{"status", "ready"}}});

  while (running_) {
    std::uint8_t opcode = 0;
    std::string payload;
    if (!read_next_frame(client->fd, client->ssl, opcode, payload)) {
      break;
    }
    if (opcode == 0x8u) {
      break;
    }
    if (opcode == 0x9u) {
      std::lock_guard<std::mutex> write_lock(client->write_mutex);
      (void)send_frame(client->fd, client->ssl, 0xAu, payload);
      continue;
    }
    if (opcode != 0x1u) {
      continue;
    }

    const auto parsed = parse_ws_client_message(payload);
    if (!parsed.ok()) {
      (void)send_server_message(client, WsServerMessage{.type = "error", .error = parsed.error()});
      continue;
    }
    handle_client_message(client, parsed.value());
  }

  remove_client(client->fd);
#else
  (void)client;
#endif
}

bool WebSocketServer::perform_handshake(const std::shared_ptr<ClientState> &client) const {
#ifndef _WIN32
  if (client == nullptr || client->fd < 0) {
    return false;
  }

  const int fd = client->fd;
  SSL *ssl = client->ssl;
  std::string request;
  request.reserve(1024);
  std::array<char, 1024> buf{};
  while (request.size() < kMaxHandshakeBytes) {
    const ssize_t n = read_bytes(fd, ssl, reinterpret_cast<std::uint8_t *>(buf.data()),
                                 buf.size());
    if (n <= 0) {
      return false;
    }
    request.append(buf.data(), static_cast<std::size_t>(n));
    if (request.find("\r\n\r\n") != std::string::npos) {
      break;
    }
  }
  if (request.find("\r\n\r\n") == std::string::npos) {
    return send_http_response(fd, ssl, 400, "Bad Request",
                              {{"Content-Type", "application/json"}},
                              R"({"error":"invalid_websocket_handshake"})");
  }

  const auto headers = parse_headers(request);
  const auto request_line_it = headers.find(":request-line");
  if (request_line_it == headers.end() ||
      request_line_it->second.rfind("GET ", 0) != 0) {
    return send_http_response(fd, ssl, 405, "Method Not Allowed",
                              {{"Content-Type", "application/json"}},
                              R"({"error":"websocket_requires_get"})");
  }

  const auto upgrade_it = headers.find("upgrade");
  const auto connection_it = headers.find("connection");
  const auto version_it = headers.find("sec-websocket-version");
  const auto key_it = headers.find("sec-websocket-key");
  if (upgrade_it == headers.end() || connection_it == headers.end() ||
      version_it == headers.end() || key_it == headers.end() ||
      lower_trimmed(upgrade_it->second) != "websocket" ||
      common::to_lower(connection_it->second).find("upgrade") == std::string::npos ||
      common::trim(version_it->second) != "13") {
    return send_http_response(fd, ssl, 400, "Bad Request",
                              {{"Content-Type", "application/json"}},
                              R"({"error":"missing_websocket_headers"})");
  }

  if (options_.require_authorization) {
    const auto auth_it = headers.find("authorization");
    const std::string auth = (auth_it == headers.end()) ? "" : auth_it->second;
    if (!options_.authorize || !options_.authorize(auth)) {
      return send_http_response(fd, ssl, 401, "Unauthorized",
                                {{"Content-Type", "application/json"}},
                                R"({"error":"unauthorized"})");
    }
  }

  const std::string accept_key = websocket_accept(common::trim(key_it->second));
  return send_http_response(fd, ssl, 101, "Switching Protocols",
                            {{"Upgrade", "websocket"},
                             {"Connection", "Upgrade"},
                             {"Sec-WebSocket-Accept", accept_key}});
#else
  (void)client;
  return false;
#endif
}

void WebSocketServer::remove_client(const int fd) {
#ifndef _WIN32
  std::shared_ptr<ClientState> client;
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    auto it = clients_.find(fd);
    if (it == clients_.end()) {
      return;
    }
    client = it->second;
    clients_.erase(it);
  }
  if (client && client->fd >= 0) {
    if (client->ssl != nullptr) {
      SSL_shutdown(client->ssl);
      SSL_free(client->ssl);
      client->ssl = nullptr;
    }
    shutdown(client->fd, SHUT_RDWR);
    close(client->fd);
    client->fd = -1;
  } else if (client && client->ssl != nullptr) {
    SSL_shutdown(client->ssl);
    SSL_free(client->ssl);
    client->ssl = nullptr;
  }
#else
  (void)fd;
#endif
}

bool WebSocketServer::send_text_frame(const std::shared_ptr<ClientState> &client,
                                      const std::string &payload) const {
#ifndef _WIN32
  if (client == nullptr || client->fd < 0) {
    return false;
  }
  std::lock_guard<std::mutex> write_lock(client->write_mutex);
  return send_frame(client->fd, client->ssl, 0x1u, payload);
#else
  (void)client;
  (void)payload;
  return false;
#endif
}

bool WebSocketServer::send_server_message(const std::shared_ptr<ClientState> &client,
                                          const WsServerMessage &message) const {
  return send_text_frame(client, message.to_json());
}

void WebSocketServer::handle_client_message(const std::shared_ptr<ClientState> &client,
                                            const WsClientMessage &message) {
  const std::string method = !message.method.empty() ? message.method : message.type;
  if (method == "subscribe" || method == "session.subscribe") {
    if (message.session.empty()) {
      (void)send_server_message(client,
                                WsServerMessage{.type = "error",
                                                .id = message.id,
                                                .error = "missing session"});
      return;
    }
    client->sessions.insert(message.session);
    (void)send_server_message(
        client, WsServerMessage{.type = "ack",
                                .id = message.id,
                                .session = message.session,
                                .payload = {{"action", "subscribe"}}});
    return;
  }

  if (method == "unsubscribe" || method == "session.unsubscribe") {
    if (message.session.empty()) {
      (void)send_server_message(client,
                                WsServerMessage{.type = "error",
                                                .id = message.id,
                                                .error = "missing session"});
      return;
    }
    (void)client->sessions.erase(message.session);
    (void)send_server_message(
        client, WsServerMessage{.type = "ack",
                                .id = message.id,
                                .session = message.session,
                                .payload = {{"action", "unsubscribe"}}});
    return;
  }

  if (method == "ping") {
    (void)send_server_message(client, WsServerMessage{.type = "pong", .id = message.id});
    return;
  }

  if (message.type == "rpc") {
    if (method.empty() || method == "rpc") {
      (void)send_server_message(client, WsServerMessage{.type = "error",
                                                        .id = message.id,
                                                        .error = "missing rpc method"});
      return;
    }
    if (!options_.rpc_handler) {
      (void)send_server_message(client, WsServerMessage{.type = "error",
                                                        .id = message.id,
                                                        .error = "rpc handler unavailable"});
      return;
    }
    const auto result = options_.rpc_handler(
        message, [&](const RpcMap &event_payload) {
          (void)send_server_message(client, WsServerMessage{.type = "rpc.event",
                                                            .id = message.id,
                                                            .session = message.session,
                                                            .payload = event_payload});
        });
    if (!result.ok()) {
      (void)send_server_message(client, WsServerMessage{.type = "error",
                                                        .id = message.id,
                                                        .error = result.error()});
      return;
    }
    (void)send_server_message(client, WsServerMessage{.type = "rpc.result",
                                                      .id = message.id,
                                                      .session = message.session,
                                                      .payload = result.value()});
    return;
  }

  (void)send_server_message(client, WsServerMessage{.type = "error",
                                                    .id = message.id,
                                                    .error = "unsupported message type"});
}

} // namespace ghostclaw::gateway
