#pragma once

#include "ghostclaw/config/schema.hpp"
#include "ghostclaw/tools/tool.hpp"

namespace ghostclaw::tools {

class SoulUpdateTool final : public ITool {
public:
  explicit SoulUpdateTool(const config::SoulConfig &soul_config);

  [[nodiscard]] std::string_view name() const override;
  [[nodiscard]] std::string_view description() const override;
  [[nodiscard]] std::string parameters_schema() const override;
  [[nodiscard]] common::Result<ToolResult> execute(const ToolArgs &args,
                                                    const ToolContext &ctx) override;
  [[nodiscard]] bool is_safe() const override { return false; }
  [[nodiscard]] std::string_view group() const override { return "soul"; }

private:
  config::SoulConfig soul_config_;
};

class SoulReflectTool final : public ITool {
public:
  explicit SoulReflectTool(const config::SoulConfig &soul_config);

  [[nodiscard]] std::string_view name() const override;
  [[nodiscard]] std::string_view description() const override;
  [[nodiscard]] std::string parameters_schema() const override;
  [[nodiscard]] common::Result<ToolResult> execute(const ToolArgs &args,
                                                    const ToolContext &ctx) override;
  [[nodiscard]] bool is_safe() const override { return true; }
  [[nodiscard]] std::string_view group() const override { return "soul"; }

private:
  config::SoulConfig soul_config_;
};

class SoulReadTool final : public ITool {
public:
  [[nodiscard]] std::string_view name() const override;
  [[nodiscard]] std::string_view description() const override;
  [[nodiscard]] std::string parameters_schema() const override;
  [[nodiscard]] common::Result<ToolResult> execute(const ToolArgs &args,
                                                    const ToolContext &ctx) override;
  [[nodiscard]] bool is_safe() const override { return true; }
  [[nodiscard]] std::string_view group() const override { return "soul"; }
};

} // namespace ghostclaw::tools
