#include "ghostclaw/profiler/tool_profiler.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace ghostclaw::profiler {

void ToolProfiler::record(const std::string &tool_name, bool success,
                           std::chrono::milliseconds latency) {
  auto &s = stats_[tool_name];
  s.tool_name = tool_name;
  s.call_count++;
  if (success) {
    s.success_count++;
  } else {
    s.failure_count++;
  }
  const double lat = static_cast<double>(latency.count());
  s.total_latency_ms += lat;
  s.avg_latency_ms = s.total_latency_ms / static_cast<double>(s.call_count);
}

std::vector<ToolStats> ToolProfiler::all_stats() const {
  std::vector<ToolStats> out;
  out.reserve(stats_.size());
  for (const auto &[name, stat] : stats_) {
    out.push_back(stat);
  }
  return out;
}

ToolStats ToolProfiler::stats_for(const std::string &tool_name) const {
  const auto it = stats_.find(tool_name);
  if (it == stats_.end()) {
    ToolStats empty;
    empty.tool_name = tool_name;
    return empty;
  }
  return it->second;
}

std::string ToolProfiler::format_report() const {
  if (stats_.empty()) {
    return "No tool calls recorded yet.\n";
  }

  auto sorted = all_stats();
  std::sort(sorted.begin(), sorted.end(), [](const ToolStats &a, const ToolStats &b) {
    return a.call_count > b.call_count;
  });

  std::ostringstream out;
  out << "Tool Usage Report\n";
  out << std::string(60, '-') << "\n";
  out << std::left << std::setw(30) << "Tool"
      << std::right << std::setw(6) << "Calls"
      << std::setw(8) << "OK%"
      << std::setw(12) << "Avg ms" << "\n";
  out << std::string(60, '-') << "\n";

  for (const auto &s : sorted) {
    out << std::left << std::setw(30) << s.tool_name
        << std::right << std::setw(6) << s.call_count
        << std::setw(7) << std::fixed << std::setprecision(1)
        << (s.success_rate() * 100.0) << "%"
        << std::setw(12) << std::fixed << std::setprecision(1)
        << s.avg_latency_ms << "\n";
  }
  return out.str();
}

std::vector<ToolStats> ToolProfiler::sorted_by_failure_rate() const {
  auto sorted = all_stats();
  std::sort(sorted.begin(), sorted.end(), [](const ToolStats &a, const ToolStats &b) {
    return (1.0 - a.success_rate()) > (1.0 - b.success_rate());
  });
  return sorted;
}

std::vector<ToolStats> ToolProfiler::sorted_by_latency() const {
  auto sorted = all_stats();
  std::sort(sorted.begin(), sorted.end(), [](const ToolStats &a, const ToolStats &b) {
    return a.avg_latency_ms > b.avg_latency_ms;
  });
  return sorted;
}

std::uint64_t ToolProfiler::total_calls() const {
  std::uint64_t total = 0;
  for (const auto &[name, stat] : stats_) {
    total += stat.call_count;
  }
  return total;
}

} // namespace ghostclaw::profiler
