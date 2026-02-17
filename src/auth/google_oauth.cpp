#include "ghostclaw/auth/google_oauth.hpp"

#include "ghostclaw/common/fs.hpp"
#include "ghostclaw/common/json_util.hpp"
#include "ghostclaw/config/config.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include <openssl/evp.h>
#include <openssl/sha.h>

namespace ghostclaw::auth {

namespace {

constexpr std::uint64_t HTTP_TIMEOUT_MS = 30000;
constexpr std::int64_t EXPIRY_BUFFER_SECS = 60;

constexpr const char *GOOGLE_AUTH_URL = "https://accounts.google.com/o/oauth2/v2/auth";
constexpr const char *GOOGLE_TOKEN_URL = "https://oauth2.googleapis.com/token";

std::filesystem::path google_auth_json_path() {
  auto dir = config::config_dir();
  if (!dir.ok()) {
    return {};
  }
  return dir.value() / "google_auth.json";
}

std::int64_t now_unix() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void set_file_permissions_0600(const std::filesystem::path &path) {
#ifndef _WIN32
  chmod(path.c_str(), 0600);
#else
  (void)path;
#endif
}

std::string url_encode_component(const std::string &value) {
  std::ostringstream encoded;
  for (const char c : value) {
    const auto ch = static_cast<unsigned char>(c);
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
        ch == '-' || ch == '_' || ch == '.' || ch == '~') {
      encoded << static_cast<char>(ch);
    } else {
      encoded << '%';
      encoded << "0123456789ABCDEF"[ch >> 4];
      encoded << "0123456789ABCDEF"[ch & 0x0F];
    }
  }
  return encoded.str();
}

std::string generate_code_verifier() {
  static constexpr const char charset[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<std::size_t> dist(0, sizeof(charset) - 2);

  std::string verifier;
  verifier.reserve(43);
  for (int i = 0; i < 43; ++i) {
    verifier.push_back(charset[dist(gen)]);
  }
  return verifier;
}

std::string base64url_encode(const unsigned char *data, std::size_t len) {
  static constexpr const char table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  result.reserve(((len + 2) / 3) * 4);

  for (std::size_t i = 0; i < len; i += 3) {
    const unsigned int n = (static_cast<unsigned int>(data[i]) << 16) |
                            (i + 1 < len ? static_cast<unsigned int>(data[i + 1]) << 8 : 0) |
                            (i + 2 < len ? static_cast<unsigned int>(data[i + 2]) : 0);
    result.push_back(table[(n >> 18) & 0x3F]);
    result.push_back(table[(n >> 12) & 0x3F]);
    if (i + 1 < len) result.push_back(table[(n >> 6) & 0x3F]);
    if (i + 2 < len) result.push_back(table[n & 0x3F]);
  }

  // Convert to base64url: replace + with -, / with _, remove trailing =
  for (auto &ch : result) {
    if (ch == '+') ch = '-';
    else if (ch == '/') ch = '_';
  }
  return result;
}

std::string compute_code_challenge(const std::string &verifier) {
  unsigned char hash[SHA256_DIGEST_LENGTH];
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
  EVP_DigestUpdate(ctx, verifier.c_str(), verifier.size());
  unsigned int hash_len = 0;
  EVP_DigestFinal_ex(ctx, hash, &hash_len);
  EVP_MD_CTX_free(ctx);
  return base64url_encode(hash, SHA256_DIGEST_LENGTH);
}

std::string extract_code_from_request(const std::string &request) {
  // Find "GET /callback?code=..." in HTTP request
  const auto query_start = request.find("code=");
  if (query_start == std::string::npos) {
    return "";
  }
  const auto value_start = query_start + 5;
  auto value_end = request.find_first_of("& \r\n", value_start);
  if (value_end == std::string::npos) {
    value_end = request.size();
  }
  return request.substr(value_start, value_end - value_start);
}

common::Result<std::string> wait_for_callback(std::uint16_t port) {
  const int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    return common::Result<std::string>::failure("failed to create socket");
  }

  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);

  if (bind(server_fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
    close(server_fd);
    return common::Result<std::string>::failure(
        "failed to bind to localhost:" + std::to_string(port));
  }

  if (listen(server_fd, 1) < 0) {
    close(server_fd);
    return common::Result<std::string>::failure("failed to listen");
  }

  const int client_fd = accept(server_fd, nullptr, nullptr);
  if (client_fd < 0) {
    close(server_fd);
    return common::Result<std::string>::failure("failed to accept connection");
  }

  std::array<char, 4096> buf{};
  const ssize_t bytes = read(client_fd, buf.data(), buf.size() - 1);
  std::string request;
  if (bytes > 0) {
    request.assign(buf.data(), static_cast<std::size_t>(bytes));
  }

  // Send success response
  const char *response =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/html\r\n"
      "Connection: close\r\n\r\n"
      "<html><body><h1>Authorization successful!</h1>"
      "<p>You can close this tab and return to GhostClaw.</p></body></html>";
  (void)write(client_fd, response, strlen(response));

  close(client_fd);
  close(server_fd);

  const std::string code = extract_code_from_request(request);
  if (code.empty()) {
    return common::Result<std::string>::failure("no authorization code in callback");
  }
  return common::Result<std::string>::success(code);
}

providers::HttpResponse post_form(providers::HttpClient &http, const std::string &url,
                                   const std::string &body) {
  std::unordered_map<std::string, std::string> headers = {
      {"Content-Type", "application/x-www-form-urlencoded"},
  };
  return http.post_json(url, headers, body, HTTP_TIMEOUT_MS);
}

} // namespace

// ── Token storage ─────────────────────────────────────────────────────────────

common::Result<GoogleTokens> load_google_tokens() {
  const auto path = google_auth_json_path();
  if (path.empty()) {
    return common::Result<GoogleTokens>::failure("unable to determine config directory");
  }
  if (!std::filesystem::exists(path)) {
    return common::Result<GoogleTokens>::failure("google_auth.json not found");
  }

  std::ifstream file(path);
  if (!file) {
    return common::Result<GoogleTokens>::failure("unable to open google_auth.json");
  }

  std::ostringstream buf;
  buf << file.rdbuf();
  const std::string json = buf.str();

  GoogleTokens tokens;
  tokens.access_token = common::json_get_string(json, "access_token");
  tokens.refresh_token = common::json_get_string(json, "refresh_token");

  const std::string expires_str = common::json_get_number(json, "expires_at");
  if (!expires_str.empty()) {
    try {
      tokens.expires_at = std::stoll(expires_str);
    } catch (...) {
      tokens.expires_at = 0;
    }
  }

  if (tokens.access_token.empty() && tokens.refresh_token.empty()) {
    return common::Result<GoogleTokens>::failure("google_auth.json contains no tokens");
  }

  return common::Result<GoogleTokens>::success(std::move(tokens));
}

common::Status save_google_tokens(const GoogleTokens &tokens) {
  const auto path = google_auth_json_path();
  if (path.empty()) {
    return common::Status::error("unable to determine config directory");
  }

  const std::filesystem::path tmp_path = path.string() + ".tmp";

  std::ofstream file(tmp_path, std::ios::trunc);
  if (!file) {
    return common::Status::error("unable to write google_auth.json.tmp");
  }

  file << "{\n";
  file << "  \"access_token\": \"" << common::json_escape(tokens.access_token) << "\",\n";
  file << "  \"refresh_token\": \"" << common::json_escape(tokens.refresh_token) << "\",\n";
  file << "  \"expires_at\": " << tokens.expires_at << "\n";
  file << "}\n";

  file.close();
  if (!file) {
    return common::Status::error("failed writing google_auth.json.tmp");
  }

  set_file_permissions_0600(tmp_path);

  std::error_code ec;
  std::filesystem::rename(tmp_path, path, ec);
  if (ec) {
    return common::Status::error("failed to atomically replace google_auth.json: " + ec.message());
  }

  return common::Status::success();
}

common::Status delete_google_tokens() {
  const auto path = google_auth_json_path();
  if (path.empty()) {
    return common::Status::error("unable to determine config directory");
  }
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    std::filesystem::remove(path, ec);
    if (ec) {
      return common::Status::error("failed to remove google_auth.json: " + ec.message());
    }
  }
  return common::Status::success();
}

bool has_valid_google_tokens() {
  auto tokens = load_google_tokens();
  if (!tokens.ok()) {
    return false;
  }
  if (!tokens.value().refresh_token.empty()) {
    return true;
  }
  if (tokens.value().expires_at > 0 && now_unix() >= tokens.value().expires_at) {
    return false;
  }
  return !tokens.value().access_token.empty();
}

// ── OAuth2 flow ───────────────────────────────────────────────────────────────

common::Status run_google_login(providers::HttpClient &http,
                                 const config::GoogleConfig &config) {
  if (config.client_id.empty()) {
    return common::Status::error("google.client_id is required for Google login");
  }

  const std::string code_verifier = generate_code_verifier();
  const std::string code_challenge = compute_code_challenge(code_verifier);
  const std::string redirect_uri =
      "http://localhost:" + std::to_string(config.redirect_port) + "/callback";

  // Build scopes string
  std::string scopes;
  for (std::size_t i = 0; i < config.scopes.size(); ++i) {
    if (i > 0) scopes += ' ';
    scopes += config.scopes[i];
  }

  std::ostringstream auth_url;
  auth_url << GOOGLE_AUTH_URL;
  auth_url << "?client_id=" << url_encode_component(config.client_id);
  auth_url << "&redirect_uri=" << url_encode_component(redirect_uri);
  auth_url << "&response_type=code";
  auth_url << "&scope=" << url_encode_component(scopes);
  auth_url << "&code_challenge=" << url_encode_component(code_challenge);
  auth_url << "&code_challenge_method=S256";
  auth_url << "&access_type=offline";
  auth_url << "&prompt=consent";

  std::cout << "\nOpen this URL in your browser to authorize GhostClaw:\n\n";
  std::cout << "  " << auth_url.str() << "\n\n";
  std::cout << "Waiting for authorization callback on localhost:" << config.redirect_port
            << "...\n";

  // Start local server and wait for callback
  auto callback_result = wait_for_callback(config.redirect_port);
  if (!callback_result.ok()) {
    return common::Status::error(callback_result.error());
  }

  const std::string &authorization_code = callback_result.value();
  std::cout << "Authorization code received. Exchanging for tokens...\n";

  // Exchange code for tokens
  std::string body = "grant_type=authorization_code";
  body += "&code=" + url_encode_component(authorization_code);
  body += "&redirect_uri=" + url_encode_component(redirect_uri);
  body += "&client_id=" + url_encode_component(config.client_id);
  body += "&client_secret=" + url_encode_component(config.client_secret);
  body += "&code_verifier=" + url_encode_component(code_verifier);

  auto response = post_form(http, GOOGLE_TOKEN_URL, body);

  if (response.network_error) {
    return common::Status::error("network error exchanging code: " + response.network_error_message);
  }
  if (response.status != 200) {
    return common::Status::error("token exchange failed (HTTP " + std::to_string(response.status) +
                                  "): " + response.body);
  }

  GoogleTokens tokens;
  tokens.access_token = common::json_get_string(response.body, "access_token");
  tokens.refresh_token = common::json_get_string(response.body, "refresh_token");

  const std::string expires_in_str = common::json_get_number(response.body, "expires_in");
  if (!expires_in_str.empty()) {
    try {
      tokens.expires_at = now_unix() + std::stoll(expires_in_str);
    } catch (...) {
      tokens.expires_at = 0;
    }
  }

  if (tokens.access_token.empty()) {
    return common::Status::error("token exchange returned no access_token");
  }

  auto saved = save_google_tokens(tokens);
  if (!saved.ok()) {
    return common::Status::error("login succeeded but failed to save tokens: " + saved.error());
  }

  std::cout << "Google login successful! Tokens saved to " << google_auth_json_path().string()
            << "\n";
  return common::Status::success();
}

common::Result<GoogleTokens> refresh_google_token(providers::HttpClient &http,
                                                    const config::GoogleConfig &config,
                                                    const std::string &refresh_token) {
  std::string body = "grant_type=refresh_token";
  body += "&refresh_token=" + url_encode_component(refresh_token);
  body += "&client_id=" + url_encode_component(config.client_id);
  body += "&client_secret=" + url_encode_component(config.client_secret);

  auto response = post_form(http, GOOGLE_TOKEN_URL, body);

  if (response.network_error) {
    return common::Result<GoogleTokens>::failure(
        "network error refreshing Google token: " + response.network_error_message);
  }
  if (response.status != 200) {
    return common::Result<GoogleTokens>::failure(
        "Google token refresh failed (HTTP " + std::to_string(response.status) +
        "): " + response.body);
  }

  GoogleTokens tokens;
  tokens.access_token = common::json_get_string(response.body, "access_token");
  tokens.refresh_token = common::json_get_string(response.body, "refresh_token");

  if (tokens.refresh_token.empty()) {
    tokens.refresh_token = refresh_token;
  }

  const std::string expires_in_str = common::json_get_number(response.body, "expires_in");
  if (!expires_in_str.empty()) {
    try {
      tokens.expires_at = now_unix() + std::stoll(expires_in_str);
    } catch (...) {
      tokens.expires_at = 0;
    }
  }

  if (tokens.access_token.empty()) {
    return common::Result<GoogleTokens>::failure("refresh returned no access_token");
  }

  return common::Result<GoogleTokens>::success(std::move(tokens));
}

common::Result<std::string> get_valid_google_token(providers::HttpClient &http,
                                                     const config::GoogleConfig &config) {
  auto loaded = load_google_tokens();
  if (!loaded.ok()) {
    return common::Result<std::string>::failure(loaded.error());
  }

  auto &tokens = loaded.value();

  const bool expired =
      tokens.expires_at > 0 && now_unix() >= (tokens.expires_at - EXPIRY_BUFFER_SECS);

  if (!expired && !tokens.access_token.empty()) {
    return common::Result<std::string>::success(tokens.access_token);
  }

  if (tokens.refresh_token.empty()) {
    return common::Result<std::string>::failure(
        "Google access token expired and no refresh token available");
  }

  auto refreshed = refresh_google_token(http, config, tokens.refresh_token);
  if (!refreshed.ok()) {
    return common::Result<std::string>::failure(
        "failed to refresh Google token: " + refreshed.error());
  }

  auto saved = save_google_tokens(refreshed.value());
  if (!saved.ok()) {
    return common::Result<std::string>::success(refreshed.value().access_token);
  }

  return common::Result<std::string>::success(refreshed.value().access_token);
}

} // namespace ghostclaw::auth
