#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ghostclaw::profiler {

struct ToolStats {
  std::string tool_name;
  std::uint64_t call_count = 0;
  std::uint64_t success_count = 0;
  std::uint64_t failure_count = 0;
  double avg_latency_ms = 0.0;
  double total_latency_ms = 0.0;
  double success_rate() const {
    return call_count > 0 ? (static_cast<double>(success_count) / static_cast<double>(call_count)) : 0.0;
  }
};

class ToolProfiler {
public:
  // Record a tool call result
  void record(const std::string &tool_name, bool success,
              std::chrono::milliseconds latency);

  // Get stats for all tools
  [[nodiscard]] std::vector<ToolStats> all_stats() const;

  // Get stats for a specific tool
  [[nodiscard]] ToolStats stats_for(const std::string &tool_name) const;

  // Format a human-readable report
  [[nodiscard]] std::string format_report() const;

  // Return tools sorted by failure rate (worst first)
  [[nodiscard]] std::vector<ToolStats> sorted_by_failure_rate() const;

  // Return tools sorted by average latency (slowest first)
  [[nodiscard]] std::vector<ToolStats> sorted_by_latency() const;

  // Total calls recorded
  [[nodiscard]] std::uint64_t total_calls() const;

  void reset() { stats_.clear(); }

private:
  mutable std::unordered_map<std::string, ToolStats> stats_;
};

} // namespace ghostclaw::profiler
