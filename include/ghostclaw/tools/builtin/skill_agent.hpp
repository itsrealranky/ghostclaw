#pragma once

#include "ghostclaw/tools/tool.hpp"

namespace ghostclaw::tools {

class SkillDiscoverTool final : public ITool {
public:
  [[nodiscard]] std::string_view name() const override;
  [[nodiscard]] std::string_view description() const override;
  [[nodiscard]] std::string parameters_schema() const override;
  [[nodiscard]] common::Result<ToolResult> execute(const ToolArgs &args,
                                                    const ToolContext &ctx) override;
  [[nodiscard]] bool is_safe() const override { return true; }
  [[nodiscard]] std::string_view group() const override { return "skills"; }
};

class SkillAutoInstallTool final : public ITool {
public:
  [[nodiscard]] std::string_view name() const override;
  [[nodiscard]] std::string_view description() const override;
  [[nodiscard]] std::string parameters_schema() const override;
  [[nodiscard]] common::Result<ToolResult> execute(const ToolArgs &args,
                                                    const ToolContext &ctx) override;
  [[nodiscard]] bool is_safe() const override { return false; }
  [[nodiscard]] std::string_view group() const override { return "skills"; }
};

class SkillCreateTool final : public ITool {
public:
  [[nodiscard]] std::string_view name() const override;
  [[nodiscard]] std::string_view description() const override;
  [[nodiscard]] std::string parameters_schema() const override;
  [[nodiscard]] common::Result<ToolResult> execute(const ToolArgs &args,
                                                    const ToolContext &ctx) override;
  [[nodiscard]] bool is_safe() const override { return false; }
  [[nodiscard]] std::string_view group() const override { return "skills"; }
};

} // namespace ghostclaw::tools
