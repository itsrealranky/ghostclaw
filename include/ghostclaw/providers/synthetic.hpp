#pragma once

#include "ghostclaw/providers/traits.hpp"

#include <string>

namespace ghostclaw::providers {

class SyntheticProvider final : public Provider {
public:
  explicit SyntheticProvider(std::string name = "synthetic");

  [[nodiscard]] common::Result<std::string>
  chat(const std::string &message, const std::string &model, double temperature) override;

  [[nodiscard]] common::Result<std::string>
  chat_with_system(const std::optional<std::string> &system_prompt, const std::string &message,
                   const std::string &model, double temperature) override;

  [[nodiscard]] common::Status warmup() override;
  [[nodiscard]] std::string name() const override;

private:
  [[nodiscard]] static std::string summarize_message(const std::string &message);
  [[nodiscard]] static std::string build_response_text(
      const std::optional<std::string> &system_prompt, const std::string &message,
      const std::string &model, double temperature);

  std::string name_;
};

} // namespace ghostclaw::providers
