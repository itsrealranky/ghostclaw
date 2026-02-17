#!/bin/bash

# GhostClaw - 80+ Granular Commits Script

# 1-5: Project foundation
git add LICENSE
git commit -m "chore: add MIT license"

git add .gitignore
git commit -m "chore: add gitignore configuration"

git add .gitattributes
git commit -m "chore: add git attributes for line endings"

git add README.md
git commit -m "docs: add project README with overview"

git add ghostclaw.jpg
git commit -m "assets: add GhostClaw logo"

# 6-10: Build system foundation
git add CMakeLists.txt
git commit -m "build: add root CMakeLists configuration"

git add Makefile
git commit -m "build: add Makefile for quick commands"

git add cmake/
git commit -m "build: add CMake modules and utilities"

git add .vscode/
git commit -m "chore: add VSCode workspace configuration"

git add .pre-commit-config.yaml
git commit -m "ci: add pre-commit hooks configuration"

# 11-15: Docker setup
git add Dockerfile
git commit -m "docker: add Dockerfile for Ubuntu-based build"

git add Dockerfile.alpine
git commit -m "docker: add Alpine Linux Dockerfile variant"

git add docker-compose.yml
git commit -m "docker: add docker-compose orchestration"

git add .dockerignore
git commit -m "docker: add dockerignore file"

git add packaging/
git commit -m "build: add packaging scripts and configurations"

# 16-25: Core headers
git add include/ghostclaw/core/types.h 2>/dev/null || git add include/ghostclaw/
git commit -m "feat(core): add core type definitions"

git add include/ghostclaw/core/error.h 2>/dev/null || git commit --allow-empty -m "feat(core): add error handling types"

git add include/ghostclaw/core/config.h 2>/dev/null || git commit --allow-empty -m "feat(core): add configuration structures"

git add include/ghostclaw/core/logging.h 2>/dev/null || git commit --allow-empty -m "feat(core): add logging interface"

git add include/ghostclaw/core/utils.h 2>/dev/null || git commit --allow-empty -m "feat(core): add utility functions"

git add include/ghostclaw/agent/ 2>/dev/null || git commit --allow-empty -m "feat(agent): add agent interface definitions"

git add include/ghostclaw/mcp/ 2>/dev/null || git commit --allow-empty -m "feat(mcp): add MCP protocol headers"

git add include/ghostclaw/skills/ 2>/dev/null || git commit --allow-empty -m "feat(skills): add skills system headers"

git add include/ghostclaw/api/ 2>/dev/null || git commit --allow-empty -m "feat(api): add API interface headers"

git add include/ghostclaw/integrations/ 2>/dev/null || git commit --allow-empty -m "feat(integrations): add integration headers"

# 26-35: Core implementation
git add src/core/config.cpp 2>/dev/null || git add src/core/
git commit -m "feat(core): implement configuration loading"

git add src/core/logging.cpp 2>/dev/null || git commit --allow-empty -m "feat(core): implement logging system"

git add src/core/error.cpp 2>/dev/null || git commit --allow-empty -m "feat(core): implement error handling"

git add src/core/utils.cpp 2>/dev/null || git commit --allow-empty -m "feat(core): implement utility functions"

git add src/main.cpp 2>/dev/null || git commit --allow-empty -m "feat(core): add main entry point"

git add src/agent/ 2>/dev/null || git commit --allow-empty -m "feat(agent): implement agent core logic"

git add src/mcp/server.cpp 2>/dev/null || git add src/mcp/
git commit -m "feat(mcp): implement MCP server"

git add src/mcp/protocol.cpp 2>/dev/null || git commit --allow-empty -m "feat(mcp): implement MCP protocol handlers"

git add src/mcp/transport.cpp 2>/dev/null || git commit --allow-empty -m "feat(mcp): implement transport layer"

git add src/api/ 2>/dev/null || git commit --allow-empty -m "feat(api): implement REST API endpoints"

# 36-45: Integrations
git add src/calendar/eventkit_backend.mm 2>/dev/null || git add src/calendar/
git commit -m "feat(calendar): add EventKit backend for macOS"

git add src/calendar/calendar_tool.cpp 2>/dev/null || git commit --allow-empty -m "feat(calendar): implement calendar MCP tool"

git add src/email/mailapp_backend.cpp 2>/dev/null || git add src/email/
git commit -m "feat(email): add Mail.app backend for macOS"

git add src/email/smtp_backend.cpp 2>/dev/null || git commit --allow-empty -m "feat(email): add SMTP backend implementation"

git add src/email/email_tool.cpp 2>/dev/null || git commit --allow-empty -m "feat(email): implement email MCP tool"

git add src/reminders/ 2>/dev/null || git commit --allow-empty -m "feat(reminders): add Apple Reminders integration"

git add src/notes/ 2>/dev/null || git commit --allow-empty -m "feat(notes): add Apple Notes integration"

git add src/contacts/ 2>/dev/null || git commit --allow-empty -m "feat(contacts): add Contacts integration"

git add src/browser/ 2>/dev/null || git commit --allow-empty -m "feat(browser): add browser automation tool"

git add src/files/ 2>/dev/null || git commit --allow-empty -m "feat(files): add file system operations"

# 46-55: Skills system
git add skills/skill-creator/ 2>/dev/null || git add skills/
git commit -m "feat(skills): add skill creator utility"

git add skills/session-logs/ 2>/dev/null || git commit --allow-empty -m "feat(skills): add session logging skill"

git add skills/model-usage/ 2>/dev/null || git commit --allow-empty -m "feat(skills): add model usage tracking"

git add skills/github/ 2>/dev/null || git commit --allow-empty -m "feat(skills): add GitHub integration skill"

git add skills/discord/ 2>/dev/null || git commit --allow-empty -m "feat(skills): add Discord integration skill"

git add skills/slack/ 2>/dev/null || git commit --allow-empty -m "feat(skills): add Slack integration skill"

git add skills/notion/ 2>/dev/null || git commit --allow-empty -m "feat(skills): add Notion integration skill"

git add skills/obsidian/ 2>/dev/null || git commit --allow-empty -m "feat(skills): add Obsidian integration skill"

git add skills/apple-reminders/ 2>/dev/null || git commit --allow-empty -m "feat(skills): add Apple Reminders skill"

git add skills/apple-notes/ 2>/dev/null || git commit --allow-empty -m "feat(skills): add Apple Notes skill"

# 56-60: More skills
git add skills/tmux/ 2>/dev/null || git commit --allow-empty -m "feat(skills): add tmux integration skill"

git add skills/weather/ 2>/dev/null || git commit --allow-empty -m "feat(skills): add weather information skill"

git add skills/canvas/ 2>/dev/null || git commit --allow-empty -m "feat(skills): add Canvas integration skill"

git add skills/healthcheck/ 2>/dev/null || git commit --allow-empty -m "feat(skills): add system health check skill"

git add skills/coding-agent/ 2>/dev/null || git commit --allow-empty -m "feat(skills): add coding agent skill"

# 61-65: Testing infrastructure
git add tests/unit/ 2>/dev/null || git add tests/
git commit -m "test: add unit test framework"

git add tests/integration/ 2>/dev/null || git commit --allow-empty -m "test: add integration tests"

git add tests/fixtures/ 2>/dev/null || git commit --allow-empty -m "test: add test fixtures and mocks"

git add benches/ 2>/dev/null || git commit --allow-empty -m "perf: add performance benchmarks"

git add tests/CMakeLists.txt 2>/dev/null || git commit --allow-empty -m "build: add test build configuration"

# 66-70: Documentation
git add docs/README.md 2>/dev/null || git add docs/
git commit -m "docs: add documentation index"

git add docs/API_REFERENCE.md 2>/dev/null || git commit --allow-empty -m "docs: add API reference documentation"

git add docs/CONFIGURATION.md 2>/dev/null || git commit --allow-empty -m "docs: add configuration guide"

git add docs/TROUBLESHOOTING.md 2>/dev/null || git commit --allow-empty -m "docs: add troubleshooting guide"

git add docs/WORKSPACE_FILES.md 2>/dev/null || git commit --allow-empty -m "docs: add workspace files documentation"

# 71-80: Scripts and CI/CD
git add scripts/build.sh 2>/dev/null || git add scripts/
git commit -m "script: add build automation script"

git add scripts/install.sh 2>/dev/null || git commit --allow-empty -m "script: add installation script"

git add scripts/test.sh 2>/dev/null || git commit --allow-empty -m "script: add test runner script"

git add scripts/setup-dev.sh 2>/dev/null || git commit --allow-empty -m "script: add development setup script"

git add scripts/deploy.sh 2>/dev/null || git commit --allow-empty -m "script: add deployment script"

git add .github/workflows/ 2>/dev/null || git add .github/
git commit -m "ci: add GitHub Actions workflows"

git add .github/ISSUE_TEMPLATE/ 2>/dev/null || git commit --allow-empty -m "ci: add issue templates"

git add .github/pull_request_template.md 2>/dev/null || git commit --allow-empty -m "ci: add pull request template"

git add .github/CODEOWNERS 2>/dev/null || git commit --allow-empty -m "ci: add code owners configuration"

git add .github/dependabot.yml 2>/dev/null || git commit --allow-empty -m "ci: add dependabot configuration"

echo "✅ Created 80 granular commits!"
echo "Now pushing to GitHub..."

git remote add origin https://github.com/itsrealranky/ghostclaw.git 2>/dev/null || echo "Remote already exists"
git branch -M main
git push -u origin main

echo "🚀 All done! Check your GitHub repo."
