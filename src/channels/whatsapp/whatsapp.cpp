#include "ghostclaw/channels/whatsapp/whatsapp.hpp"

#include "ghostclaw/channels/allowlist.hpp"
#include "ghostclaw/common/fs.hpp"
#include "ghostclaw/common/json_util.hpp"

#include <algorithm>
#include <sstream>

namespace ghostclaw::channels::whatsapp {

namespace {

std::string json_escape(const std::string &value) { return common::json_escape(value); }

std::string normalize_number(std::string value) {
  value = common::trim(std::move(value));
  value.erase(std::remove_if(value.begin(), value.end(),
                             [](const char ch) { return ch == ' ' || ch == '-' || ch == '(' || ch == ')'; }),
              value.end());
  return value;
}

} // namespace

WhatsAppChannelPlugin::WhatsAppChannelPlugin(std::shared_ptr<providers::HttpClient> http_client)
    : http_client_(std::move(http_client)) {}

WhatsAppChannelPlugin::~WhatsAppChannelPlugin() { stop(); }

std::string_view WhatsAppChannelPlugin::id() const { return "whatsapp"; }

ChannelCapabilities WhatsAppChannelPlugin::capabilities() const {
  ChannelCapabilities caps;
  caps.reply = true;
  caps.media = true;
  caps.reactions = true;
  caps.polls = true;
  return caps;
}

common::Status WhatsAppChannelPlugin::start(const ChannelConfig &config) {
  if (running_.load()) {
    return common::Status::success();
  }
  if (http_client_ == nullptr) {
    return common::Status::error("whatsapp http client unavailable");
  }

  const auto token_it = config.settings.find("access_token");
  const auto phone_it = config.settings.find("phone_number_id");
  if (token_it == config.settings.end() || common::trim(token_it->second).empty() ||
      phone_it == config.settings.end() || common::trim(phone_it->second).empty()) {
    return common::Status::error("whatsapp access_token and phone_number_id are required");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  access_token_ = common::trim(token_it->second);
  phone_number_id_ = common::trim(phone_it->second);
  const auto version_it = config.settings.find("api_version");
  if (version_it != config.settings.end() && !common::trim(version_it->second).empty()) {
    api_version_ = common::trim(version_it->second);
  } else {
    api_version_ = "v21.0";
  }

  allowed_numbers_.clear();
  const auto allow_it = config.settings.find("allowed_numbers");
  if (allow_it != config.settings.end()) {
    allowed_numbers_ = parse_allowlist(allow_it->second);
  }
  healthy_.store(true);
  running_.store(true);
  return common::Status::success();
}

void WhatsAppChannelPlugin::stop() { running_.store(false); }

common::Status WhatsAppChannelPlugin::send_text(const std::string &recipient,
                                                const std::string &text) {
  if (!running_.load()) {
    return common::Status::error("whatsapp plugin is not running");
  }
  const std::string to = normalize_number(recipient);
  if (to.empty()) {
    return common::Status::error("whatsapp recipient is required");
  }
  if (!is_allowed_number(to)) {
    return common::Status::error("whatsapp recipient blocked by allowlist");
  }
  if (common::trim(text).empty()) {
    return common::Status::error("whatsapp text is required");
  }

  std::string token;
  std::string phone_id;
  std::string version;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    token = access_token_;
    phone_id = phone_number_id_;
    version = api_version_;
  }

  std::ostringstream body;
  body << "{";
  body << "\"messaging_product\":\"whatsapp\",";
  body << "\"to\":\"" << json_escape(to) << "\",";
  body << "\"type\":\"text\",";
  body << "\"text\":{\"body\":\"" << json_escape(text) << "\"}";
  body << "}";

  const auto response = http_client_->post_json(
      "https://graph.facebook.com/" + version + "/" + phone_id + "/messages",
      {{"Authorization", "Bearer " + token}, {"Content-Type", "application/json"}},
      body.str(),
      18000);
  return check_response(response, "whatsapp send text");
}

common::Status WhatsAppChannelPlugin::send_media(const std::string &recipient,
                                                 const MediaMessage &media) {
  if (!running_.load()) {
    return common::Status::error("whatsapp plugin is not running");
  }
  const std::string to = normalize_number(recipient);
  if (to.empty()) {
    return common::Status::error("whatsapp recipient is required");
  }
  if (!is_allowed_number(to)) {
    return common::Status::error("whatsapp recipient blocked by allowlist");
  }
  if (common::trim(media.url).empty()) {
    return common::Status::error("whatsapp media url is required");
  }

  const bool is_image = common::starts_with(common::to_lower(media.mime_type), "image/");
  const std::string type = is_image ? "image" : "document";

  std::string token;
  std::string phone_id;
  std::string version;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    token = access_token_;
    phone_id = phone_number_id_;
    version = api_version_;
  }

  std::ostringstream body;
  body << "{";
  body << "\"messaging_product\":\"whatsapp\",";
  body << "\"to\":\"" << json_escape(to) << "\",";
  body << "\"type\":\"" << type << "\",";
  body << "\"" << type << "\":{";
  body << "\"link\":\"" << json_escape(media.url) << "\"";
  if (!common::trim(media.caption).empty()) {
    body << ",\"caption\":\"" << json_escape(media.caption) << "\"";
  }
  body << "}";
  body << "}";

  const auto response = http_client_->post_json(
      "https://graph.facebook.com/" + version + "/" + phone_id + "/messages",
      {{"Authorization", "Bearer " + token}, {"Content-Type", "application/json"}},
      body.str(),
      22000);
  return check_response(response, "whatsapp send media");
}

void WhatsAppChannelPlugin::on_message(PluginMessageCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  message_callback_ = std::move(callback);
}

void WhatsAppChannelPlugin::on_reaction(PluginReactionCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  reaction_callback_ = std::move(callback);
}

bool WhatsAppChannelPlugin::health_check() { return !running_.load() || healthy_.load(); }

common::Status WhatsAppChannelPlugin::check_response(const providers::HttpResponse &response,
                                                     const std::string_view operation) const {
  if (response.timeout) {
    return common::Status::error(std::string(operation) + " timeout");
  }
  if (response.network_error) {
    return common::Status::error(std::string(operation) + " network error: " +
                                 response.network_error_message);
  }
  if (response.status < 200 || response.status >= 300) {
    std::string body = common::trim(response.body);
    if (body.size() > 200) {
      body.resize(200);
    }
    return common::Status::error(std::string(operation) + " failed status=" +
                                 std::to_string(response.status) + " body=" + body);
  }
  if (response.body.find("\"error\"") != std::string::npos &&
      response.body.find("\"messages\"") == std::string::npos) {
    return common::Status::error(std::string(operation) + " api returned error");
  }
  return common::Status::success();
}

bool WhatsAppChannelPlugin::is_allowed_number(const std::string &number) const {
  std::vector<std::string> allowlist;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    allowlist = allowed_numbers_;
  }
  if (allowlist.empty()) {
    return true;
  }
  return check_allowlist(normalize_number(number), allowlist);
}

std::vector<std::string> WhatsAppChannelPlugin::parse_allowlist(const std::string &raw) {
  std::vector<std::string> out;
  std::stringstream stream(raw);
  std::string token;
  while (std::getline(stream, token, ',')) {
    token = normalize_number(token);
    if (!token.empty()) {
      out.push_back(token);
    }
  }
  return out;
}

} // namespace ghostclaw::channels::whatsapp
