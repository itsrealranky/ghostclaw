#include "test_framework.hpp"

#include <csignal>
#include <iostream>

void register_config_tests(std::vector<ghostclaw::tests::TestCase> &tests);
void register_security_tests(std::vector<ghostclaw::tests::TestCase> &tests);
void register_provider_tests(std::vector<ghostclaw::tests::TestCase> &tests);
void register_memory_tests(std::vector<ghostclaw::tests::TestCase> &tests);
void register_tools_tests(std::vector<ghostclaw::tests::TestCase> &tests);
void register_agent_tests(std::vector<ghostclaw::tests::TestCase> &tests);
void register_browser_tests(std::vector<ghostclaw::tests::TestCase> &tests);
void register_gateway_tests(std::vector<ghostclaw::tests::TestCase> &tests);
void register_sessions_tests(std::vector<ghostclaw::tests::TestCase> &tests);
void register_channels_tests(std::vector<ghostclaw::tests::TestCase> &tests);
void register_cli_onboard_tests(std::vector<ghostclaw::tests::TestCase> &tests);
void register_daemon_tests(std::vector<ghostclaw::tests::TestCase> &tests);
void register_heartbeat_tests(std::vector<ghostclaw::tests::TestCase> &tests);
void register_skills_integrations_tests(std::vector<ghostclaw::tests::TestCase> &tests);
void register_tts_voice_tests(std::vector<ghostclaw::tests::TestCase> &tests);
void register_tunnel_tests(std::vector<ghostclaw::tests::TestCase> &tests);
void register_observability_health_doctor_tests(std::vector<ghostclaw::tests::TestCase> &tests);
void register_identity_tests(std::vector<ghostclaw::tests::TestCase> &tests);
void register_config_integration_tests(std::vector<ghostclaw::tests::TestCase> &tests);
void register_agent_integration_tests(std::vector<ghostclaw::tests::TestCase> &tests);
void register_gateway_integration_tests(std::vector<ghostclaw::tests::TestCase> &tests);
void register_cron_integration_tests(std::vector<ghostclaw::tests::TestCase> &tests);
void register_skills_integration_tests(std::vector<ghostclaw::tests::TestCase> &tests);
void register_security_integration_tests(std::vector<ghostclaw::tests::TestCase> &tests);
void register_sessions_tools_nodes_tests(std::vector<ghostclaw::tests::TestCase> &tests);
void register_full_integration_tests(std::vector<ghostclaw::tests::TestCase> &tests);
void register_multi_tests(std::vector<ghostclaw::tests::TestCase> &tests);

int main() {
  // Ignore SIGPIPE to prevent crashes when output is piped
  std::signal(SIGPIPE, SIG_IGN);

  std::vector<ghostclaw::tests::TestCase> tests;
  register_config_tests(tests);
  register_security_tests(tests);
  register_provider_tests(tests);
  register_memory_tests(tests);
  register_tools_tests(tests);
  register_agent_tests(tests);
  register_browser_tests(tests);
  register_gateway_tests(tests);
  register_sessions_tests(tests);
  register_channels_tests(tests);
  register_cli_onboard_tests(tests);
  register_daemon_tests(tests);
  register_heartbeat_tests(tests);
  register_skills_integrations_tests(tests);
  register_tts_voice_tests(tests);
  register_tunnel_tests(tests);
  register_observability_health_doctor_tests(tests);
  register_identity_tests(tests);
  register_config_integration_tests(tests);
  register_agent_integration_tests(tests);
  register_gateway_integration_tests(tests);
  register_cron_integration_tests(tests);
  register_skills_integration_tests(tests);
  register_security_integration_tests(tests);
  register_sessions_tools_nodes_tests(tests);
  register_full_integration_tests(tests);
  register_multi_tests(tests);

  std::size_t passed = 0;
  std::size_t failed = 0;

  for (const auto &test : tests) {
    try {
      test.fn();
      ++passed;
    } catch (const std::exception &ex) {
      ++failed;
      std::cerr << "[FAIL] " << test.name << ": " << ex.what() << "\n";
    }
  }

  std::cout << "Ran " << tests.size() << " tests: " << passed << " passed, " << failed
            << " failed\n";

  return failed == 0 ? 0 : 1;
}
