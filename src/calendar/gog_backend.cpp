#include "ghostclaw/calendar/backend.hpp"

#include "ghostclaw/common/fs.hpp"

namespace ghostclaw::calendar {

#if defined(__APPLE__)
std::unique_ptr<ICalendarBackend> make_eventkit_calendar_backend();
#endif
std::unique_ptr<ICalendarBackend> make_google_calendar_backend(const config::Config &config);

namespace {

class GogCalendarBackend final : public ICalendarBackend {
public:
  [[nodiscard]] std::string_view name() const override { return "gog"; }

  [[nodiscard]] common::Result<std::vector<CalendarInfo>> list_calendars() override {
    return common::Result<std::vector<CalendarInfo>>::failure(
        "capability_unavailable: calendar backend 'gog' is not configured");
  }

  [[nodiscard]] common::Result<std::vector<CalendarEvent>>
  list_events(const std::string &, const std::string &, const std::string &) override {
    return common::Result<std::vector<CalendarEvent>>::failure(
        "capability_unavailable: calendar backend 'gog' is not configured");
  }

  [[nodiscard]] common::Result<CalendarEvent>
  create_event(const EventWriteRequest &) override {
    return common::Result<CalendarEvent>::failure(
        "capability_unavailable: calendar backend 'gog' cannot create events");
  }

  [[nodiscard]] common::Result<CalendarEvent>
  update_event(const EventUpdateRequest &) override {
    return common::Result<CalendarEvent>::failure(
        "capability_unavailable: calendar backend 'gog' cannot update events");
  }

  [[nodiscard]] common::Result<bool> delete_event(const std::string &) override {
    return common::Result<bool>::failure(
        "capability_unavailable: calendar backend 'gog' cannot delete events");
  }
};

} // namespace

std::unique_ptr<ICalendarBackend> make_calendar_backend(const config::Config &config) {
  const std::string backend = common::to_lower(common::trim(config.calendar.backend));
  if (backend == "gog") {
    return std::make_unique<GogCalendarBackend>();
  }
  if (backend == "google") {
    return make_google_calendar_backend(config);
  }

#if defined(__APPLE__)
  if (backend.empty() || backend == "auto" || backend == "eventkit") {
    return make_eventkit_calendar_backend();
  }
#else
  // config is used above for "google" backend
#endif
  return std::make_unique<GogCalendarBackend>();
}

} // namespace ghostclaw::calendar
