#include "ghostclaw/tools/builtin/skill_agent.hpp"

#include "ghostclaw/common/fs.hpp"
#include "ghostclaw/skills/registry.hpp"

#include <fstream>
#include <sstream>

namespace ghostclaw::tools {

// --- SkillDiscoverTool ---

std::string_view SkillDiscoverTool::name() const { return "skill_discover"; }

std::string_view SkillDiscoverTool::description() const {
  return "Discover available skills by searching the workspace skills registry. "
         "Returns matching skills with descriptions and sources.";
}

std::string SkillDiscoverTool::parameters_schema() const {
  return R"json({"type":"object","required":["query"],"properties":{"query":{"type":"string","description":"Search query to find relevant skills"},"limit":{"type":"string","description":"Maximum number of results (default: 10)"}}})json";
}

common::Result<ToolResult> SkillDiscoverTool::execute(const ToolArgs &args,
                                                        const ToolContext &ctx) {
  const auto query_it = args.find("query");
  if (query_it == args.end() || query_it->second.empty()) {
    return common::Result<ToolResult>::failure("Missing query");
  }

  std::size_t limit = 10;
  const auto limit_it = args.find("limit");
  if (limit_it != args.end()) {
    try {
      limit = static_cast<std::size_t>(std::stoull(limit_it->second));
    } catch (...) {
      // Use default
    }
  }

  skills::SkillRegistry registry(ctx.workspace_path / "skills",
                                   ctx.workspace_path / ".community-skills");
  auto searched = registry.search(query_it->second, true);
  if (!searched.ok()) {
    return common::Result<ToolResult>::failure(searched.error());
  }

  std::ostringstream out;
  out << "Skills matching '" << query_it->second << "':\n";
  std::size_t emitted = 0;
  for (const auto &entry : searched.value()) {
    if (emitted >= limit) {
      break;
    }
    out << "- " << entry.skill.name
        << " [" << skills::skill_source_to_string(entry.skill.source) << "]"
        << " (score=" << entry.score << ")";
    if (!entry.skill.description.empty()) {
      out << " - " << entry.skill.description;
    }
    out << "\n";
    ++emitted;
  }

  if (searched.value().empty()) {
    out << "(no matches found)\n";
    out << "To install a skill from GitHub: use skill_auto_install with "
           "source=github and repo=<owner/repo>\n";
  }

  ToolResult result;
  result.success = true;
  result.output = out.str();
  result.metadata["count"] = std::to_string(searched.value().size());
  return common::Result<ToolResult>::success(std::move(result));
}

// --- SkillAutoInstallTool ---

std::string_view SkillAutoInstallTool::name() const { return "skill_auto_install"; }

std::string_view SkillAutoInstallTool::description() const {
  return "Autonomously install a skill from a GitHub repository or local path. "
         "The skill will be immediately available after installation.";
}

std::string SkillAutoInstallTool::parameters_schema() const {
  return R"json({"type":"object","required":["source"],"properties":{"source":{"type":"string","enum":["github","local"],"description":"Installation source"},"repo":{"type":"string","description":"GitHub repo (owner/repo) when source=github"},"path":{"type":"string","description":"Local directory path when source=local"},"branch":{"type":"string","description":"Git branch (default: main) when source=github"},"skills_subdir":{"type":"string","description":"Subdirectory in repo containing skills (default: skills)"}}})json";
}

common::Result<ToolResult> SkillAutoInstallTool::execute(const ToolArgs &args,
                                                           const ToolContext &ctx) {
  const auto source_it = args.find("source");
  if (source_it == args.end() || source_it->second.empty()) {
    return common::Result<ToolResult>::failure("Missing source");
  }

  skills::SkillRegistry registry(ctx.workspace_path / "skills",
                                   ctx.workspace_path / ".community-skills");

  const std::string &source = source_it->second;

  if (source == "github") {
    const auto repo_it = args.find("repo");
    if (repo_it == args.end() || repo_it->second.empty()) {
      return common::Result<ToolResult>::failure(
          "Missing repo (format: owner/repo) for github source");
    }

    const std::string branch =
        args.count("branch") > 0 ? args.at("branch") : "main";
    const std::string skills_subdir =
        args.count("skills_subdir") > 0 ? args.at("skills_subdir") : "skills";

    auto sync_result = registry.sync_github(repo_it->second, branch, skills_subdir, false);
    if (!sync_result.ok()) {
      return common::Result<ToolResult>::failure(
          "Failed to install from GitHub: " + sync_result.error());
    }

    ToolResult result;
    result.success = true;
    result.output = "Installed " + std::to_string(sync_result.value()) +
                    " skill(s) from " + repo_it->second;
    result.metadata["count"] = std::to_string(sync_result.value());
    result.metadata["source"] = repo_it->second;
    return common::Result<ToolResult>::success(std::move(result));
  }

  if (source == "local") {
    const auto path_it = args.find("path");
    if (path_it == args.end() || path_it->second.empty()) {
      return common::Result<ToolResult>::failure("Missing path for local source");
    }

    const std::filesystem::path skill_dir =
        std::filesystem::path(common::expand_path(path_it->second));
    auto install_result = registry.install(skill_dir);
    if (!install_result.ok()) {
      return common::Result<ToolResult>::failure(
          "Failed to install skill: " + install_result.error());
    }

    ToolResult result;
    result.success = true;
    result.output = install_result.value() ? "Skill installed successfully from " + path_it->second
                                            : "Skill was already installed";
    return common::Result<ToolResult>::success(std::move(result));
  }

  return common::Result<ToolResult>::failure("Unknown source: " + source);
}

// --- SkillCreateTool ---

std::string_view SkillCreateTool::name() const { return "skill_create"; }

std::string_view SkillCreateTool::description() const {
  return "Create a new skill from experience and save it to the workspace skills directory. "
         "Skills are reusable SKILL.md files injected into the system prompt.";
}

std::string SkillCreateTool::parameters_schema() const {
  return R"json({"type":"object","required":["name","instructions"],"properties":{"name":{"type":"string","description":"Skill name (no spaces, e.g. deploy-webapp)"},"description":{"type":"string","description":"Short description of what the skill does"},"instructions":{"type":"string","description":"The skill instructions in markdown format"},"auto_activate":{"type":"string","description":"Whether to auto-activate this skill (true/false, default: false)"},"version":{"type":"string","description":"Skill version (default: 1.0.0)"}}})json";
}

common::Result<ToolResult> SkillCreateTool::execute(const ToolArgs &args,
                                                      const ToolContext &ctx) {
  const auto name_it = args.find("name");
  if (name_it == args.end() || name_it->second.empty()) {
    return common::Result<ToolResult>::failure("Missing name");
  }
  const auto instructions_it = args.find("instructions");
  if (instructions_it == args.end() || instructions_it->second.empty()) {
    return common::Result<ToolResult>::failure("Missing instructions");
  }

  const std::string skill_name = name_it->second;
  const std::string description =
      args.count("description") > 0 ? args.at("description") : "";
  const std::string version =
      args.count("version") > 0 ? args.at("version") : "1.0.0";
  const bool auto_activate =
      args.count("auto_activate") > 0 &&
      (args.at("auto_activate") == "true" || args.at("auto_activate") == "1");

  // Create skills directory
  const auto skills_dir = ctx.workspace_path / "skills" / skill_name;
  std::error_code ec;
  std::filesystem::create_directories(skills_dir, ec);
  if (ec) {
    return common::Result<ToolResult>::failure(
        "Failed to create skill directory: " + ec.message());
  }

  // Build SKILL.md content
  std::ostringstream skill_content;
  skill_content << "---\n";
  skill_content << "name: " << skill_name << "\n";
  skill_content << "version: " << version << "\n";
  if (!description.empty()) {
    skill_content << "description: " << description << "\n";
  }
  if (auto_activate) {
    skill_content << "auto-activate: true\n";
  }
  skill_content << "---\n\n";
  skill_content << instructions_it->second;
  if (skill_content.str().back() != '\n') {
    skill_content << "\n";
  }

  const auto skill_file = skills_dir / "SKILL.md";
  std::ofstream out(skill_file, std::ios::trunc);
  if (!out) {
    return common::Result<ToolResult>::failure("Failed to create SKILL.md");
  }
  out << skill_content.str();
  out.close();
  if (!out) {
    return common::Result<ToolResult>::failure("Failed to write SKILL.md");
  }

  ToolResult result;
  result.success = true;
  result.output = "Created skill '" + skill_name + "' at " + skill_file.string();
  result.metadata["name"] = skill_name;
  result.metadata["path"] = skill_file.string();
  return common::Result<ToolResult>::success(std::move(result));
}

} // namespace ghostclaw::tools
