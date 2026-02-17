#include "ghostclaw/email/backend.hpp"

#include "ghostclaw/common/fs.hpp"

namespace ghostclaw::email {

std::unique_ptr<IEmailBackend> make_smtp_email_backend(const config::Config &config);
std::unique_ptr<IEmailBackend> make_mailapp_email_backend(const config::Config &config);
std::unique_ptr<IEmailBackend> make_gmail_email_backend(const config::Config &config);

namespace {

class GogEmailBackend final : public IEmailBackend {
public:
  [[nodiscard]] std::string_view name() const override { return "gog"; }

  [[nodiscard]] common::Result<std::vector<EmailAccount>> list_accounts() override {
    return common::Result<std::vector<EmailAccount>>::failure(
        "capability_unavailable: email backend 'gog' is not configured");
  }

  [[nodiscard]] common::Result<std::string> draft(const EmailMessage &) override {
    return common::Result<std::string>::failure(
        "capability_unavailable: email backend 'gog' is not configured");
  }

  [[nodiscard]] common::Status send(const EmailMessage &) override {
    return common::Status::error(
        "capability_unavailable: email backend 'gog' cannot send messages");
  }
};

} // namespace

std::unique_ptr<IEmailBackend> make_email_backend(const config::Config &config) {
  const std::string backend = common::to_lower(common::trim(config.email.backend));
  if (backend == "gog") {
    return std::make_unique<GogEmailBackend>();
  }
  if (backend == "gmail") {
    return make_gmail_email_backend(config);
  }
  if (backend == "smtp") {
    return make_smtp_email_backend(config);
  }

#if defined(__APPLE__)
  if (backend.empty() || backend == "auto" || backend == "mailapp") {
    return make_mailapp_email_backend(config);
  }
#endif

  if (backend.empty() || backend == "auto") {
    return make_smtp_email_backend(config);
  }

  return std::make_unique<GogEmailBackend>();
}

} // namespace ghostclaw::email
