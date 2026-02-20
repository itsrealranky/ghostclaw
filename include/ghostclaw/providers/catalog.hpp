#pragma once

#include "ghostclaw/common/result.hpp"
#include "ghostclaw/config/schema.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace ghostclaw::providers {

struct ProviderInfo {
  std::string id;
  std::string display_name;
  std::vector<std::string> aliases;
  bool local = false;
  bool supports_model_catalog = false;
};

struct ModelCatalog {
  std::string provider;
  std::vector<std::string> models;
  std::chrono::system_clock::time_point updated_at{};
  bool from_cache = false;
};

[[nodiscard]] const std::vector<ProviderInfo> &provider_catalog();
[[nodiscard]] std::optional<ProviderInfo> find_provider(const std::string &id_or_alias);

[[nodiscard]] common::Result<ModelCatalog>
refresh_model_catalog(const config::Config &config, const std::string &provider,
                      bool force_refresh);
[[nodiscard]] common::Result<std::vector<ModelCatalog>>
refresh_model_catalogs(const config::Config &config, bool force_refresh);

} // namespace ghostclaw::providers
