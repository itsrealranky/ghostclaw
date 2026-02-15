#include "ghostclaw/multi/types.hpp"

#include "ghostclaw/common/fs.hpp"

#include <regex>

namespace ghostclaw::multi {

std::vector<MentionMatch> extract_mentions(const std::string &text) {
  std::vector<MentionMatch> matches;
  static const std::regex mention_re(R"(\[@([a-zA-Z0-9_-]+):\s*([^\]]+)\])");

  auto it = std::sregex_iterator(text.begin(), text.end(), mention_re);
  const auto end = std::sregex_iterator();

  for (; it != end; ++it) {
    const auto &match = *it;
    MentionMatch m;
    m.target_agent_id = match[1].str();
    m.message = common::trim(match[2].str());
    m.start_pos = static_cast<std::size_t>(match.position());
    m.end_pos = m.start_pos + static_cast<std::size_t>(match.length());
    matches.push_back(std::move(m));
  }

  return matches;
}

std::optional<RouteTarget> parse_route_prefix(const std::string &input) {
  const std::string trimmed = common::trim(input);
  if (trimmed.empty() || trimmed[0] != '@') {
    return std::nullopt;
  }

  const auto space_pos = trimmed.find(' ');
  if (space_pos == std::string::npos) {
    return std::nullopt;
  }

  const std::string target = trimmed.substr(1, space_pos - 1);
  if (target.empty()) {
    return std::nullopt;
  }

  const std::string message = common::trim(trimmed.substr(space_pos + 1));
  if (message.empty()) {
    return std::nullopt;
  }

  RouteTarget route;
  route.target_id = target;
  route.message = message;
  route.is_team = false;
  return route;
}

} // namespace ghostclaw::multi
