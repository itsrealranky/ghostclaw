#include "ghostclaw/service/module.hpp"

#include "ghostclaw/common/fs.hpp"
#include "ghostclaw/config/config.hpp"
#include "ghostclaw/daemon/pid_file.hpp"

#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace ghostclaw::service {

namespace {

enum class ServiceBackend {
  Managed,
  Launchd,
  Systemd,
};

struct ServicePaths {
  std::filesystem::path root;
  std::filesystem::path pid_file;
  std::filesystem::path install_marker;
  std::filesystem::path stdout_log;
  std::filesystem::path stderr_log;
  std::filesystem::path launchd_plist;
  std::filesystem::path systemd_unit;
  std::string launchd_label;
  std::string systemd_unit_name;
};

common::Result<ServicePaths> resolve_paths() {
  const auto config_dir = config::config_dir();
  if (!config_dir.ok()) {
    return common::Result<ServicePaths>::failure(config_dir.error());
  }

  ServicePaths out;
  out.root = config_dir.value() / "service";
  out.pid_file = out.root / "daemon.pid";
  out.install_marker = out.root / "installed.txt";
  out.stdout_log = out.root / "daemon.stdout.log";
  out.stderr_log = out.root / "daemon.stderr.log";
  out.launchd_label = "dev.ghostclaw.agent";
  out.systemd_unit_name = "ghostclaw.service";

  std::error_code ec;
  std::filesystem::create_directories(out.root, ec);
  if (ec) {
    return common::Result<ServicePaths>::failure("failed to create service directory: " +
                                                 ec.message());
  }

  const auto home = common::home_dir();
  if (home.ok()) {
    out.launchd_plist = home.value() / "Library" / "LaunchAgents" / (out.launchd_label + ".plist");
    out.systemd_unit =
        home.value() / ".config" / "systemd" / "user" / out.systemd_unit_name;
  }

  return common::Result<ServicePaths>::success(std::move(out));
}

std::string backend_to_string(const ServiceBackend backend) {
  switch (backend) {
  case ServiceBackend::Managed:
    return "managed";
  case ServiceBackend::Launchd:
    return "launchd";
  case ServiceBackend::Systemd:
    return "systemd";
  }
  return "managed";
}

ServiceBackend backend_from_string(const std::string &raw) {
  const std::string value = common::to_lower(common::trim(raw));
  if (value == "launchd") {
    return ServiceBackend::Launchd;
  }
  if (value == "systemd") {
    return ServiceBackend::Systemd;
  }
  return ServiceBackend::Managed;
}

std::optional<std::string> read_marker_value(const std::filesystem::path &path,
                                             const std::string &key) {
  if (!std::filesystem::exists(path)) {
    return std::nullopt;
  }

  std::ifstream in(path);
  if (!in) {
    return std::nullopt;
  }

  std::string line;
  while (std::getline(in, line)) {
    const auto eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    const std::string marker_key = common::trim(line.substr(0, eq));
    if (marker_key != key) {
      continue;
    }
    return common::trim(line.substr(eq + 1));
  }

  return std::nullopt;
}

ServiceBackend backend_from_marker(const ServicePaths &paths) {
  const auto value = read_marker_value(paths.install_marker, "backend");
  if (!value.has_value()) {
    return ServiceBackend::Managed;
  }
  return backend_from_string(*value);
}

common::Status write_install_marker(const ServicePaths &paths, const std::string &executable_path,
                                    const ServiceBackend backend) {
  std::ofstream out(paths.install_marker, std::ios::trunc);
  if (!out) {
    return common::Status::error("failed to write install marker");
  }
  out << "executable=" << executable_path << "\n";
  out << "backend=" << backend_to_string(backend) << "\n";
  out << "installed_at="
      << std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
             .count()
      << "\n";
  if (!out) {
    return common::Status::error("failed to flush install marker");
  }
  return common::Status::success();
}

int read_pid(const std::filesystem::path &pid_path) {
  if (!std::filesystem::exists(pid_path)) {
    return 0;
  }

  std::ifstream in(pid_path);
  if (!in) {
    return 0;
  }

  int pid = 0;
  in >> pid;
  if (!in) {
    return 0;
  }
  return pid;
}

common::Status write_pid(const std::filesystem::path &pid_path, const int pid) {
  std::ofstream out(pid_path, std::ios::trunc);
  if (!out) {
    return common::Status::error("failed to write service pid file");
  }
  out << pid << "\n";
  if (!out) {
    return common::Status::error("failed to flush service pid file");
  }
  return common::Status::success();
}

void remove_file_if_exists(const std::filesystem::path &path) {
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    std::filesystem::remove(path, ec);
  }
}

#ifndef _WIN32
std::string resolve_executable_path(const std::string &hint) {
  std::string candidate = common::trim(hint);
  if (!candidate.empty()) {
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec)) {
      auto canonical = std::filesystem::weakly_canonical(candidate, ec);
      if (!ec) {
        return canonical.string();
      }
      return candidate;
    }
    if (candidate.find('/') != std::string::npos) {
      return candidate;
    }
  }

#if defined(__APPLE__)
  std::uint32_t size = 0;
  (void)_NSGetExecutablePath(nullptr, &size);
  std::string path(size, '\0');
  if (_NSGetExecutablePath(path.data(), &size) == 0) {
    path.resize(std::strlen(path.c_str()));
    std::error_code ec;
    const auto self = std::filesystem::path(path);
    if (common::starts_with(self.filename().string(), "ghostclaw_tests")) {
      const auto sibling = self.parent_path() / "ghostclaw";
      if (std::filesystem::exists(sibling, ec)) {
        return sibling.string();
      }
    }
    return path;
  }
#elif defined(__linux__)
  char buf[PATH_MAX] = {0};
  const ssize_t read = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (read > 0) {
    buf[read] = '\0';
    std::error_code ec;
    const auto self = std::filesystem::path(buf);
    if (common::starts_with(self.filename().string(), "ghostclaw_tests")) {
      const auto sibling = self.parent_path() / "ghostclaw";
      if (std::filesystem::exists(sibling, ec)) {
        return sibling.string();
      }
    }
    return std::string(buf);
  }
#endif

  return "ghostclaw";
}

int run_command(const std::string &command) {
  return std::system(command.c_str());
}

bool command_exists(const std::string &command) {
  const std::string probe = "command -v " + command + " >/dev/null 2>&1";
  return run_command(probe) == 0;
}

std::string shell_quote(const std::string &value) {
  std::string out = "'";
  for (const char ch : value) {
    if (ch == '\'') {
      out += "'\\''";
    } else {
      out.push_back(ch);
    }
  }
  out.push_back('\'');
  return out;
}

std::string launchd_domain() {
  return "gui/" + std::to_string(static_cast<long long>(getuid()));
}

common::Status write_launchd_plist(const ServicePaths &paths, const std::string &executable_path) {
#if defined(__APPLE__)
  if (paths.launchd_plist.empty()) {
    return common::Status::error("unable to resolve launchd plist path");
  }

  std::error_code ec;
  std::filesystem::create_directories(paths.launchd_plist.parent_path(), ec);
  if (ec) {
    return common::Status::error("failed to create launch agent directory: " + ec.message());
  }

  std::ofstream out(paths.launchd_plist, std::ios::trunc);
  if (!out) {
    return common::Status::error("failed to write launchd plist");
  }

  out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  out << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n";
  out << "<plist version=\"1.0\">\n";
  out << "<dict>\n";
  out << "  <key>Label</key><string>" << paths.launchd_label << "</string>\n";
  out << "  <key>ProgramArguments</key>\n";
  out << "  <array>\n";
  out << "    <string>" << executable_path << "</string>\n";
  out << "    <string>daemon</string>\n";
  out << "    <string>--duration-secs</string>\n";
  out << "    <string>315360000</string>\n";
  out << "  </array>\n";
  out << "  <key>RunAtLoad</key><true/>\n";
  out << "  <key>KeepAlive</key><true/>\n";
  out << "  <key>StandardOutPath</key><string>" << paths.stdout_log.string() << "</string>\n";
  out << "  <key>StandardErrorPath</key><string>" << paths.stderr_log.string() << "</string>\n";
  out << "  <key>WorkingDirectory</key><string>" << paths.root.string() << "</string>\n";
  out << "</dict>\n";
  out << "</plist>\n";

  if (!out) {
    return common::Status::error("failed to flush launchd plist");
  }
  return common::Status::success();
#else
  (void)paths;
  (void)executable_path;
  return common::Status::error("launchd is unavailable on this platform");
#endif
}

common::Status install_launchd_service(const ServicePaths &paths, const std::string &executable_path) {
#if defined(__APPLE__)
  if (!command_exists("launchctl")) {
    return common::Status::error("launchctl is not available");
  }

  auto written = write_launchd_plist(paths, executable_path);
  if (!written.ok()) {
    return written;
  }
  // Install only. Start is explicit via `service start`.
  return common::Status::success();
#else
  (void)paths;
  (void)executable_path;
  return common::Status::error("launchd is unavailable on this platform");
#endif
}

common::Status start_launchd_service(const ServicePaths &paths) {
#if defined(__APPLE__)
  const std::string domain = launchd_domain();
  const std::string print_cmd = "launchctl print " + domain + "/" + paths.launchd_label;
  if (run_command(print_cmd + " >/dev/null 2>&1") != 0) {
    const std::string bootstrap =
        "launchctl bootstrap " + domain + " " + shell_quote(paths.launchd_plist.string());
    if (run_command(bootstrap + " >/dev/null 2>&1") != 0) {
      return common::Status::error("failed to bootstrap launchd service");
    }
  }
  const std::string command = "launchctl kickstart -k " + domain + "/" + paths.launchd_label;
  if (run_command(command + " >/dev/null 2>&1") != 0) {
    return common::Status::error("failed to start launchd service");
  }
  return common::Status::success();
#else
  (void)paths;
  return common::Status::error("launchd is unavailable on this platform");
#endif
}

common::Status stop_launchd_service(const ServicePaths &paths) {
#if defined(__APPLE__)
  const std::string domain = launchd_domain();
  const std::string command = "launchctl bootout " + domain + "/" + paths.launchd_label;
  if (run_command(command + " >/dev/null 2>&1") != 0) {
    return common::Status::error("failed to stop launchd service");
  }
  return common::Status::success();
#else
  (void)paths;
  return common::Status::error("launchd is unavailable on this platform");
#endif
}

bool launchd_service_running(const ServicePaths &paths) {
#if defined(__APPLE__)
  const std::string domain = launchd_domain();
  const std::string command = "launchctl print " + domain + "/" + paths.launchd_label + " >/dev/null 2>&1";
  return run_command(command) == 0;
#else
  (void)paths;
  return false;
#endif
}

common::Status uninstall_launchd_service(const ServicePaths &paths) {
#if defined(__APPLE__)
  const std::string domain = launchd_domain();
  (void)run_command("launchctl bootout " + domain + "/" + paths.launchd_label + " >/dev/null 2>&1");
  remove_file_if_exists(paths.launchd_plist);
  return common::Status::success();
#else
  (void)paths;
  return common::Status::error("launchd is unavailable on this platform");
#endif
}

[[maybe_unused]] common::Status write_systemd_unit(const ServicePaths &paths,
                                                   const std::string &executable_path) {
#if defined(__linux__)
  if (paths.systemd_unit.empty()) {
    return common::Status::error("unable to resolve systemd unit path");
  }

  std::error_code ec;
  std::filesystem::create_directories(paths.systemd_unit.parent_path(), ec);
  if (ec) {
    return common::Status::error("failed to create systemd directory: " + ec.message());
  }

  std::ofstream out(paths.systemd_unit, std::ios::trunc);
  if (!out) {
    return common::Status::error("failed to write systemd unit");
  }

  out << "[Unit]\n";
  out << "Description=GhostClaw Autonomous Daemon\n";
  out << "After=network-online.target\n";
  out << "\n";
  out << "[Service]\n";
  out << "Type=simple\n";
  out << "ExecStart=" << executable_path << " daemon --duration-secs 315360000\n";
  out << "Restart=on-failure\n";
  out << "RestartSec=2\n";
  out << "WorkingDirectory=" << paths.root.string() << "\n";
  out << "StandardOutput=append:" << paths.stdout_log.string() << "\n";
  out << "StandardError=append:" << paths.stderr_log.string() << "\n";
  out << "\n";
  out << "[Install]\n";
  out << "WantedBy=default.target\n";

  if (!out) {
    return common::Status::error("failed to flush systemd unit");
  }

  if (run_command("systemctl --user daemon-reload >/dev/null 2>&1") != 0) {
    return common::Status::error("failed to reload systemd user daemon");
  }
  return common::Status::success();
#else
  (void)paths;
  (void)executable_path;
  return common::Status::error("systemd is unavailable on this platform");
#endif
}

[[maybe_unused]] common::Status install_systemd_service(const ServicePaths &paths,
                                                        const std::string &executable_path) {
#if defined(__linux__)
  if (!command_exists("systemctl")) {
    return common::Status::error("systemctl is not available");
  }
  auto written = write_systemd_unit(paths, executable_path);
  if (!written.ok()) {
    return written;
  }
  if (run_command("systemctl --user enable " + paths.systemd_unit_name + " >/dev/null 2>&1") != 0) {
    return common::Status::error("failed to enable systemd user unit");
  }
  return common::Status::success();
#else
  (void)paths;
  (void)executable_path;
  return common::Status::error("systemd is unavailable on this platform");
#endif
}

common::Status start_systemd_service(const ServicePaths &paths) {
#if defined(__linux__)
  if (run_command("systemctl --user start " + paths.systemd_unit_name + " >/dev/null 2>&1") != 0) {
    return common::Status::error("failed to start systemd user unit");
  }
  return common::Status::success();
#else
  (void)paths;
  return common::Status::error("systemd is unavailable on this platform");
#endif
}

common::Status stop_systemd_service(const ServicePaths &paths) {
#if defined(__linux__)
  if (run_command("systemctl --user stop " + paths.systemd_unit_name + " >/dev/null 2>&1") != 0) {
    return common::Status::error("failed to stop systemd user unit");
  }
  return common::Status::success();
#else
  (void)paths;
  return common::Status::error("systemd is unavailable on this platform");
#endif
}

bool systemd_service_running(const ServicePaths &paths) {
#if defined(__linux__)
  return run_command("systemctl --user is-active --quiet " + paths.systemd_unit_name) == 0;
#else
  (void)paths;
  return false;
#endif
}

common::Status uninstall_systemd_service(const ServicePaths &paths) {
#if defined(__linux__)
  (void)run_command("systemctl --user disable --now " + paths.systemd_unit_name + " >/dev/null 2>&1");
  remove_file_if_exists(paths.systemd_unit);
  (void)run_command("systemctl --user daemon-reload >/dev/null 2>&1");
  return common::Status::success();
#else
  (void)paths;
  return common::Status::error("systemd is unavailable on this platform");
#endif
}

common::Status spawn_daemon_process(const ServicePaths &paths, const std::string &executable_path,
                                    int &pid_out) {
  const int existing_pid = read_pid(paths.pid_file);
  if (existing_pid > 0 && daemon::PidFile::is_process_running(existing_pid)) {
    return common::Status::error("service already running with pid " +
                                 std::to_string(existing_pid));
  }

  pid_t pid = fork();
  if (pid < 0) {
    return common::Status::error("failed to fork service process");
  }

  if (pid == 0) {
    (void)setsid();

    int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
      (void)dup2(devnull, STDIN_FILENO);
      close(devnull);
    }

    int out_fd = open(paths.stdout_log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (out_fd >= 0) {
      (void)dup2(out_fd, STDOUT_FILENO);
      close(out_fd);
    }

    int err_fd = open(paths.stderr_log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (err_fd >= 0) {
      (void)dup2(err_fd, STDERR_FILENO);
      close(err_fd);
    }

    const std::string exe = resolve_executable_path(executable_path);
    execl(exe.c_str(), exe.c_str(), "daemon", "--duration-secs", "315360000", static_cast<char *>(nullptr));
    _exit(127);
  }

  pid_out = static_cast<int>(pid);
  return common::Status::success();
}

common::Status stop_running_process(const ServicePaths &paths) {
  const int pid = read_pid(paths.pid_file);
  if (pid <= 0) {
    remove_file_if_exists(paths.pid_file);
    return common::Status::success();
  }

  if (!daemon::PidFile::is_process_running(pid)) {
    remove_file_if_exists(paths.pid_file);
    return common::Status::success();
  }

  if (kill(pid, SIGTERM) != 0) {
    remove_file_if_exists(paths.pid_file);
    return common::Status::error("failed to signal daemon process");
  }

  for (int i = 0; i < 50; ++i) {
    if (!daemon::PidFile::is_process_running(pid)) {
      remove_file_if_exists(paths.pid_file);
      return common::Status::success();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  (void)kill(pid, SIGKILL);
  for (int i = 0; i < 20; ++i) {
    if (!daemon::PidFile::is_process_running(pid)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  remove_file_if_exists(paths.pid_file);
  return common::Status::success();
}

common::Status install_native_service(const ServicePaths &paths, const std::string &executable_path,
                                      ServiceBackend &backend_out) {
#if defined(__APPLE__)
  auto installed = install_launchd_service(paths, executable_path);
  if (installed.ok()) {
    backend_out = ServiceBackend::Launchd;
    return common::Status::success();
  }
  backend_out = ServiceBackend::Managed;
  return installed;
#elif defined(__linux__)
  auto installed = install_systemd_service(paths, executable_path);
  if (installed.ok()) {
    backend_out = ServiceBackend::Systemd;
    return common::Status::success();
  }
  backend_out = ServiceBackend::Managed;
  return installed;
#else
  (void)paths;
  (void)executable_path;
  backend_out = ServiceBackend::Managed;
  return common::Status::error("native service management is unavailable on this platform");
#endif
}

common::Status start_managed_service(const ServicePaths &paths, const std::string &executable_path) {
  int pid = 0;
  auto spawned = spawn_daemon_process(paths, executable_path, pid);
  if (!spawned.ok()) {
    return spawned;
  }
  auto written = write_pid(paths.pid_file, pid);
  if (!written.ok()) {
    return written;
  }
  return common::Status::success();
}

common::Status stop_managed_service(const ServicePaths &paths) { return stop_running_process(paths); }

bool managed_service_running(const ServicePaths &paths) {
  const int pid = read_pid(paths.pid_file);
  return pid > 0 && daemon::PidFile::is_process_running(pid);
}

#endif

common::Status install_service(const ServicePaths &paths, const std::string &executable_path,
                               std::string &backend_message) {
#ifdef _WIN32
  backend_message = "managed";
  return write_install_marker(paths, executable_path, ServiceBackend::Managed);
#else
  ServiceBackend backend = ServiceBackend::Managed;
  auto native = install_native_service(paths, executable_path, backend);
  if (!native.ok()) {
    backend = ServiceBackend::Managed;
    backend_message = "managed";
  } else {
    backend_message = backend_to_string(backend);
  }

  auto marker = write_install_marker(paths, executable_path, backend);
  if (!marker.ok()) {
    return marker;
  }
  return common::Status::success();
#endif
}

common::Status start_service(const ServicePaths &paths, const std::string &executable_path,
                             std::string &backend_message) {
#ifdef _WIN32
  (void)paths;
  (void)executable_path;
  (void)backend_message;
  return common::Status::error("service start is not implemented on Windows");
#else
  const ServiceBackend backend = backend_from_marker(paths);

  if (backend == ServiceBackend::Launchd) {
    auto started = start_launchd_service(paths);
    if (started.ok()) {
      backend_message = "launchd";
      return common::Status::success();
    }
  }

  if (backend == ServiceBackend::Systemd) {
    auto started = start_systemd_service(paths);
    if (started.ok()) {
      backend_message = "systemd";
      return common::Status::success();
    }
  }

  auto started = start_managed_service(paths, executable_path);
  if (!started.ok()) {
    return started;
  }
  backend_message = "managed";
  return common::Status::success();
#endif
}

common::Status stop_service(const ServicePaths &paths, std::string &backend_message) {
#ifdef _WIN32
  (void)paths;
  (void)backend_message;
  return common::Status::error("service stop is not implemented on Windows");
#else
  const ServiceBackend backend = backend_from_marker(paths);

  if (backend == ServiceBackend::Launchd) {
    auto stopped = stop_launchd_service(paths);
    if (stopped.ok()) {
      backend_message = "launchd";
      return common::Status::success();
    }
  }

  if (backend == ServiceBackend::Systemd) {
    auto stopped = stop_systemd_service(paths);
    if (stopped.ok()) {
      backend_message = "systemd";
      return common::Status::success();
    }
  }

  auto stopped = stop_managed_service(paths);
  if (!stopped.ok()) {
    return stopped;
  }
  backend_message = "managed";
  return common::Status::success();
#endif
}

common::Status status_service(const ServicePaths &paths, std::string &backend_message) {
#ifdef _WIN32
  (void)paths;
  backend_message = "managed";
  return common::Status::success();
#else
  const ServiceBackend backend = backend_from_marker(paths);

  if (backend == ServiceBackend::Launchd) {
    backend_message = "launchd";
    if (launchd_service_running(paths)) {
      return common::Status::error("running");
    }
    return common::Status::success();
  }

  if (backend == ServiceBackend::Systemd) {
    backend_message = "systemd";
    if (systemd_service_running(paths)) {
      return common::Status::error("running");
    }
    return common::Status::success();
  }

  backend_message = "managed";
  if (managed_service_running(paths)) {
    return common::Status::error("running");
  }
  return common::Status::success();
#endif
}

common::Status uninstall_service(const ServicePaths &paths, std::string &backend_message) {
#ifdef _WIN32
  backend_message = "managed";
  remove_file_if_exists(paths.install_marker);
  remove_file_if_exists(paths.pid_file);
  return common::Status::success();
#else
  const ServiceBackend backend = backend_from_marker(paths);
  backend_message = backend_to_string(backend);

  if (backend == ServiceBackend::Launchd) {
    (void)uninstall_launchd_service(paths);
  } else if (backend == ServiceBackend::Systemd) {
    (void)uninstall_systemd_service(paths);
  }

  (void)stop_managed_service(paths);
  remove_file_if_exists(paths.install_marker);
  remove_file_if_exists(paths.pid_file);
  return common::Status::success();
#endif
}

} // namespace

common::Result<std::string> handle_command(const std::vector<std::string> &args,
                                           const std::string &executable_path) {
  if (args.empty()) {
    return common::Result<std::string>::failure(
        "usage: ghostclaw service <install|start|stop|status|uninstall>");
  }

  const auto paths = resolve_paths();
  if (!paths.ok()) {
    return common::Result<std::string>::failure(paths.error());
  }

  const std::string subcommand = common::to_lower(common::trim(args[0]));
  if (subcommand == "install") {
    std::string backend;
    auto installed = install_service(paths.value(), executable_path, backend);
    if (!installed.ok()) {
      return common::Result<std::string>::failure(installed.error());
    }
    return common::Result<std::string>::success("installed (backend=" + backend + ")");
  }

  if (subcommand == "start") {
    std::string backend;
    auto started = start_service(paths.value(), executable_path, backend);
    if (!started.ok()) {
      return common::Result<std::string>::failure(started.error());
    }
    return common::Result<std::string>::success("started (backend=" + backend + ")");
  }

  if (subcommand == "stop") {
    std::string backend;
    auto stopped = stop_service(paths.value(), backend);
    if (!stopped.ok()) {
      return common::Result<std::string>::failure(stopped.error());
    }
    return common::Result<std::string>::success("stopped (backend=" + backend + ")");
  }

  if (subcommand == "status") {
    std::string backend;
    const auto status = status_service(paths.value(), backend);
    if (!status.ok()) {
      return common::Result<std::string>::success("running (backend=" + backend + ")");
    }
    return common::Result<std::string>::success("stopped (backend=" + backend + ")");
  }

  if (subcommand == "uninstall") {
    std::string backend;
    auto removed = uninstall_service(paths.value(), backend);
    if (!removed.ok()) {
      return common::Result<std::string>::failure(removed.error());
    }
    return common::Result<std::string>::success("uninstalled (backend=" + backend + ")");
  }

  return common::Result<std::string>::failure(
      "usage: ghostclaw service <install|start|stop|status|uninstall>");
}

} // namespace ghostclaw::service
