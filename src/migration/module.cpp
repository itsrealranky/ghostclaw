#include "ghostclaw/migration/module.hpp"

#include "ghostclaw/common/fs.hpp"
#include "ghostclaw/common/json_util.hpp"
#include "ghostclaw/config/config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace ghostclaw::migration {

namespace {

struct LegacyDefaults {
  std::string provider = "anthropic";
  std::string model = "claude-sonnet-4-5";
  std::filesystem::path workspace_path;
};

std::string trim_copy(std::string value) { return common::trim(std::move(value)); }

std::string normalize_provider(std::string provider, const std::string &fallback_provider) {
  provider = common::to_lower(trim_copy(std::move(provider)));
  if (provider.empty()) {
    provider = common::to_lower(trim_copy(fallback_provider));
  }
  if (provider == "claude") {
    return "anthropic";
  }
  if (provider == "codex") {
    return "openai";
  }
  if (provider.empty()) {
    return "anthropic";
  }
  return provider;
}

std::string normalize_model(std::string provider, std::string model) {
  provider = common::to_lower(trim_copy(std::move(provider)));
  const std::string lower_model = common::to_lower(trim_copy(model));

  if (provider == "anthropic") {
    if (lower_model.empty() || lower_model == "sonnet") {
      return "claude-sonnet-4-5";
    }
    if (lower_model == "opus") {
      return "claude-opus-4-6";
    }
    return trim_copy(std::move(model));
  }

  if (provider == "openai") {
    if (lower_model.empty()) {
      return "gpt-5.3-codex";
    }
    return trim_copy(std::move(model));
  }

  return trim_copy(std::move(model));
}

common::Result<std::string> read_text_file(const std::filesystem::path &path) {
  std::ifstream file(path);
  if (!file) {
    return common::Result<std::string>::failure("unable to open settings file: " + path.string());
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return common::Result<std::string>::success(buffer.str());
}

common::Result<std::unordered_map<std::string, std::string>>
parse_object_members(const std::string &object_json) {
  std::unordered_map<std::string, std::string> values;

  std::size_t pos = common::json_skip_ws(object_json, 0);
  if (pos >= object_json.size() || object_json[pos] != '{') {
    return common::Result<std::unordered_map<std::string, std::string>>::failure(
        "expected JSON object");
  }
  ++pos;

  while (pos < object_json.size()) {
    pos = common::json_skip_ws(object_json, pos);
    if (pos >= object_json.size()) {
      break;
    }
    if (object_json[pos] == '}') {
      break;
    }
    if (object_json[pos] == ',') {
      ++pos;
      continue;
    }
    if (object_json[pos] != '"') {
      return common::Result<std::unordered_map<std::string, std::string>>::failure(
          "invalid JSON object member (expected quoted key)");
    }

    const auto key_end = common::json_find_string_end(object_json, pos);
    if (key_end == std::string::npos || key_end <= pos) {
      return common::Result<std::unordered_map<std::string, std::string>>::failure(
          "unterminated JSON key string");
    }
    const std::string key = common::json_unescape(object_json.substr(pos + 1, key_end - pos - 1));
    pos = key_end + 1;
    pos = common::json_skip_ws(object_json, pos);
    if (pos >= object_json.size() || object_json[pos] != ':') {
      return common::Result<std::unordered_map<std::string, std::string>>::failure(
          "missing ':' after JSON key");
    }
    ++pos;
    pos = common::json_skip_ws(object_json, pos);
    if (pos >= object_json.size()) {
      return common::Result<std::unordered_map<std::string, std::string>>::failure(
          "missing JSON value");
    }

    std::size_t value_end = std::string::npos;
    if (object_json[pos] == '"') {
      value_end = common::json_find_string_end(object_json, pos);
    } else if (object_json[pos] == '{') {
      value_end = common::json_find_matching_token(object_json, pos, '{', '}');
    } else if (object_json[pos] == '[') {
      value_end = common::json_find_matching_token(object_json, pos, '[', ']');
    } else {
      value_end = pos;
      while (value_end < object_json.size() && object_json[value_end] != ',' &&
             object_json[value_end] != '}' &&
             std::isspace(static_cast<unsigned char>(object_json[value_end])) == 0) {
        ++value_end;
      }
      if (value_end > pos) {
        --value_end;
      }
    }

    if (value_end == std::string::npos || value_end < pos || value_end >= object_json.size()) {
      return common::Result<std::unordered_map<std::string, std::string>>::failure(
          "invalid JSON value for key '" + key + "'");
    }

    values[key] = object_json.substr(pos, value_end - pos + 1);
    pos = value_end + 1;
  }

  return common::Result<std::unordered_map<std::string, std::string>>::success(std::move(values));
}

std::vector<std::string> parse_raw_string_array(const std::string &array_json) {
  std::vector<std::string> out;
  std::string wrapped = "{\"v\":" + array_json + "}";
  auto parsed = common::json_get_string_array(wrapped, "v");
  out.reserve(parsed.size());
  for (auto &item : parsed) {
    item = trim_copy(std::move(item));
    if (!item.empty()) {
      out.push_back(std::move(item));
    }
  }
  return out;
}

std::vector<std::pair<std::string, std::string>>
sorted_entries(const std::unordered_map<std::string, std::string> &fields) {
  std::vector<std::pair<std::string, std::string>> entries;
  entries.reserve(fields.size());
  for (const auto &[key, value] : fields) {
    entries.emplace_back(key, value);
  }
  std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
    return a.first < b.first;
  });
  return entries;
}

std::string compatibility_env_home_name() { return std::string("TINY") + "CLAW_HOME"; }

std::string compatibility_settings_dir_name() { return "." + std::string("tiny") + "claw"; }

LegacyDefaults derive_defaults(const std::string &settings_json,
                               const std::unordered_map<std::string, std::string> &root_fields) {
  LegacyDefaults defaults;
  auto default_workspace = config::workspace_dir();
  if (default_workspace.ok()) {
    defaults.workspace_path = default_workspace.value() / "legacy-import";
  } else {
    defaults.workspace_path = std::filesystem::path(common::home_dir().ok()
                                                        ? common::home_dir().value().string()
                                                        : ".") /
                              ".ghostclaw" / "workspace" / "legacy-import";
  }

  const auto workspace_it = root_fields.find("workspace");
  if (workspace_it != root_fields.end()) {
    const std::string workspace_path = common::json_get_string(workspace_it->second, "path");
    if (!trim_copy(workspace_path).empty()) {
      defaults.workspace_path = std::filesystem::path(common::expand_path(workspace_path));
    }
  }

  std::string models_json;
  const auto models_it = root_fields.find("models");
  if (models_it != root_fields.end()) {
    models_json = models_it->second;
  } else {
    models_json = common::json_get_object(settings_json, "models");
  }

  if (!models_json.empty()) {
    std::string provider = common::json_get_string(models_json, "provider");
    if (provider.empty()) {
      const std::string openai_block = common::json_get_object(models_json, "openai");
      const std::string anthropic_block = common::json_get_object(models_json, "anthropic");
      if (!openai_block.empty()) {
        provider = "openai";
      } else if (!anthropic_block.empty()) {
        provider = "anthropic";
      }
    }
    defaults.provider = normalize_provider(provider, defaults.provider);

    if (defaults.provider == "openai") {
      const std::string openai_block = common::json_get_object(models_json, "openai");
      const std::string model = common::json_get_string(openai_block, "model");
      defaults.model = normalize_model(defaults.provider, model);
    } else {
      const std::string anthropic_block = common::json_get_object(models_json, "anthropic");
      const std::string model = common::json_get_string(anthropic_block, "model");
      defaults.model = normalize_model(defaults.provider, model);
    }
  }

  return defaults;
}

config::AgentConfig parse_legacy_agent(const std::string &agent_id, const std::string &agent_json,
                                       const LegacyDefaults &defaults) {
  config::AgentConfig out;
  out.id = trim_copy(agent_id);
  const std::string name = common::json_get_string(agent_json, "name");
  std::string provider = common::json_get_string(agent_json, "provider");
  provider = normalize_provider(std::move(provider), defaults.provider);

  std::string model = common::json_get_string(agent_json, "model");
  model = normalize_model(provider, std::move(model));
  if (trim_copy(model).empty()) {
    model = defaults.model;
  }

  std::string working_directory = common::json_get_string(agent_json, "working_directory");
  if (trim_copy(working_directory).empty()) {
    working_directory = (defaults.workspace_path / out.id).string();
  }

  out.provider = provider;
  out.model = model;
  out.workspace_directory = common::expand_path(working_directory);
  if (!trim_copy(name).empty()) {
    out.system_prompt = "You are " + name + ".";
  }
  return out;
}

config::TeamConfig parse_legacy_team(const std::string &team_id, const std::string &team_json,
                                     std::vector<std::string> *warnings) {
  config::TeamConfig out;
  out.id = trim_copy(team_id);
  out.description = common::json_get_string(team_json, "name");
  const std::string agents_raw = common::json_get_array(team_json, "agents");
  if (!agents_raw.empty()) {
    out.agents = parse_raw_string_array(agents_raw);
  }
  out.leader_agent = trim_copy(common::json_get_string(team_json, "leader_agent"));
  if (out.leader_agent.empty() && !out.agents.empty()) {
    out.leader_agent = out.agents.front();
    if (warnings != nullptr) {
      warnings->push_back("team '" + out.id +
                          "' had no leader_agent; defaulted to first member '" +
                          out.leader_agent + "'");
    }
  }
  return out;
}

void upsert_agent(std::vector<config::AgentConfig> *agents, config::AgentConfig next) {
  if (agents == nullptr) {
    return;
  }
  for (auto &existing : *agents) {
    if (existing.id == next.id) {
      existing = std::move(next);
      return;
    }
  }
  agents->push_back(std::move(next));
}

void upsert_team(std::vector<config::TeamConfig> *teams, config::TeamConfig next) {
  if (teams == nullptr) {
    return;
  }
  for (auto &existing : *teams) {
    if (existing.id == next.id) {
      existing = std::move(next);
      return;
    }
  }
  teams->push_back(std::move(next));
}

std::string pick_default_agent(const std::vector<config::AgentConfig> &agents,
                               const std::vector<config::TeamConfig> &teams,
                               const std::string &fallback) {
  for (const auto &team : teams) {
    if (!team.leader_agent.empty()) {
      return team.leader_agent;
    }
    if (!team.agents.empty()) {
      return team.agents.front();
    }
  }
  if (!agents.empty()) {
    return agents.front().id;
  }
  return fallback;
}

std::filesystem::path expand_input_path(const std::filesystem::path &path) {
  return std::filesystem::path(common::expand_path(path.string()));
}

} // namespace

common::Result<std::filesystem::path> detect_legacy_settings_path() {
  std::vector<std::filesystem::path> candidates;

  if (const char *env_home = std::getenv("LEGACY_CLAW_HOME");
      env_home != nullptr && *env_home != '\0') {
    candidates.emplace_back(std::filesystem::path(common::expand_path(env_home)) / "settings.json");
  }
  const std::string compatibility_env_home = compatibility_env_home_name();
  if (const char *env_home = std::getenv(compatibility_env_home.c_str());
      env_home != nullptr && *env_home != '\0') {
    candidates.emplace_back(std::filesystem::path(common::expand_path(env_home)) / "settings.json");
  }

  std::error_code ec;
  const auto cwd = std::filesystem::current_path(ec);
  if (!ec) {
    candidates.emplace_back(cwd / ".legacy" / "settings.json");
    candidates.emplace_back(cwd / compatibility_settings_dir_name() / "settings.json");
    candidates.emplace_back(cwd / "settings.json");
  }

  auto home = common::home_dir();
  if (home.ok()) {
    candidates.emplace_back(home.value() / ".legacy" / "settings.json");
    candidates.emplace_back(home.value() / compatibility_settings_dir_name() / "settings.json");
  }

  for (const auto &candidate : candidates) {
    std::error_code exists_ec;
    if (std::filesystem::exists(candidate, exists_ec) &&
        std::filesystem::is_regular_file(candidate, exists_ec)) {
      return common::Result<std::filesystem::path>::success(candidate);
    }
  }

  return common::Result<std::filesystem::path>::failure(
      "unable to locate legacy settings.json (checked ./.legacy/settings.json and "
      "~/.legacy/settings.json)");
}

common::Result<LegacyImportResult> import_legacy_settings(const LegacyImportOptions &options) {
  std::filesystem::path settings_path;
  if (options.settings_path.has_value()) {
    settings_path = expand_input_path(*options.settings_path);
  } else {
    auto detected = detect_legacy_settings_path();
    if (!detected.ok()) {
      return common::Result<LegacyImportResult>::failure(detected.error());
    }
    settings_path = detected.value();
  }

  if (!std::filesystem::exists(settings_path) || !std::filesystem::is_regular_file(settings_path)) {
    return common::Result<LegacyImportResult>::failure(
        "legacy settings file not found: " + settings_path.string());
  }

  auto raw = read_text_file(settings_path);
  if (!raw.ok()) {
    return common::Result<LegacyImportResult>::failure(raw.error());
  }
  const std::string &settings_json = raw.value();

  auto root = parse_object_members(settings_json);
  if (!root.ok()) {
    return common::Result<LegacyImportResult>::failure("invalid settings JSON: " + root.error());
  }

  const LegacyDefaults defaults = derive_defaults(settings_json, root.value());

  std::vector<config::AgentConfig> imported_agents;
  bool created_default_agent = false;
  const auto agents_it = root.value().find("agents");
  const std::string agents_json =
      (agents_it != root.value().end()) ? trim_copy(agents_it->second) : "";
  if (!agents_json.empty() && agents_json.front() == '{') {
    auto agent_members = parse_object_members(agents_it->second);
    if (!agent_members.ok()) {
      return common::Result<LegacyImportResult>::failure(
          "invalid agents object: " + agent_members.error());
    }
    for (const auto &[agent_id, agent_json] : sorted_entries(agent_members.value())) {
      if (trim_copy(agent_id).empty()) {
        continue;
      }
      if (trim_copy(agent_json).empty() || trim_copy(agent_json).front() != '{') {
        continue;
      }
      imported_agents.push_back(parse_legacy_agent(agent_id, agent_json, defaults));
    }
  }

  if (imported_agents.empty()) {
    created_default_agent = true;
    config::AgentConfig fallback;
    fallback.id = "default";
    fallback.provider = normalize_provider(defaults.provider, "anthropic");
    fallback.model = normalize_model(fallback.provider, defaults.model);
    fallback.workspace_directory = (defaults.workspace_path / "default").string();
    fallback.system_prompt = "You are Default.";
    imported_agents.push_back(std::move(fallback));
  }

  std::vector<std::string> warnings;
  std::vector<config::TeamConfig> imported_teams;
  const auto teams_it = root.value().find("teams");
  const std::string teams_json =
      (teams_it != root.value().end()) ? trim_copy(teams_it->second) : "";
  if (!teams_json.empty() && teams_json.front() == '{') {
    auto team_members = parse_object_members(teams_it->second);
    if (!team_members.ok()) {
      return common::Result<LegacyImportResult>::failure(
          "invalid teams object: " + team_members.error());
    }
    for (const auto &[team_id, team_json] : sorted_entries(team_members.value())) {
      if (trim_copy(team_id).empty()) {
        continue;
      }
      if (trim_copy(team_json).empty() || trim_copy(team_json).front() != '{') {
        continue;
      }
      imported_teams.push_back(parse_legacy_team(team_id, team_json, &warnings));
    }
  }

  auto loaded = config::load_config();
  if (!loaded.ok()) {
    return common::Result<LegacyImportResult>::failure(loaded.error());
  }
  config::Config merged = loaded.value();

  if (options.merge_with_existing) {
    for (auto agent : imported_agents) {
      upsert_agent(&merged.multi.agents, std::move(agent));
    }
    for (auto team : imported_teams) {
      upsert_team(&merged.multi.teams, std::move(team));
    }
  } else {
    merged.multi.agents = imported_agents;
    merged.multi.teams = imported_teams;
  }

  merged.multi.default_agent = pick_default_agent(merged.multi.agents, merged.multi.teams,
                                                  merged.multi.default_agent);
  if (merged.multi.max_internal_messages == 0) {
    merged.multi.max_internal_messages = 50;
  }

  if (!trim_copy(defaults.provider).empty()) {
    merged.default_provider = normalize_provider(defaults.provider, merged.default_provider);
  } else if (!merged.multi.agents.empty() && !trim_copy(merged.multi.agents.front().provider).empty()) {
    merged.default_provider =
        normalize_provider(merged.multi.agents.front().provider, merged.default_provider);
  }

  if (!trim_copy(defaults.model).empty()) {
    merged.default_model = normalize_model(merged.default_provider, defaults.model);
  } else if (!merged.multi.agents.empty() && !trim_copy(merged.multi.agents.front().model).empty()) {
    merged.default_model = merged.multi.agents.front().model;
  }

  auto validation = config::validate_config(merged);
  if (!validation.ok()) {
    return common::Result<LegacyImportResult>::failure(
        "legacy import produced invalid GhostClaw config: " + validation.error());
  }
  warnings.insert(warnings.end(), validation.value().begin(), validation.value().end());

  if (options.write_config) {
    auto saved = config::save_config(merged);
    if (!saved.ok()) {
      return common::Result<LegacyImportResult>::failure(saved.error());
    }
  }

  for (const auto &agent : merged.multi.agents) {
    if (trim_copy(agent.workspace_directory).empty()) {
      continue;
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(agent.workspace_directory), ec);
    if (ec) {
      warnings.push_back("failed to create agent workspace '" + agent.workspace_directory +
                         "': " + ec.message());
    }
  }

  LegacyImportResult result;
  result.settings_path = settings_path;
  result.imported_agents = imported_agents.size();
  result.imported_teams = imported_teams.size();
  result.created_default_agent = created_default_agent;
  result.warnings = std::move(warnings);
  result.merged_config = std::move(merged);
  return common::Result<LegacyImportResult>::success(std::move(result));
}

} // namespace ghostclaw::migration
