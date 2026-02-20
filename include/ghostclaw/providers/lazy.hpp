#pragma once

#include "ghostclaw/providers/traits.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace ghostclaw::providers {

class LazyProvider final : public Provider {
public:
  using ProviderFactory = std::function<common::Result<std::shared_ptr<Provider>>()>;

  LazyProvider(std::string pending_name, ProviderFactory factory);

  [[nodiscard]] common::Result<std::string>
  chat(const std::string &message, const std::string &model, double temperature) override;

  [[nodiscard]] common::Result<std::string>
  chat_with_system(const std::optional<std::string> &system_prompt, const std::string &message,
                   const std::string &model, double temperature) override;

  [[nodiscard]] common::Result<std::string> chat_with_system_tools(
      const std::optional<std::string> &system_prompt, const std::string &message,
      const std::string &model, double temperature,
      const std::vector<tools::ToolSpec> &tools) override;

  [[nodiscard]] common::Result<std::string>
  chat_with_system_stream(const std::optional<std::string> &system_prompt,
                          const std::string &message, const std::string &model,
                          double temperature,
                          const StreamChunkCallback &on_chunk) override;

  [[nodiscard]] common::Status warmup() override;
  [[nodiscard]] std::string name() const override;

private:
  [[nodiscard]] common::Result<std::shared_ptr<Provider>> ensure_provider() const;

  std::string pending_name_;
  ProviderFactory factory_;
  mutable std::mutex mutex_;
  mutable std::shared_ptr<Provider> provider_;
  mutable std::optional<std::string> init_error_;
};

} // namespace ghostclaw::providers
