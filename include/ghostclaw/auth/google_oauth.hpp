#pragma once

#include "ghostclaw/common/result.hpp"
#include "ghostclaw/config/schema.hpp"
#include "ghostclaw/providers/traits.hpp"

#include <cstdint>
#include <string>

namespace ghostclaw::auth {

struct GoogleTokens {
  std::string access_token;
  std::string refresh_token;
  std::int64_t expires_at = 0;
};

[[nodiscard]] common::Result<GoogleTokens> load_google_tokens();
[[nodiscard]] common::Status save_google_tokens(const GoogleTokens &tokens);
[[nodiscard]] common::Status delete_google_tokens();
[[nodiscard]] bool has_valid_google_tokens();

[[nodiscard]] common::Status run_google_login(providers::HttpClient &http,
                                               const config::GoogleConfig &config);
[[nodiscard]] common::Result<GoogleTokens> refresh_google_token(
    providers::HttpClient &http, const config::GoogleConfig &config,
    const std::string &refresh_token);
[[nodiscard]] common::Result<std::string> get_valid_google_token(
    providers::HttpClient &http, const config::GoogleConfig &config);

} // namespace ghostclaw::auth
