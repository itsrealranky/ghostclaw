#include "ghostclaw/soul/manager.hpp"

#include "ghostclaw/common/fs.hpp"

#include <array>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>

namespace ghostclaw::soul {

namespace {

constexpr const char *DEFAULT_SOUL_TEMPLATE = R"(# Identity
GhostClaw — autonomous coding assistant. I help humans build software, understand systems,
and solve problems. I operate with precision and care.

# Values
- Accuracy: I prefer getting things right over getting them done fast.
- Transparency: I explain my reasoning and surface uncertainty.
- Autonomy: I take initiative within my configured boundaries.
- Continuity: I learn from each session and build on past work.

# Capabilities
Core: code generation, file editing, shell commands, web search, memory recall.
Skills: auto-discovered from workspace skills directory.

# Goals
Serve the operator and user effectively. Improve continuously.

# Reflections
)";

constexpr std::size_t kMaxSummaryChars = 1500;

std::string current_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto t = std::chrono::system_clock::to_time_t(now);
  std::tm local_tm{};
#ifdef _WIN32
  localtime_s(&local_tm, &t);
#else
  localtime_r(&t, &local_tm);
#endif
  std::ostringstream out;
  out << std::put_time(&local_tm, "%Y-%m-%d %H:%M");
  return out.str();
}

} // namespace

SoulManager::SoulManager(std::filesystem::path workspace_path,
                         const std::vector<std::string> &protected_sections,
                         std::uint32_t max_reflections,
                         bool git_versioned)
    : soul_path_(std::move(workspace_path) / "SOUL.md"),
      protected_sections_(protected_sections),
      max_reflections_(max_reflections),
      git_versioned_(git_versioned) {}

std::string SoulManager::load() const {
  if (!std::filesystem::exists(soul_path_)) {
    return "";
  }
  std::ifstream in(soul_path_);
  if (!in) {
    return "";
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

common::Status SoulManager::initialize(const std::string &name) {
  if (std::filesystem::exists(soul_path_)) {
    return common::Status::success();
  }

  std::error_code ec;
  const auto parent = soul_path_.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      return common::Status::error("Failed to create soul directory: " + ec.message());
    }
  }

  std::string content = DEFAULT_SOUL_TEMPLATE;
  // Replace "GhostClaw" with the agent's name if different
  if (name != "GhostClaw" && !name.empty()) {
    content = std::regex_replace(content, std::regex("GhostClaw"), name);
  }

  std::ofstream out(soul_path_, std::ios::trunc);
  if (!out) {
    return common::Status::error("Failed to create SOUL.md");
  }
  out << content;
  out.close();
  if (!out) {
    return common::Status::error("Failed to write SOUL.md");
  }

  if (git_versioned_) {
    git_commit("Initialize SOUL.md");
  }
  return common::Status::success();
}

common::Status SoulManager::update_section(const std::string &section,
                                            const std::string &content) {
  if (is_protected(section)) {
    return common::Status::error("Section '" + section + "' is protected and cannot be modified");
  }

  std::string current = load();
  if (current.empty()) {
    const auto init_status = initialize();
    if (!init_status.ok()) {
      return init_status;
    }
    current = load();
  }

  // Find the section heading and replace its content
  const std::string heading = "# " + section;
  const auto heading_pos = current.find(heading);

  if (heading_pos == std::string::npos) {
    // Section not found — append it
    if (!current.empty() && current.back() != '\n') {
      current += '\n';
    }
    current += "\n" + heading + "\n" + content + "\n";
  } else {
    // Find where this section's content ends (next heading or end of file)
    const auto content_start = heading_pos + heading.size();
    const auto next_heading = current.find("\n# ", content_start);
    const auto section_end =
        (next_heading != std::string::npos) ? next_heading : current.size();

    std::string updated;
    updated.reserve(current.size() + content.size() + 2);
    updated.append(current, 0, content_start);
    updated.push_back('\n');
    updated.append(content);
    updated.push_back('\n');
    updated.append(current, section_end, std::string::npos);
    current = std::move(updated);
  }

  std::ofstream out(soul_path_, std::ios::trunc);
  if (!out) {
    return common::Status::error("Failed to open SOUL.md for writing");
  }
  out << current;
  out.close();
  if (!out) {
    return common::Status::error("Failed to write SOUL.md");
  }

  if (git_versioned_) {
    git_commit("Update SOUL.md section: " + section);
  }
  return common::Status::success();
}

common::Status SoulManager::append_reflection(const std::string &reflection) {
  std::string current = load();
  if (current.empty()) {
    const auto init_status = initialize();
    if (!init_status.ok()) {
      return init_status;
    }
    current = load();
  }

  // Check reflection count and trim oldest if over limit
  if (max_reflections_ > 0) {
    const std::size_t count = count_reflections(current);
    if (count >= max_reflections_) {
      // Remove the oldest reflection (first "- [" entry after "# Reflections")
      const auto reflect_heading = current.find("# Reflections");
      if (reflect_heading != std::string::npos) {
        const auto first_entry = current.find("\n- [", reflect_heading);
        if (first_entry != std::string::npos) {
          const auto next_entry = current.find("\n- [", first_entry + 1);
          if (next_entry != std::string::npos) {
            current.erase(first_entry, next_entry - first_entry);
          }
        }
      }
    }
  }

  // Find the Reflections section or append it
  const std::string heading = "# Reflections";
  auto heading_pos = current.find(heading);
  if (heading_pos == std::string::npos) {
    if (!current.empty() && current.back() != '\n') {
      current += '\n';
    }
    current += "\n" + heading + "\n";
    heading_pos = current.size() - 1;
  }

  // Find the end of the file or next heading after Reflections
  const auto content_start = heading_pos + heading.size();
  const auto next_heading = current.find("\n# ", content_start);
  const auto insert_pos =
      (next_heading != std::string::npos) ? next_heading : current.size();

  const std::string entry = "\n- [" + current_timestamp() + "] " + reflection;
  current.insert(insert_pos, entry);

  std::ofstream out(soul_path_, std::ios::trunc);
  if (!out) {
    return common::Status::error("Failed to open SOUL.md for writing");
  }
  out << current;
  out.close();
  if (!out) {
    return common::Status::error("Failed to write SOUL.md");
  }

  if (git_versioned_) {
    git_commit("Add reflection to SOUL.md");
  }
  return common::Status::success();
}

std::string SoulManager::summary() const {
  std::string content = load();
  if (content.empty()) {
    return "";
  }
  if (content.size() > kMaxSummaryChars) {
    content.resize(kMaxSummaryChars);
    content += "\n[truncated]";
  }
  return content;
}

bool SoulManager::is_protected(const std::string &section) const {
  for (const auto &ps : protected_sections_) {
    if (common::to_lower(ps) == common::to_lower(section)) {
      return true;
    }
  }
  return false;
}

void SoulManager::git_commit(const std::string &message) const {
  // Best-effort git commit of SOUL.md — ignore all errors silently
  const auto parent = soul_path_.parent_path();
  const std::string soul_name = soul_path_.filename().string();

  // Single-quote the path components to handle spaces; message gets double-quoting
  auto sq = [](const std::string &s) -> std::string {
    std::string out = "'";
    for (char c : s) {
      if (c == '\'') {
        out += "'\\''";
      } else {
        out += c;
      }
    }
    out += "'";
    return out;
  };

  const std::string add_cmd =
      "git -C " + sq(parent.string()) + " add " + sq(soul_name) + " 2>/dev/null";
  const std::string commit_cmd =
      "git -C " + sq(parent.string()) + " commit -m " + sq(message) +
      " -- " + sq(soul_name) + " 2>/dev/null";
  (void)std::system(add_cmd.c_str());
  (void)std::system(commit_cmd.c_str());
}

std::size_t SoulManager::count_reflections(const std::string &content) const {
  const std::string heading = "# Reflections";
  const auto heading_pos = content.find(heading);
  if (heading_pos == std::string::npos) {
    return 0;
  }
  std::size_t count = 0;
  std::size_t pos = heading_pos;
  while ((pos = content.find("\n- [", pos + 1)) != std::string::npos) {
    ++count;
  }
  return count;
}

} // namespace ghostclaw::soul
