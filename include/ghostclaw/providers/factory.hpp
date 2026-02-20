#pragma once

#include "ghostclaw/common/result.hpp"
#include "ghostclaw/config/schema.hpp"
#include "ghostclaw/providers/traits.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace ghostclaw::providers {

[[nodiscard]] common::Result<std::shared_ptr<Provider>>
create_provider(const std::string &name, const std::optional<std::string> &api_key,
                std::shared_ptr<HttpClient> http_client = std::make_shared<CurlHttpClient>());

[[nodiscard]] common::Result<std::shared_ptr<Provider>>
create_reliable_provider(const std::string &primary_name, const std::optional<std::string> &api_key,
                         const config::ReliabilityConfig &reliability,
                         std::shared_ptr<HttpClient> http_client =
                             std::make_shared<CurlHttpClient>());

[[nodiscard]] common::Status precompile_provider_state(const config::Config &config,
                                                       const std::filesystem::path &workspace);
void clear_precompiled_provider_state();

} // namespace ghostclaw::providers
