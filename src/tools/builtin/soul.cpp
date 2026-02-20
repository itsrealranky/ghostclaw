#include "ghostclaw/tools/builtin/soul.hpp"

#include "ghostclaw/soul/manager.hpp"

namespace ghostclaw::tools {

// --- SoulUpdateTool ---

SoulUpdateTool::SoulUpdateTool(const config::SoulConfig &soul_config)
    : soul_config_(soul_config) {}

std::string_view SoulUpdateTool::name() const { return "soul_update"; }

std::string_view SoulUpdateTool::description() const {
  return "Update a section of your SOUL.md identity document. "
         "Sections: Identity, Values, Capabilities, Goals.";
}

std::string SoulUpdateTool::parameters_schema() const {
  return R"json({"type":"object","required":["section","content"],"properties":{"section":{"type":"string","description":"Section heading to update (e.g. Identity, Values, Capabilities, Goals)"},"content":{"type":"string","description":"New content for the section"}}})json";
}

common::Result<ToolResult> SoulUpdateTool::execute(const ToolArgs &args,
                                                     const ToolContext &ctx) {
  const auto section_it = args.find("section");
  if (section_it == args.end() || section_it->second.empty()) {
    return common::Result<ToolResult>::failure("Missing section");
  }
  const auto content_it = args.find("content");
  if (content_it == args.end()) {
    return common::Result<ToolResult>::failure("Missing content");
  }

  soul::SoulManager manager(ctx.workspace_path, soul_config_.protected_sections,
                             soul_config_.max_reflections, soul_config_.git_versioned);

  const auto status = manager.update_section(section_it->second, content_it->second);
  if (!status.ok()) {
    return common::Result<ToolResult>::failure(status.error());
  }

  ToolResult result;
  result.success = true;
  result.output = "Updated SOUL.md section: " + section_it->second;
  result.metadata["section"] = section_it->second;
  return common::Result<ToolResult>::success(std::move(result));
}

// --- SoulReflectTool ---

SoulReflectTool::SoulReflectTool(const config::SoulConfig &soul_config)
    : soul_config_(soul_config) {}

std::string_view SoulReflectTool::name() const { return "soul_reflect"; }

std::string_view SoulReflectTool::description() const {
  return "Append a timestamped reflection to your SOUL.md. "
         "Use this to record insights, decisions, and observations about your operation.";
}

std::string SoulReflectTool::parameters_schema() const {
  return R"json({"type":"object","required":["reflection"],"properties":{"reflection":{"type":"string","description":"The reflection to append (a single sentence or short paragraph)"}}})json";
}

common::Result<ToolResult> SoulReflectTool::execute(const ToolArgs &args,
                                                      const ToolContext &ctx) {
  const auto reflection_it = args.find("reflection");
  if (reflection_it == args.end() || reflection_it->second.empty()) {
    return common::Result<ToolResult>::failure("Missing reflection");
  }

  soul::SoulManager manager(ctx.workspace_path, soul_config_.protected_sections,
                             soul_config_.max_reflections, soul_config_.git_versioned);

  const auto status = manager.append_reflection(reflection_it->second);
  if (!status.ok()) {
    return common::Result<ToolResult>::failure(status.error());
  }

  ToolResult result;
  result.success = true;
  result.output = "Reflection recorded in SOUL.md.";
  return common::Result<ToolResult>::success(std::move(result));
}

// --- SoulReadTool ---

std::string_view SoulReadTool::name() const { return "soul_read"; }

std::string_view SoulReadTool::description() const {
  return "Read the current SOUL.md identity document.";
}

std::string SoulReadTool::parameters_schema() const {
  return R"({"type":"object","properties":{}})";
}

common::Result<ToolResult> SoulReadTool::execute(const ToolArgs & /*args*/,
                                                   const ToolContext &ctx) {
  soul::SoulManager manager(ctx.workspace_path);
  const std::string content = manager.load();

  ToolResult result;
  result.success = true;
  if (content.empty()) {
    result.output = "SOUL.md does not exist yet. Use soul_update to create it.";
  } else {
    result.output = content;
  }
  return common::Result<ToolResult>::success(std::move(result));
}

} // namespace ghostclaw::tools
