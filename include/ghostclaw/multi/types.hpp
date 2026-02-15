#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ghostclaw::multi {

struct InternalMessage {
  std::uint64_t id = 0;
  std::string sender_agent_id;
  std::string target_agent_id;
  std::string content;
  std::string conversation_id;
  std::uint64_t timestamp = 0;
  bool is_mention = false;
};

struct Conversation {
  std::string id;
  std::string originator;
  std::string origin_channel;
  std::string origin_sender;
  std::size_t pending_count = 0;
  std::size_t total_messages = 0;
  bool complete = false;
};

struct MentionMatch {
  std::string target_agent_id;
  std::string message;
  std::size_t start_pos = 0;
  std::size_t end_pos = 0;
};

struct RouteTarget {
  std::string target_id;
  std::string message;
  bool is_team = false;
};

[[nodiscard]] std::vector<MentionMatch> extract_mentions(const std::string &text);
[[nodiscard]] std::optional<RouteTarget> parse_route_prefix(const std::string &input);

} // namespace ghostclaw::multi
