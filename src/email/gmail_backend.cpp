#include "ghostclaw/email/backend.hpp"

#include "ghostclaw/auth/google_oauth.hpp"
#include "ghostclaw/common/json_util.hpp"
#include "ghostclaw/providers/traits.hpp"

#include <sstream>

namespace ghostclaw::email {

namespace {

constexpr const char *GMAIL_API_BASE = "https://gmail.googleapis.com/gmail/v1/users/me";
constexpr std::uint64_t HTTP_TIMEOUT_MS = 30000;

std::string base64url_encode_rfc2822(const std::string &data) {
  static constexpr const char table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  result.reserve(((data.size() + 2) / 3) * 4);

  for (std::size_t i = 0; i < data.size(); i += 3) {
    const auto b0 = static_cast<unsigned char>(data[i]);
    const bool has_b1 = i + 1 < data.size();
    const bool has_b2 = i + 2 < data.size();
    const auto b1 = has_b1 ? static_cast<unsigned char>(data[i + 1]) : 0u;
    const auto b2 = has_b2 ? static_cast<unsigned char>(data[i + 2]) : 0u;
    const unsigned int n = (static_cast<unsigned int>(b0) << 16) |
                            (static_cast<unsigned int>(b1) << 8) |
                            static_cast<unsigned int>(b2);
    result.push_back(table[(n >> 18) & 0x3F]);
    result.push_back(table[(n >> 12) & 0x3F]);
    if (has_b1) result.push_back(table[(n >> 6) & 0x3F]);
    if (has_b2) result.push_back(table[n & 0x3F]);
  }

  for (auto &ch : result) {
    if (ch == '+') ch = '-';
    else if (ch == '/') ch = '_';
  }
  return result;
}

std::string build_rfc2822(const EmailMessage &msg) {
  std::ostringstream rfc;
  if (!msg.from_account.empty()) {
    rfc << "From: " << msg.from_account << "\r\n";
  }
  rfc << "To: " << msg.to << "\r\n";
  rfc << "Subject: " << msg.subject << "\r\n";
  rfc << "Content-Type: text/plain; charset=UTF-8\r\n";
  rfc << "\r\n";
  rfc << msg.body;
  return rfc.str();
}

class GmailBackend final : public IEmailBackend {
public:
  explicit GmailBackend(const config::Config &config)
      : google_config_(config.google), http_(std::make_shared<providers::CurlHttpClient>()) {}

  [[nodiscard]] std::string_view name() const override { return "gmail"; }

  [[nodiscard]] common::Result<std::vector<EmailAccount>> list_accounts() override {
    auto token = auth::get_valid_google_token(*http_, google_config_);
    if (!token.ok()) {
      return common::Result<std::vector<EmailAccount>>::failure(token.error());
    }

    std::unordered_map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + token.value()},
    };

    auto response = http_->post_json(std::string(GMAIL_API_BASE) + "/profile", headers, "",
                                      HTTP_TIMEOUT_MS);
    if (response.network_error) {
      return common::Result<std::vector<EmailAccount>>::failure(response.network_error_message);
    }
    if (response.status != 200) {
      return common::Result<std::vector<EmailAccount>>::failure(
          "Gmail API error (HTTP " + std::to_string(response.status) + "): " + response.body);
    }

    EmailAccount account;
    account.id = common::json_get_string(response.body, "emailAddress");
    account.label = account.id;

    return common::Result<std::vector<EmailAccount>>::success({account});
  }

  [[nodiscard]] common::Result<std::string> draft(const EmailMessage &msg) override {
    auto token = auth::get_valid_google_token(*http_, google_config_);
    if (!token.ok()) {
      return common::Result<std::string>::failure(token.error());
    }

    const std::string rfc2822 = build_rfc2822(msg);
    const std::string encoded = base64url_encode_rfc2822(rfc2822);

    std::string body = R"({"message":{"raw":")" + encoded + R"("}})";

    std::unordered_map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + token.value()},
        {"Content-Type", "application/json"},
    };

    auto response = http_->post_json(std::string(GMAIL_API_BASE) + "/drafts", headers, body,
                                      HTTP_TIMEOUT_MS);
    if (response.network_error) {
      return common::Result<std::string>::failure(response.network_error_message);
    }
    if (response.status != 200) {
      return common::Result<std::string>::failure(
          "Gmail draft error (HTTP " + std::to_string(response.status) + "): " + response.body);
    }

    const std::string draft_id = common::json_get_string(response.body, "id");
    return common::Result<std::string>::success(draft_id);
  }

  [[nodiscard]] common::Status send(const EmailMessage &msg) override {
    auto token = auth::get_valid_google_token(*http_, google_config_);
    if (!token.ok()) {
      return common::Status::error(token.error());
    }

    const std::string rfc2822 = build_rfc2822(msg);
    const std::string encoded = base64url_encode_rfc2822(rfc2822);

    std::string body = R"({"raw":")" + encoded + R"("})";

    std::unordered_map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + token.value()},
        {"Content-Type", "application/json"},
    };

    auto response = http_->post_json(std::string(GMAIL_API_BASE) + "/messages/send", headers, body,
                                      HTTP_TIMEOUT_MS);
    if (response.network_error) {
      return common::Status::error(response.network_error_message);
    }
    if (response.status != 200) {
      return common::Status::error("Gmail send error (HTTP " + std::to_string(response.status) +
                                    "): " + response.body);
    }

    return common::Status::success();
  }

private:
  config::GoogleConfig google_config_;
  std::shared_ptr<providers::CurlHttpClient> http_;
};

} // namespace

std::unique_ptr<IEmailBackend> make_gmail_email_backend(const config::Config &config) {
  return std::make_unique<GmailBackend>(config);
}

} // namespace ghostclaw::email
