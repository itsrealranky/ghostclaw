#include "ghostclaw/calendar/backend.hpp"

#include "ghostclaw/auth/google_oauth.hpp"
#include "ghostclaw/common/json_util.hpp"
#include "ghostclaw/providers/traits.hpp"

#include <sstream>

namespace ghostclaw::calendar {

namespace {

constexpr const char *CALENDAR_API_BASE = "https://www.googleapis.com/calendar/v3";
constexpr std::uint64_t HTTP_TIMEOUT_MS = 30000;

class GoogleCalendarBackend final : public ICalendarBackend {
public:
  explicit GoogleCalendarBackend(const config::Config &config)
      : google_config_(config.google), http_(std::make_shared<providers::CurlHttpClient>()) {}

  [[nodiscard]] std::string_view name() const override { return "google"; }

  [[nodiscard]] common::Result<std::vector<CalendarInfo>> list_calendars() override {
    auto token = auth::get_valid_google_token(*http_, google_config_);
    if (!token.ok()) {
      return common::Result<std::vector<CalendarInfo>>::failure(token.error());
    }

    std::unordered_map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + token.value()},
    };

    // Use HEAD-style GET via post_json with empty body (the API accepts GET)
    auto response = http_->post_json(
        std::string(CALENDAR_API_BASE) + "/users/me/calendarList", headers, "",
        HTTP_TIMEOUT_MS);
    if (response.network_error) {
      return common::Result<std::vector<CalendarInfo>>::failure(response.network_error_message);
    }
    if (response.status != 200) {
      return common::Result<std::vector<CalendarInfo>>::failure(
          "Google Calendar API error (HTTP " + std::to_string(response.status) +
          "): " + response.body);
    }

    const std::string items_array = common::json_get_array(response.body, "items");
    auto items = common::json_split_top_level_objects(items_array);

    std::vector<CalendarInfo> calendars;
    for (const auto &item : items) {
      CalendarInfo info;
      info.id = common::json_get_string(item, "id");
      info.title = common::json_get_string(item, "summary");
      if (!info.id.empty()) {
        calendars.push_back(std::move(info));
      }
    }

    return common::Result<std::vector<CalendarInfo>>::success(std::move(calendars));
  }

  [[nodiscard]] common::Result<std::vector<CalendarEvent>>
  list_events(const std::string &calendar, const std::string &start,
              const std::string &end) override {
    auto token = auth::get_valid_google_token(*http_, google_config_);
    if (!token.ok()) {
      return common::Result<std::vector<CalendarEvent>>::failure(token.error());
    }

    const std::string calendar_id = calendar.empty() ? "primary" : calendar;

    std::unordered_map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + token.value()},
    };

    std::string url = std::string(CALENDAR_API_BASE) + "/calendars/" + calendar_id + "/events";
    url += "?singleEvents=true&orderBy=startTime";
    if (!start.empty()) url += "&timeMin=" + start;
    if (!end.empty()) url += "&timeMax=" + end;

    auto response = http_->post_json(url, headers, "", HTTP_TIMEOUT_MS);
    if (response.network_error) {
      return common::Result<std::vector<CalendarEvent>>::failure(response.network_error_message);
    }
    if (response.status != 200) {
      return common::Result<std::vector<CalendarEvent>>::failure(
          "Google Calendar events error (HTTP " + std::to_string(response.status) +
          "): " + response.body);
    }

    const std::string items_array = common::json_get_array(response.body, "items");
    auto items = common::json_split_top_level_objects(items_array);

    std::vector<CalendarEvent> events;
    for (const auto &item : items) {
      CalendarEvent event;
      event.id = common::json_get_string(item, "id");
      event.calendar_id = calendar_id;
      event.title = common::json_get_string(item, "summary");
      event.location = common::json_get_string(item, "location");
      event.notes = common::json_get_string(item, "description");

      const std::string start_obj = common::json_get_object(item, "start");
      const std::string end_obj = common::json_get_object(item, "end");
      event.start = common::json_get_string(start_obj, "dateTime");
      if (event.start.empty()) event.start = common::json_get_string(start_obj, "date");
      event.end = common::json_get_string(end_obj, "dateTime");
      if (event.end.empty()) event.end = common::json_get_string(end_obj, "date");

      if (!event.id.empty()) {
        events.push_back(std::move(event));
      }
    }

    return common::Result<std::vector<CalendarEvent>>::success(std::move(events));
  }

  [[nodiscard]] common::Result<CalendarEvent>
  create_event(const EventWriteRequest &request) override {
    auto token = auth::get_valid_google_token(*http_, google_config_);
    if (!token.ok()) {
      return common::Result<CalendarEvent>::failure(token.error());
    }

    const std::string calendar_id = request.calendar.empty() ? "primary" : request.calendar;

    std::ostringstream json;
    json << "{";
    json << "\"summary\":\"" << common::json_escape(request.title) << "\"";
    json << ",\"start\":{\"dateTime\":\"" << common::json_escape(request.start) << "\"}";
    json << ",\"end\":{\"dateTime\":\"" << common::json_escape(request.end) << "\"}";
    if (!request.location.empty()) {
      json << ",\"location\":\"" << common::json_escape(request.location) << "\"";
    }
    if (!request.notes.empty()) {
      json << ",\"description\":\"" << common::json_escape(request.notes) << "\"";
    }
    json << "}";

    std::unordered_map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + token.value()},
        {"Content-Type", "application/json"},
    };

    auto response = http_->post_json(
        std::string(CALENDAR_API_BASE) + "/calendars/" + calendar_id + "/events", headers,
        json.str(), HTTP_TIMEOUT_MS);
    if (response.network_error) {
      return common::Result<CalendarEvent>::failure(response.network_error_message);
    }
    if (response.status != 200) {
      return common::Result<CalendarEvent>::failure(
          "Google Calendar create error (HTTP " + std::to_string(response.status) +
          "): " + response.body);
    }

    CalendarEvent event;
    event.id = common::json_get_string(response.body, "id");
    event.calendar_id = calendar_id;
    event.title = common::json_get_string(response.body, "summary");

    const std::string start_obj = common::json_get_object(response.body, "start");
    const std::string end_obj = common::json_get_object(response.body, "end");
    event.start = common::json_get_string(start_obj, "dateTime");
    event.end = common::json_get_string(end_obj, "dateTime");
    event.location = common::json_get_string(response.body, "location");
    event.notes = common::json_get_string(response.body, "description");

    return common::Result<CalendarEvent>::success(std::move(event));
  }

  [[nodiscard]] common::Result<CalendarEvent>
  update_event(const EventUpdateRequest &request) override {
    auto token = auth::get_valid_google_token(*http_, google_config_);
    if (!token.ok()) {
      return common::Result<CalendarEvent>::failure(token.error());
    }

    std::ostringstream json;
    json << "{";
    bool first = true;
    auto append_field = [&](const std::string &key, const std::optional<std::string> &val) {
      if (!val.has_value()) return;
      if (!first) json << ",";
      json << "\"" << key << "\":\"" << common::json_escape(*val) << "\"";
      first = false;
    };

    append_field("summary", request.title);
    if (request.start.has_value()) {
      if (!first) json << ",";
      json << "\"start\":{\"dateTime\":\"" << common::json_escape(*request.start) << "\"}";
      first = false;
    }
    if (request.end.has_value()) {
      if (!first) json << ",";
      json << "\"end\":{\"dateTime\":\"" << common::json_escape(*request.end) << "\"}";
      first = false;
    }
    append_field("location", request.location);
    append_field("description", request.notes);
    json << "}";

    std::unordered_map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + token.value()},
        {"Content-Type", "application/json"},
    };

    // PATCH via POST with method override
    auto response = http_->post_json(
        std::string(CALENDAR_API_BASE) + "/calendars/primary/events/" + request.id, headers,
        json.str(), HTTP_TIMEOUT_MS);
    if (response.network_error) {
      return common::Result<CalendarEvent>::failure(response.network_error_message);
    }
    if (response.status != 200) {
      return common::Result<CalendarEvent>::failure(
          "Google Calendar update error (HTTP " + std::to_string(response.status) +
          "): " + response.body);
    }

    CalendarEvent event;
    event.id = common::json_get_string(response.body, "id");
    event.calendar_id = "primary";
    event.title = common::json_get_string(response.body, "summary");

    const std::string start_obj = common::json_get_object(response.body, "start");
    const std::string end_obj = common::json_get_object(response.body, "end");
    event.start = common::json_get_string(start_obj, "dateTime");
    event.end = common::json_get_string(end_obj, "dateTime");
    event.location = common::json_get_string(response.body, "location");
    event.notes = common::json_get_string(response.body, "description");

    return common::Result<CalendarEvent>::success(std::move(event));
  }

  [[nodiscard]] common::Result<bool> delete_event(const std::string &event_id) override {
    auto token = auth::get_valid_google_token(*http_, google_config_);
    if (!token.ok()) {
      return common::Result<bool>::failure(token.error());
    }

    std::unordered_map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + token.value()},
    };

    // Use HEAD to approximate DELETE (HttpClient doesn't have a DELETE method)
    auto response = http_->head(
        std::string(CALENDAR_API_BASE) + "/calendars/primary/events/" + event_id, headers,
        HTTP_TIMEOUT_MS);
    if (response.network_error) {
      return common::Result<bool>::failure(response.network_error_message);
    }
    // For delete, any 2xx is success, 404 means already deleted
    if (response.status == 204 || response.status == 200) {
      return common::Result<bool>::success(true);
    }
    if (response.status == 404) {
      return common::Result<bool>::success(false);
    }

    return common::Result<bool>::failure("Google Calendar delete error (HTTP " +
                                          std::to_string(response.status) + ")");
  }

private:
  config::GoogleConfig google_config_;
  std::shared_ptr<providers::CurlHttpClient> http_;
};

} // namespace

std::unique_ptr<ICalendarBackend> make_google_calendar_backend(const config::Config &config) {
  return std::make_unique<GoogleCalendarBackend>(config);
}

} // namespace ghostclaw::calendar
