#include "ghostclaw/tools/builtin/profiler_tools.hpp"

#include <sstream>

namespace ghostclaw::tools {

// --- ToolProfileReportTool ---

ToolProfileReportTool::ToolProfileReportTool(
    std::shared_ptr<profiler::ToolProfiler> profiler)
    : profiler_(std::move(profiler)) {}

std::string_view ToolProfileReportTool::name() const { return "tool_profile_report"; }

std::string_view ToolProfileReportTool::description() const {
  return "Generate a performance report of tool usage: call counts, success rates, "
         "and average latency. Use this to understand which tools are slow or unreliable.";
}

std::string ToolProfileReportTool::parameters_schema() const {
  return R"json({"type":"object","properties":{"sort":{"type":"string","enum":["calls","failures","latency"],"description":"Sort order (default: calls)"}}})json";
}

common::Result<ToolResult> ToolProfileReportTool::execute(const ToolArgs &args,
                                                            const ToolContext & /*ctx*/) {
  if (profiler_->total_calls() == 0) {
    ToolResult result;
    result.success = true;
    result.output = "No tool calls recorded in this session yet.";
    return common::Result<ToolResult>::success(std::move(result));
  }

  const std::string sort_by =
      args.count("sort") > 0 ? args.at("sort") : "calls";

  std::string report;
  if (sort_by == "failures") {
    std::ostringstream out;
    out << "Tool Usage Report (sorted by failure rate)\n";
    out << std::string(60, '-') << "\n";
    for (const auto &s : profiler_->sorted_by_failure_rate()) {
      out << "- " << s.tool_name << ": " << s.call_count << " calls, "
          << (s.success_rate() * 100.0) << "% success, "
          << s.avg_latency_ms << "ms avg\n";
    }
    report = out.str();
  } else if (sort_by == "latency") {
    std::ostringstream out;
    out << "Tool Usage Report (sorted by latency)\n";
    out << std::string(60, '-') << "\n";
    for (const auto &s : profiler_->sorted_by_latency()) {
      out << "- " << s.tool_name << ": " << s.call_count << " calls, "
          << s.avg_latency_ms << "ms avg, "
          << (s.success_rate() * 100.0) << "% success\n";
    }
    report = out.str();
  } else {
    report = profiler_->format_report();
  }

  ToolResult result;
  result.success = true;
  result.output = report;
  result.metadata["total_calls"] = std::to_string(profiler_->total_calls());
  return common::Result<ToolResult>::success(std::move(result));
}

// --- SelfOptimizeTool ---

SelfOptimizeTool::SelfOptimizeTool(std::shared_ptr<profiler::ToolProfiler> profiler)
    : profiler_(std::move(profiler)) {}

std::string_view SelfOptimizeTool::name() const { return "self_optimize"; }

std::string_view SelfOptimizeTool::description() const {
  return "Analyze tool usage patterns and generate self-optimization recommendations. "
         "Returns specific suggestions for improving agent performance based on profiling data.";
}

std::string SelfOptimizeTool::parameters_schema() const {
  return R"({"type":"object","properties":{}})";
}

common::Result<ToolResult> SelfOptimizeTool::execute(const ToolArgs & /*args*/,
                                                       const ToolContext & /*ctx*/) {
  if (profiler_->total_calls() == 0) {
    ToolResult result;
    result.success = true;
    result.output =
        "No tool usage data available yet. Complete some tasks first, then run self_optimize.";
    return common::Result<ToolResult>::success(std::move(result));
  }

  std::ostringstream recommendations;
  recommendations << "Self-Optimization Recommendations\n";
  recommendations << std::string(50, '=') << "\n\n";

  bool has_recommendations = false;

  // Find high-failure tools
  for (const auto &s : profiler_->sorted_by_failure_rate()) {
    if (s.call_count < 3) {
      continue;
    }
    if (s.success_rate() < 0.7) {
      recommendations << "HIGH FAILURE RATE: " << s.tool_name << " ("
                      << (s.success_rate() * 100.0) << "% success over "
                      << s.call_count << " calls)\n";
      recommendations << "  -> Consider checking arguments passed to this tool.\n";
      recommendations << "  -> Consider using an alternative if available.\n\n";
      has_recommendations = true;
    }
  }

  // Find slow tools
  for (const auto &s : profiler_->sorted_by_latency()) {
    if (s.call_count < 3) {
      continue;
    }
    if (s.avg_latency_ms > 5000.0) {
      recommendations << "SLOW TOOL: " << s.tool_name << " ("
                      << s.avg_latency_ms << "ms avg over "
                      << s.call_count << " calls)\n";
      recommendations << "  -> Batch calls where possible.\n";
      recommendations << "  -> Cache results if the same query is repeated.\n\n";
      has_recommendations = true;
    }
  }

  // Find most-used tools for skill acquisition hints
  const auto all = profiler_->all_stats();
  std::uint64_t max_calls = 0;
  std::string top_tool;
  for (const auto &s : all) {
    if (s.call_count > max_calls) {
      max_calls = s.call_count;
      top_tool = s.tool_name;
    }
  }
  if (!top_tool.empty() && max_calls >= 10) {
    recommendations << "MOST USED: " << top_tool << " (" << max_calls << " calls)\n";
    recommendations << "  -> Consider creating a skill that encodes best practices "
                       "for this tool.\n\n";
    has_recommendations = true;
  }

  if (!has_recommendations) {
    recommendations << "Tool usage looks healthy. No specific recommendations.\n";
    recommendations << "Total calls: " << profiler_->total_calls() << "\n";
  }

  ToolResult result;
  result.success = true;
  result.output = recommendations.str();
  return common::Result<ToolResult>::success(std::move(result));
}

} // namespace ghostclaw::tools
