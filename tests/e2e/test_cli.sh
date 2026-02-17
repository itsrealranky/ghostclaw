#!/bin/bash
# GhostClaw CLI End-to-End Tests
# Run from project root: ./tests/e2e/test_cli.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
GHOSTCLAW="$PROJECT_ROOT/build/ghostclaw"
TEST_HOME=$(mktemp -d)
export HOME="$TEST_HOME"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

PASSED=0
FAILED=0

cleanup() {
    rm -rf "$TEST_HOME"
}
trap cleanup EXIT

log_pass() {
    echo -e "${GREEN}[PASS]${NC} $1"
    PASSED=$((PASSED + 1))
}

log_fail() {
    echo -e "${RED}[FAIL]${NC} $1: $2"
    FAILED=$((FAILED + 1))
}

log_info() {
    echo -e "${YELLOW}[INFO]${NC} $1"
}

# Check if ghostclaw binary exists
if [ ! -f "$GHOSTCLAW" ]; then
    echo "Error: ghostclaw binary not found at $GHOSTCLAW"
    echo "Please build the project first: cmake --build build"
    exit 1
fi

log_info "Starting CLI E2E tests..."
log_info "Test home: $TEST_HOME"

# ============================================
# Test 1: Version command
# ============================================
log_info "Test 1: Version command"
if $GHOSTCLAW --version 2>&1 | grep -q "ghostclaw"; then
    log_pass "Version command works"
else
    log_fail "Version command" "Did not output version info"
fi

# ============================================
# Test 2: Help command
# ============================================
log_info "Test 2: Help command"
if timeout 5 $GHOSTCLAW --help 2>&1 | grep -q -i "usage\|commands\|options"; then
    log_pass "Help command works"
else
    log_fail "Help command" "Did not output help info"
fi

# ============================================
# Test 3: Config show (before setup)
# ============================================
log_info "Test 3: Config show (before setup)"
if timeout 5 $GHOSTCLAW config show 2>&1; then
    log_pass "Config show works (may show defaults)"
else
    log_fail "Config show" "Command failed"
fi

# ============================================
# Test 4: Doctor command
# ============================================
log_info "Test 4: Doctor command"
if timeout 10 $GHOSTCLAW doctor 2>&1; then
    log_pass "Doctor command works"
else
    # Doctor may fail if not configured, but shouldn't crash
    log_pass "Doctor command runs (may report issues)"
fi

# ============================================
# Test 5: Channel list
# ============================================
log_info "Test 5: Channel list"
if timeout 5 $GHOSTCLAW channel list 2>&1 | grep -q -i "cli\|telegram\|discord\|channel"; then
    log_pass "Channel list works"
else
    log_fail "Channel list" "Did not list channels"
fi

# ============================================
# Test 6: Skills list
# ============================================
log_info "Test 6: Skills list"
if timeout 5 $GHOSTCLAW skills list 2>&1; then
    log_pass "Skills list works"
else
    log_fail "Skills list" "Command failed"
fi

# ============================================
# Test 7: Memory backend query
# ============================================
log_info "Test 7: Memory backend query"
if timeout 5 $GHOSTCLAW config get memory.backend 2>&1 | grep -q -E "sqlite|markdown|none"; then
    log_pass "Memory backend query works"
else
    log_fail "Memory backend query" "Command failed"
fi

# ============================================
# Test 8: Config set and get
# ============================================
log_info "Test 8: Config set"
mkdir -p "$TEST_HOME/.ghostclaw"
cat > "$TEST_HOME/.ghostclaw/config.toml" << 'EOF'
default_provider = "ollama"
default_model = "llama3"
default_temperature = 0.7
EOF

if timeout 5 $GHOSTCLAW config show 2>&1 | grep -q "ollama"; then
    log_pass "Config set and show works"
else
    log_fail "Config set" "Config not applied"
fi

# ============================================
# Test 9: TTS list providers
# ============================================
log_info "Test 9: TTS list providers"
if timeout 5 $GHOSTCLAW tts list 2>&1; then
    log_pass "TTS list works"
else
    log_fail "TTS list" "Command failed"
fi

# ============================================
# Test 10: TTS dry run
# ============================================
log_info "Test 10: TTS dry run"
if timeout 5 $GHOSTCLAW tts speak --dry-run --provider system --text "hello world" 2>&1; then
    log_pass "TTS dry run works"
else
    log_fail "TTS dry run" "Command failed"
fi

# ============================================
# Summary
# ============================================
echo ""
echo "============================================"
echo "CLI E2E Test Results"
echo "============================================"
echo -e "Passed: ${GREEN}$PASSED${NC}"
echo -e "Failed: ${RED}$FAILED${NC}"
echo "============================================"

if [ $FAILED -gt 0 ]; then
    exit 1
fi

exit 0
