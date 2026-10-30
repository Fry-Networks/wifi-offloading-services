#!/usr/bin/env bash
#
# Regression test: config JSON fixtures must never carry a hardcoded WPA2 PSK,
# and the renderers must never emit an OPEN network or a placeholder passphrase.
#
# The PSK is not committed. It is injected at runtime from the WIFI_PSK
# environment variable by apps/config/scripts/dev/renderer_applier.uc, which
# resolves a whole-string "${WIFI_PSK}" placeholder in the wifi-iface "key"
# field only, before emitting `uci set wireless.<iface>.key=...`.
#
# This suite asserts, without ever handling the secret value itself:
#   1. every config JSON that has a "key" field parses and uses the placeholder
#   2. the checker actually detects a hardcoded key (decoy control)
#   3. the resolver is structurally restricted to the PSK field, and the prod
#      renderer refuses unusable PSKs
#   4. dev renderer FAILS CLOSED on unset / empty / whitespace-only / short
#   5. dev renderer renders a valid PSK, and does NOT substitute non-PSK values
#   6. prod renderer refuses an unresolved placeholder (uci module stubbed via -L
#      when the host has no uci module)
#
# Exit status: 0 = all runnable tests passed, 1 = a test failed.
#
# Test 4-6 need ucode. Set UCODE_BIN=/path/to/ucode to use a specific binary,
# otherwise ucode is looked up in PATH, /usr/local/bin and /usr/bin. Without
# ucode those tests are SKIPPED (INCONCLUSIVE), never silently passed.

set -u

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PLACEHOLDER='${WIFI_PSK}'
RENDERER="$PROJECT_ROOT/apps/config/scripts/dev/renderer_applier.uc"
PROD_RENDERER="$PROJECT_ROOT/apps/config/scripts/openwrt/renderer_applier.uc"
FIXTURE="$PROJECT_ROOT/apps/config/scripts/dev/fry_config.json"

PY=/usr/bin/python3
[ -x "$PY" ] || PY=python3
GREP=/usr/bin/grep
[ -x "$GREP" ] || GREP=grep
FIND=/usr/bin/find
[ -x "$FIND" ] || FIND=find

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
PASSED=0; FAILED=0; SKIPPED=0

info() { echo -e "${BLUE}[INFO]${NC} $1"; }
pass() { echo -e "${GREEN}[PASS]${NC} $1"; PASSED=$((PASSED + 1)); }
fail() { echo -e "${RED}[FAIL]${NC} $1"; FAILED=$((FAILED + 1)); }
skip() { echo -e "${YELLOW}[SKIP]${NC} $1"; SKIPPED=$((SKIPPED + 1)); }

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

# check_no_hardcoded_key <json> [placeholder]
# Exit 0 = every "key" field equals the placeholder, 1 = a non-placeholder
# value is present, 2 = the file has no "key" field (skipped by callers).
# Never prints field values, only counts.
check_no_hardcoded_key() {
    "$PY" -I - "$1" "${2:-$PLACEHOLDER}" <<'PY'
import json, sys
path, ph = sys.argv[1], sys.argv[2]
with open(path, encoding="utf-8") as fh:
    data = json.load(fh)
vals = []
def walk(o):
    if isinstance(o, dict):
        for k, v in o.items():
            if k == "key" and isinstance(v, str):
                vals.append(v)
            else:
                walk(v)
    elif isinstance(o, list):
        for x in o:
            walk(x)
walk(data)
if not vals:
    print("no key fields")
    sys.exit(2)
bad = [v for v in vals if v != ph]
if bad:
    print("HARDCODED KEY in %d of %d field(s)" % (len(bad), len(vals)))
    sys.exit(1)
print("all %d key field(s) are placeholders" % len(vals))
sys.exit(0)
PY
}

# run_dev <config> <psk-mode> ; psk-mode is "unset" or the literal env value.
# Sets OUT, ERR, RC. The PSK test values are sentinels generated below, never
# the real key.
run_dev() {
    : > "$WORK_DIR/err"
    if [ "$2" = "unset" ]; then
        OUT="$(env -u WIFI_PSK "$UCODE_BIN" "$RENDERER" "$1" 2>"$WORK_DIR/err")"
    else
        OUT="$(WIFI_PSK="$2" "$UCODE_BIN" "$RENDERER" "$1" 2>"$WORK_DIR/err")"
    fi
    RC=$?
    ERR="$(cat "$WORK_DIR/err")"
}

# assert_dev_rejects <label> <psk-mode> <expected-message-substring>
assert_dev_rejects() {
    local label="$1" mode="$2" needle="$3"
    run_dev "$FIXTURE" "$mode"
    if [ "$RC" -eq 0 ]; then
        fail "$label: renderer exited 0, expected a refusal"
        return
    fi
    if ! printf '%s' "$ERR" | "$GREP" -q "$needle"; then
        fail "$label: stderr does not name the problem ($needle)"
        return
    fi
    if printf '%s' "$OUT" | "$GREP" -q 'key='; then
        fail "$label: a key= command was emitted despite the refusal"
        return
    fi
    if printf '%s' "$OUT" | "$GREP" -q '^uci '; then
        fail "$label: UCI commands were emitted despite the refusal"
        return
    fi
    pass "$label: refused (exit $RC), named the problem, emitted no UCI"
}

echo -e "${BLUE}Fry Config Fixture PSK Test Suite${NC}"
echo "======================================"
echo "Project root: $PROJECT_ROOT"
echo "Placeholder:  $PLACEHOLDER"
echo ""

# ---------------------------------------------------------------------------
# Test 1: every config JSON in the repo that has a "key" field must be clean.
# ---------------------------------------------------------------------------
echo -e "${YELLOW}=== Test 1: no hardcoded key in any config JSON ===${NC}"
checked=0
while IFS= read -r json_file; do
    out="$(check_no_hardcoded_key "$json_file" 2>&1)"; rc=$?
    case $rc in
        0) checked=$((checked + 1)) ;;
        1) fail "$json_file: $out" ;;
        2) : ;; # no key fields, nothing to assert
        *) fail "$json_file: could not parse JSON ($out)" ;;
    esac
done < <("$FIND" "$PROJECT_ROOT" \
            \( -name node_modules -o -name .git \) -prune -o \
            -name '*.json' -type f -print 2>/dev/null)
if [ "$checked" -gt 0 ]; then
    pass "$checked config JSON file(s) carry only the placeholder, no hardcoded key"
else
    fail "no config JSON with a key field was found at all"
fi

# ---------------------------------------------------------------------------
# Test 2: decoy control - prove the checker detects a hardcoded key.
# ---------------------------------------------------------------------------
echo -e "${YELLOW}=== Test 2: decoy control (checker must flag a hardcoded key) ===${NC}"
printf '{"device_config":{"wireless":[{"meta_type":"wifi-iface","key":"DECOY_NOT_A_REAL_KEY_9f3a"}]}}\n' \
    > "$WORK_DIR/decoy.json"
if check_no_hardcoded_key "$WORK_DIR/decoy.json" >/dev/null 2>&1; then
    fail "decoy with a hardcoded key was NOT detected - the checker is broken"
else
    pass "decoy with a hardcoded key was correctly detected (checker works)"
fi

# ---------------------------------------------------------------------------
# Test 3: static wiring - structural PSK restriction, no blanket resolver.
# ---------------------------------------------------------------------------
echo -e "${YELLOW}=== Test 3: resolver wiring is restricted to the PSK field ===${NC}"
if [ ! -f "$RENDERER" ]; then
    fail "renderer not found: $RENDERER"
else
    if "$GREP" -q "function isPskField" "$RENDERER" \
       && "$GREP" -q "packageName == 'wireless' && sectionType == 'wifi-iface' && key == 'key'" "$RENDERER" \
       && "$GREP" -q "isPskField('wireless', section.meta_type, key)" "$RENDERER"; then
        pass "dev renderer resolves the environment only for wifi-iface.key"
    else
        fail "dev renderer lacks the structural PSK field restriction"
    fi
    getenv_calls="$("$GREP" -c 'getenv(' "$RENDERER")"
    if [ "$getenv_calls" -eq 1 ] && ! "$GREP" -q 'resolveEnvPlaceholder' "$RENDERER"; then
        pass "getenv() is called once, from the PSK resolver only (no blanket substitution)"
    else
        fail "getenv() is not confined to a single PSK resolver (calls: $getenv_calls)"
    fi
    if "$GREP" -q "configured value %s" "$RENDERER" && "$GREP" -q "refusing to render" "$RENDERER"; then
        pass "dev renderer has a fail-closed error path that refuses to render"
    else
        fail "dev renderer has no fail-closed error path"
    fi
fi
if [ ! -f "$PROD_RENDERER" ]; then
    fail "prod renderer not found: $PROD_RENDERER"
else
    prod_guard_line="$("$GREP" -n 'let pskIssue = validatePsks(config);' "$PROD_RENDERER" | cut -d: -f1)"
    prod_cursor_line="$("$GREP" -n 'ctx = cursor();' "$PROD_RENDERER" | cut -d: -f1)"
    if "$GREP" -q "function validatePsks" "$PROD_RENDERER" \
       && [ -n "$prod_guard_line" ] && [ -n "$prod_cursor_line" ] \
       && [ "$prod_guard_line" -lt "$prod_cursor_line" ]; then
        pass "prod renderer validates the PSK before creating the UCI cursor"
    else
        fail "prod renderer does not validate the PSK before touching UCI"
    fi
fi

# ---------------------------------------------------------------------------
# Tests 4-6 need a ucode runtime.
# ---------------------------------------------------------------------------
UCODE_BIN="${UCODE_BIN:-}"
if [ -z "$UCODE_BIN" ]; then
    for cand in "$(command -v ucode 2>/dev/null || true)" /usr/local/bin/ucode /usr/bin/ucode; do
        if [ -n "$cand" ] && [ -x "$cand" ]; then UCODE_BIN="$cand"; break; fi
    done
fi
if [ -z "$UCODE_BIN" ]; then
    skip "ucode not installed here - fail-closed and resolution tests NOT executed (INCONCLUSIVE)"
else
    info "ucode: $UCODE_BIN"
    # The dev renderer imports the fs module; a runtime built without it cannot
    # run the file at all, so say so instead of reporting false failures.
    if ! "$UCODE_BIN" -e 'import * as fs from "fs";' >/dev/null 2>&1; then
        skip "this ucode cannot load the fs module - renderer tests NOT executed (INCONCLUSIVE)"
        UCODE_BIN=""
    fi
fi

# ---------------------------------------------------------------------------
# Test 4: dev renderer fails CLOSED on every unusable PSK state.
# ---------------------------------------------------------------------------
echo -e "${YELLOW}=== Test 4: dev renderer refuses unusable PSKs ===${NC}"
if [ -z "$UCODE_BIN" ]; then
    skip "Test 4 (needs ucode)"
elif [ ! -f "$FIXTURE" ]; then
    fail "fixture not found: $FIXTURE"
else
    assert_dev_rejects "unset WIFI_PSK" "unset" 'WIFI_PSK'
    assert_dev_rejects "empty WIFI_PSK" "" 'WIFI_PSK'
    assert_dev_rejects "whitespace-only WIFI_PSK" "        " 'whitespace-only'
    assert_dev_rejects "short WIFI_PSK (7 chars)" "abc1234" 'shorter than the WPA2 minimum'
    # An empty LITERAL key (not a placeholder) must not reach `uci set ...key=''`.
    printf '{"device_config":{"wireless":[{"meta_type":"wifi-iface","meta_section":"dev_wifi","key":""}]}}\n' \
        > "$WORK_DIR/literal-empty.json"
    run_dev "$WORK_DIR/literal-empty.json" "SENTINEL_NOT_USED_HERE"
    if [ "$RC" -ne 0 ] && ! printf '%s' "$OUT" | "$GREP" -q "key=''"; then
        pass "empty literal key: refused (exit $RC), no open network emitted"
    else
        fail "empty literal key: exit $RC and/or an empty key was emitted"
    fi
fi

# ---------------------------------------------------------------------------
# Test 5: a valid PSK is rendered, and no unresolved placeholder leaks out.
# ---------------------------------------------------------------------------
echo -e "${YELLOW}=== Test 5: valid WIFI_PSK is rendered ===${NC}"
if [ -z "$UCODE_BIN" ]; then
    skip "Test 5 (needs ucode)"
else
    SENTINEL="SENTINEL_$(date +%s)_$$_VAL"
    run_dev "$FIXTURE" "$SENTINEL"
    if [ "$RC" -ne 0 ]; then
        fail "valid WIFI_PSK: renderer exited $RC ($ERR)"
    elif printf '%s' "$OUT" | "$GREP" -q "key='$SENTINEL'"; then
        pass "WIFI_PSK from the environment was substituted into the UCI output"
    else
        fail "WIFI_PSK was not substituted into the UCI output"
    fi
    if printf '%s' "$OUT" | "$GREP" -q 'key=.\${WIFI_PSK}'; then
        fail "unresolved placeholder leaked into the UCI output"
    else
        pass "no unresolved placeholder leaked into the UCI output"
    fi
    if printf '%s' "$OUT" | "$GREP" -q "key=''"; then
        fail "an empty key was emitted"
    else
        pass "no empty key was emitted"
    fi
fi

# ---------------------------------------------------------------------------
# Test 6: non-PSK values are NOT substituted (no environment disclosure).
# ---------------------------------------------------------------------------
echo -e "${YELLOW}=== Test 6: non-PSK placeholders are left verbatim ===${NC}"
if [ -z "$UCODE_BIN" ]; then
    skip "Test 6 (needs ucode)"
else
    DECOY_VAR="PSK_TEST_DECOY_$$"
    DECOY_VAL="decoy_value_should_never_appear_$$"
    cat > "$WORK_DIR/nonpsk.json" <<JSON
{"device_config":{
  "wireless":[{"meta_type":"wifi-iface","meta_section":"dev_wifi","key":"\${WIFI_PSK}"}],
  "fry":[{"meta_config":"fry-agent","meta_type":"fry_agent","meta_section":"fry_agent","some_option":"\${$DECOY_VAR}"}]
}}
JSON
    SENTINEL="SENTINEL_$(date +%s)_$$_VAL"
    : > "$WORK_DIR/err"
    OUT="$(env WIFI_PSK="$SENTINEL" "$DECOY_VAR=$DECOY_VAL" "$UCODE_BIN" "$RENDERER" "$WORK_DIR/nonpsk.json" 2>"$WORK_DIR/err")"
    RC=$?
    if [ "$RC" -ne 0 ]; then
        fail "non-PSK placeholder config: renderer exited $RC"
    elif printf '%s' "$OUT" | "$GREP" -q "some_option='\${$DECOY_VAR}'"; then
        pass "non-PSK placeholder rendered verbatim (not resolved)"
    else
        fail "non-PSK placeholder was rewritten instead of rendered verbatim"
    fi
    if printf '%s' "$OUT" | "$GREP" -q "$DECOY_VAL"; then
        fail "a non-PSK environment value was disclosed into the UCI output"
    else
        pass "no non-PSK environment value reached the UCI output"
    fi
    if printf '%s' "$OUT" | "$GREP" -q "key='$SENTINEL'"; then
        pass "the PSK field in the same config was still resolved"
    else
        fail "the PSK field was not resolved in the mixed config"
    fi
fi

# ---------------------------------------------------------------------------
# Test 7: prod renderer refuses an unresolved placeholder as the PSK.
# The 'uci' module may not exist on a test host, so a stub module is supplied
# with -L as a fallback. The refusal happens in main() before the cursor is
# used, so the stub never has to provide a working UCI cursor.
# ---------------------------------------------------------------------------
echo -e "${YELLOW}=== Test 7: prod renderer refuses an unresolved placeholder ===${NC}"
if [ -z "$UCODE_BIN" ]; then
    skip "Test 7 (needs ucode)"
elif [ ! -f "$PROD_RENDERER" ]; then
    fail "prod renderer not found: $PROD_RENDERER"
else
    mkdir -p "$WORK_DIR/ucode-stub"
    printf 'export function cursor() { return null; }\n' > "$WORK_DIR/ucode-stub/uci.uc"
    printf '{"device_config":{"wireless":[{"meta_type":"wifi-iface","meta_section":"dev_wifi","key":"${WIFI_PSK}"}]}}\n' \
        > "$WORK_DIR/prod.json"
    run_prod() { # extra -L dir or empty
        : > "$WORK_DIR/err"
        if [ -n "$1" ]; then
            OUT="$("$UCODE_BIN" -L "$1" "$PROD_RENDERER" "$WORK_DIR/prod.json" 2>"$WORK_DIR/err")"
        else
            OUT="$("$UCODE_BIN" "$PROD_RENDERER" "$WORK_DIR/prod.json" 2>"$WORK_DIR/err")"
        fi
        RC=$?
        ERR="$(cat "$WORK_DIR/err")"
    }
    run_prod ""
    MODE="real uci module"
    if ! printf '%s' "$ERR" | "$GREP" -q 'unresolved environment placeholder' \
       && printf '%s' "$ERR" | "$GREP" -q "module 'uci'"; then
        # No uci module on this host: supply a stub via -L. The refusal happens
        # in main() before the cursor is ever used, so the stub is sufficient.
        run_prod "$WORK_DIR/ucode-stub"
        MODE="stub uci module on -L"
    fi
    if [ "$RC" -eq 0 ]; then
        fail "prod renderer accepted an unresolved placeholder as the PSK ($MODE)"
    elif printf '%s' "$ERR" | "$GREP" -q 'unresolved environment placeholder'; then
        pass "prod renderer refused the placeholder loudly (exit $RC, $MODE)"
    else
        fail "prod renderer failed for an unexpected reason ($MODE): $ERR"
    fi
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo -e "${BLUE}=== Test Summary ===${NC}"
echo "Passed:  $PASSED"
echo "Failed:  $FAILED"
echo "Skipped: $SKIPPED"
if [ "$FAILED" -eq 0 ]; then
    echo -e "${GREEN}All runnable tests passed.${NC}"
    if [ "$SKIPPED" -gt 0 ]; then
        echo -e "${YELLOW}$SKIPPED test(s) skipped - see messages above.${NC}"
    fi
    exit 0
fi
echo -e "${RED}Some tests failed.${NC}"
exit 1
