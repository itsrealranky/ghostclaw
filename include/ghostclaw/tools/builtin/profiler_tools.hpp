#pragma once

#include "ghostclaw/profiler/tool_profiler.hpp"
#include "ghostclaw/tools/tool.hpp"

#include <memory>

namespace ghostclaw::tools {

class ToolProfileReportTool final : public ITool {
public:
  explicit ToolProfileReportTool(std::shared_ptr<profiler::ToolProfiler> profiler);

  [[nodiscard]] std::string_view name() const override;
  [[nodiscard]] std::string_view description() const override;
  [[nodiscard]] std::string parameters_schema() const override;
  [[nodiscard]] common::Result<ToolResult> execute(const ToolArgs &args,
                                                    const ToolContext &ctx) override;
  [[nodiscard]] bool is_safe() const override { return true; }
  [[nodiscard]] std::string_view group() const override { return "profiler"; }

private:
  std::shared_ptr<profiler::ToolProfiler> profiler_;
};

class SelfOptimizeTool final : public ITool {
public:
  explicit SelfOptimizeTool(std::shared_ptr<profiler::ToolProfiler> profiler);

  [[nodiscard]] std::string_view name() const override;
  [[nodiscard]] std::string_view description() const override;
  [[nodiscard]] std::string parameters_schema() const override;
  [[nodiscard]] common::Result<ToolResult> execute(const ToolArgs &args,
                                                    const ToolContext &ctx) override;
  [[nodiscard]] bool is_safe() const override { return true; }
  [[nodiscard]] std::string_view group() const override { return "profiler"; }

private:
  std::shared_ptr<profiler::ToolProfiler> profiler_;
};

} // namespace ghostclaw::tools
