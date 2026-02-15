cmake_minimum_required(VERSION 3.21)
project(ghostclaw VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(GHOSTCLAW_ENABLE_SANITIZERS "Enable address/undefined sanitizers" OFF)
option(GHOSTCLAW_ENABLE_LTO "Enable link-time optimization" ON)
option(GHOSTCLAW_BUILD_BENCHMARKS "Build benchmark binaries" ON)
option(GHOSTCLAW_ENABLE_COVERAGE "Enable code coverage instrumentation" OFF)
option(BUILD_STATIC "Enable static linking where possible" OFF)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(NOT CMAKE_CONFIGURATION_TYPES)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "Dist" "RelWithDebInfo" "MinSizeRel")
endif()

if(MSVC)
  add_compile_options(/W4 /permissive- /EHsc)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2)
endif()

include(CheckIPOSupported)
if(GHOSTCLAW_ENABLE_LTO)
  check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
  if(ipo_supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
  else()
    message(WARNING "IPO/LTO not supported: ${ipo_error}")
  endif()
endif()

if(GHOSTCLAW_ENABLE_SANITIZERS AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

if(GHOSTCLAW_ENABLE_COVERAGE AND NOT MSVC)
  add_compile_options(--coverage -fprofile-arcs -ftest-coverage)
  add_link_options(--coverage)
endif()

if(BUILD_STATIC)
  set(BUILD_SHARED_LIBS OFF)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    add_link_options(-static-libgcc -static-libstdc++)
  endif()
endif()

execute_process(
  COMMAND git rev-parse --short HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE GHOSTCLAW_GIT_COMMIT
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(NOT GHOSTCLAW_GIT_COMMIT)
  set(GHOSTCLAW_GIT_COMMIT "unknown")
endif()

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)

add_library(ghostclaw_lib
  src/common/toml.cpp
  src/common/fs.cpp
  src/common/json_util.cpp
  src/config/schema.cpp
  src/config/config.cpp
  src/auth/oauth.cpp
  src/security/action_tracker.cpp
  src/security/approval.cpp
  src/security/external_content.cpp
  src/security/patterns.cpp
  src/security/policy.cpp
  src/security/secrets.cpp
  src/security/pairing.cpp
  src/security/tool_policy_pipeline.cpp
  src/providers/traits.cpp
  src/providers/compatible.cpp
  src/providers/anthropic.cpp
  src/providers/openai.cpp
  src/providers/openrouter.cpp
  src/providers/ollama.cpp
  src/providers/reliable.cpp
  src/providers/factory.cpp
  src/memory/memory.cpp
  src/memory/embedder.cpp
  src/memory/embedder_local.cpp
  src/memory/embedder_noop.cpp
  src/memory/embedder_openai.cpp
  src/memory/vector_index.cpp
  src/memory/sqlite_store.cpp
  src/memory/markdown_store.cpp
  src/memory/chunker.cpp
  src/memory/hybrid_ranker.cpp
  src/memory/workspace_indexer.cpp
  src/tools/tool.cpp
  src/tools/policy.cpp
  src/tools/tool_registry.cpp
  src/tools/approval.cpp
  src/tools/builtin/shell.cpp
  src/tools/builtin/file_read.cpp
  src/tools/builtin/file_write.cpp
  src/tools/builtin/file_edit.cpp
  src/tools/builtin/memory_store.cpp
  src/tools/builtin/memory_recall.cpp
  src/tools/builtin/memory_forget.cpp
  src/tools/builtin/web_search.cpp
  src/tools/builtin/web_fetch.cpp
  src/tools/builtin/browser.cpp
  src/tools/builtin/canvas.cpp
  src/tools/builtin/calendar.cpp
  src/tools/builtin/email.cpp
  src/tools/builtin/notify.cpp
  src/tools/builtin/reminder.cpp
  src/tools/builtin/message.cpp
  src/tools/builtin/sessions_list.cpp
  src/tools/builtin/sessions_history.cpp
  src/tools/builtin/sessions_send.cpp
  src/tools/builtin/sessions_spawn.cpp
  src/tools/builtin/subagents.cpp
  src/tools/builtin/skills.cpp
  src/tools/plugin/plugin_loader.cpp
  src/tools/plugin/plugin_watcher.cpp
  src/agent/tool_executor.cpp
  src/agent/stream_parser.cpp
  src/agent/context.cpp
  src/agent/session.cpp
  src/agent/message_queue.cpp
  src/agent/engine.cpp
  src/browser/cdp.cpp
  src/browser/actions.cpp
  src/browser/profiles.cpp
  src/browser/chrome.cpp
  src/browser/server.cpp
  src/canvas/host.cpp
  src/sandbox/docker.cpp
  src/sandbox/sandbox.cpp
  src/runtime/app.cpp
  src/gateway/protocol.cpp
  src/gateway/websocket.cpp
  src/gateway/server.cpp
  src/sessions/transcript.cpp
  src/sessions/session.cpp
  src/sessions/session_key.cpp
  src/sessions/send_policy.cpp
  src/sessions/store.cpp
  src/nodes/node.cpp
  src/nodes/discovery.cpp
  src/channels/allowlist.cpp
  src/channels/cli_channel.cpp
  src/channels/cli_plugin.cpp
  src/channels/discord/discord.cpp
  src/channels/slack/slack.cpp
  src/channels/whatsapp/whatsapp.cpp
  src/channels/signal/signal.cpp
  src/channels/imessage/imessage.cpp
  src/channels/plugin_registry.cpp
  src/channels/plugin_adapter.cpp
  src/channels/telegram/telegram.cpp
  src/channels/supervisor.cpp
  src/channels/channel_manager.cpp
  src/channels/send_service.cpp
  src/calendar/gog_backend.cpp
  src/email/gog_backend.cpp
  src/email/mailapp_backend.cpp
  src/email/smtp_backend.cpp
  src/onboard/wizard.cpp
  src/cli/commands.cpp
  src/health/health.cpp
  src/daemon/pid_file.cpp
  src/daemon/state_writer.cpp
  src/daemon/daemon.cpp
  src/heartbeat/cron.cpp
  src/heartbeat/cron_store.cpp
  src/heartbeat/scheduler.cpp
  src/heartbeat/engine.cpp
  src/skills/loader.cpp
  src/skills/registry.cpp
  src/skills/import_openclaw.cpp
  src/skills/compat.cpp
  src/skills/module.cpp
  src/tts/tts.cpp
  src/tts/elevenlabs.cpp
  src/tts/system.cpp
  src/voice/wake.cpp
  src/integrations/registry.cpp
  src/observability/log_observer.cpp
  src/observability/multi_observer.cpp
  src/observability/factory.cpp
  src/observability/global.cpp
  src/doctor/diagnostics.cpp
  src/identity/identity.cpp
  src/identity/openclaw.cpp
  src/identity/aieos.cpp
  src/identity/factory.cpp
  src/identity/templates.cpp
  src/tunnel/process.cpp
  src/tunnel/cloudflare.cpp
  src/tunnel/ngrok.cpp
  src/tunnel/tailscale.cpp
  src/tunnel/custom.cpp
  src/tunnel/factory.cpp
  src/multi/types.cpp
  src/multi/agent_pool.cpp
  src/multi/orchestrator.cpp
)

if(APPLE)
  target_sources(ghostclaw_lib PRIVATE src/calendar/eventkit_backend.mm)
  find_library(EVENTKIT_FRAMEWORK EventKit REQUIRED)
  find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
endif()

target_include_directories(ghostclaw_lib PUBLIC include)
target_link_libraries(ghostclaw_lib
  PUBLIC
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    SQLite::SQLite3
    ${CMAKE_DL_LIBS}
)
if(APPLE)
  target_link_libraries(ghostclaw_lib PUBLIC ${EVENTKIT_FRAMEWORK} ${FOUNDATION_FRAMEWORK})
endif()
target_compile_definitions(ghostclaw_lib PUBLIC
  GHOSTCLAW_VERSION="${PROJECT_VERSION}"
  GHOSTCLAW_GIT_COMMIT="${GHOSTCLAW_GIT_COMMIT}"
)

target_compile_definitions(ghostclaw_lib PRIVATE $<$<BOOL:${WIN32}>:NOMINMAX>)

add_executable(ghostclaw src/main.cpp)
target_link_libraries(ghostclaw PRIVATE ghostclaw_lib)

if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT MSVC)
  # Prefer balanced optimization in Release to avoid aggressive stripping behavior.
  target_compile_options(ghostclaw PRIVATE -O2)
  if(NOT APPLE)
    target_compile_options(ghostclaw PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(ghostclaw PRIVATE -Wl,--gc-sections)
  endif()
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Dist")
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(ghostclaw PRIVATE -Oz -ffunction-sections -fdata-sections)
    if(APPLE)
      target_link_options(ghostclaw PRIVATE -Wl,-dead_strip)
    else()
      target_link_options(ghostclaw PRIVATE -Wl,--gc-sections)
    endif()
  elseif(MSVC)
    target_compile_options(ghostclaw PRIVATE /O1 /GL)
    target_link_options(ghostclaw PRIVATE /LTCG)
  endif()
endif()

enable_testing()
add_executable(ghostclaw_tests
  tests/test_main.cpp
  tests/helpers/test_helpers.cpp
  tests/test_config.cpp
  tests/test_security.cpp
  tests/test_providers.cpp
  tests/test_memory.cpp
  tests/test_tools.cpp
  tests/test_agent.cpp
  tests/test_browser.cpp
  tests/test_gateway.cpp
  tests/test_sessions.cpp
  tests/test_channels.cpp
  tests/test_cli_onboard.cpp
  tests/test_daemon.cpp
  tests/test_heartbeat.cpp
  tests/test_skills_integrations.cpp
  tests/test_tts_voice.cpp
  tests/test_tunnel.cpp
  tests/test_observability_health_doctor.cpp
  tests/test_identity.cpp
  tests/test_sessions_tools_nodes.cpp
  tests/integration/test_config_integration.cpp
  tests/integration/test_agent_integration.cpp
  tests/integration/test_gateway_integration.cpp
  tests/integration/test_cron_integration.cpp
  tests/integration/test_skills_integration.cpp
  tests/integration/test_security_integration.cpp
  tests/integration/test_full_integration.cpp
  tests/test_multi.cpp
)
target_link_libraries(ghostclaw_tests PRIVATE ghostclaw_lib)
target_include_directories(ghostclaw_tests PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tests)
add_test(NAME ghostclaw_tests COMMAND ghostclaw_tests)

if(GHOSTCLAW_BUILD_BENCHMARKS)
  add_subdirectory(benches)
endif()

include(cmake/Doxygen.cmake)
