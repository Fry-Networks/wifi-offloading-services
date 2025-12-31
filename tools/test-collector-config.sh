#!/usr/bin/env bash

set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_DIR="$PROJECT_ROOT/test-collector-config"
PASSED_TESTS=0
TOTAL_TESTS=0

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[PASS]${NC} $1"
    ((PASSED_TESTS++))
}

log_error() {
    echo -e "${RED}[FAIL]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

test_header() {
    ((TOTAL_TESTS++))
    echo ""
    echo -e "${YELLOW}=== Test $TOTAL_TESTS: $1 ===${NC}"
}

cleanup() {
    if [ -d "$TEST_DIR" ]; then
        log_info "Cleaning up test directory: $TEST_DIR"
        rm -rf "$TEST_DIR"
    fi

    if [ -d "$PROJECT_ROOT/run/collector" ]; then
        log_info "Cleaning up run directory: $PROJECT_ROOT/run/collector"
        rm -rf "$PROJECT_ROOT/run/collector"
    fi
}

# Trap to ensure cleanup on exit
trap cleanup EXIT

main() {
    echo -e "${BLUE}Fry Collector Configuration Test Suite${NC}"
    echo "========================================"
    echo "Project root: $PROJECT_ROOT"
    echo ""

    cd "$PROJECT_ROOT"

    # Test 1: Build system
    test_header "Build System Test"
    if just cmake; then
        log_success "CMake build completed successfully"
    else
        log_error "CMake build failed"
        exit 1
    fi

    # Test 2: Executable creation
    test_header "Executable Creation Test"
    if [ -f "build/fry-collector" ]; then
        log_success "fry-collector executable created"
    else
        log_error "fry-collector executable not found in build directory"
        exit 1
    fi

    # Test 3: Configuration file existence
    test_header "Configuration File Test"
    if [ -f "apps/collector/scripts/dev/fry-collector.config" ]; then
        log_success "Development configuration file exists"
    else
        log_error "Development configuration file not found"
        exit 1
    fi

    # Test 4: Configuration file format validation
    test_header "Configuration Format Validation"
    config_file="apps/collector/scripts/dev/fry-collector.config"
    if grep -q "config fry_collector 'fry_collector'" "$config_file" && \
       grep -q "option enabled" "$config_file" && \
       grep -q "option logs_endpoint" "$config_file"; then
        log_success "Configuration file has correct UCI format"
    else
        log_error "Configuration file format is invalid"
        exit 1
    fi

    # Test 5: Just run collector setup
    test_header "Just Run Collector Setup Test"

    # Create a background process to run collector for a short time
    timeout 10s just run collector > test_collector_output.log 2>&1 &
    COLLECTOR_PID=$!

    # Wait a moment for setup
    sleep 3

    # Kill the collector process
    if kill $COLLECTOR_PID 2>/dev/null; then
        log_info "Collector process terminated"
    fi

    # Wait for process to actually exit
    wait $COLLECTOR_PID 2>/dev/null || true

    # Check if setup was successful
    if [ -d "run/collector" ]; then
        log_success "Run directory created successfully"
    else
        log_error "Run directory not created"
        exit 1
    fi

    # Test 6: File placement verification
    test_header "File Placement Verification"

    files_to_check=(
        "run/collector/collector"
        "run/collector/fry-collector.config"
        "run/collector/scripts/test-logs.sh"
        "run/collector/scripts/mock-backend.py"
    )

    all_files_present=true
    for file in "${files_to_check[@]}"; do
        if [ -f "$file" ]; then
            log_info "✓ $file exists"
        else
            log_error "✗ $file missing"
            all_files_present=false
        fi
    done

    if $all_files_present; then
        log_success "All required files placed correctly"
    else
        log_error "Some files are missing from run directory"
        exit 1
    fi

    # Test 7: Configuration content verification
    test_header "Configuration Content Verification"
    run_config="run/collector/fry-collector.config"

    if [ -f "$run_config" ]; then
        # Check for development-specific settings
        if grep -q "http://localhost:8080" "$run_config" && \
           grep -q "batch_size '5'" "$run_config" && \
           grep -q "dev_mode '1'" "$run_config"; then
            log_success "Configuration contains correct development settings"
        else
            log_warn "Configuration may not have optimal development settings"
            log_info "Checking configuration content:"
            grep -E "(logs_endpoint|batch_size|dev_mode)" "$run_config" || true
        fi
    else
        log_error "Configuration file not found in run directory"
        exit 1
    fi

    # Test 8: Executable permissions
    test_header "Executable Permissions Test"

    if [ -x "run/collector/collector" ]; then
        log_success "Collector executable has correct permissions"
    else
        log_error "Collector executable is not executable"
        exit 1
    fi

    if [ -x "run/collector/scripts/test-logs.sh" ]; then
        log_success "Test scripts have correct permissions"
    else
        log_error "Test scripts are not executable"
        exit 1
    fi

    # Test 9: Configuration validation with collector
    test_header "Configuration Validation Test"

    cd run/collector

    # Test configuration loading without running the full collector
    if echo "" | timeout 5s ./collector --help > /dev/null 2>&1; then
        log_success "Collector can load and display help"
    else
        log_warn "Collector help command had issues (may be normal)"
    fi

    cd "$PROJECT_ROOT"

    # Test 10: Clean collector output test
    test_header "Collector Output Test"

    if [ -f "test_collector_output.log" ]; then
        # Check for expected output patterns
        if grep -q "Configuration loaded" test_collector_output.log || \
           grep -q "Single-core collection system initialized" test_collector_output.log || \
           grep -q "development mode" test_collector_output.log; then
            log_success "Collector produces expected output"
        else
            log_warn "Collector output may be different than expected"
            log_info "Collector output sample:"
            head -10 test_collector_output.log || true
        fi

        # Check for error patterns
        if grep -qi "error\|failed\|critical" test_collector_output.log; then
            log_warn "Collector output contains error messages"
            log_info "Error messages found:"
            grep -i "error\|failed\|critical" test_collector_output.log || true
        fi

        rm -f test_collector_output.log
    else
        log_warn "No collector output log found"
    fi

    # Final summary
    echo ""
    echo -e "${BLUE}=== Test Summary ===${NC}"
    echo "Total tests: $TOTAL_TESTS"
    echo "Passed tests: $PASSED_TESTS"
    echo "Failed tests: $((TOTAL_TESTS - PASSED_TESTS))"

    if [ $PASSED_TESTS -eq $TOTAL_TESTS ]; then
        echo -e "${GREEN}✓ All tests passed! Collector configuration is working correctly.${NC}"
        echo ""
        echo "Next steps:"
        echo "1. Run: just run collector"
        echo "2. In another terminal: cd run/collector/scripts && python3 mock-backend.py --verbose"
        echo "3. In another terminal: cd run/collector/scripts && ./test-logs.sh 10 1 normal"
        exit 0
    else
        echo -e "${RED}✗ Some tests failed. Please check the output above.${NC}"
        exit 1
    fi
}

# Show usage if help requested
if [ "$1" = "--help" ] || [ "$1" = "-h" ]; then
    echo "Fry Collector Configuration Test Suite"
    echo ""
    echo "Usage: $0"
    echo ""
    echo "This script tests the collector configuration system:"
    echo "  - Builds the collector"
    echo "  - Tests configuration file placement"
    echo "  - Validates configuration loading"
    echo "  - Verifies 'just run collector' workflow"
    echo ""
    echo "Run this script from the project root directory."
    exit 0
fi

main "$@"
