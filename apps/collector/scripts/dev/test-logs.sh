#!/usr/bin/env bash

# Fry OS Collector - Development Log Test Script
# This script generates test syslog messages to verify collector functionality

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_COUNT=${1:-10}
DELAY=${2:-1}
TEST_TYPE=${3:-normal}

echo "Starting collector log test script"
echo "Generating $LOG_COUNT log messages with $DELAY second delay"
echo "Test type: $TEST_TYPE"

# Function to generate a random log message
generate_random_log() {
    local programs=("sshd" "nginx" "systemd" "kernel" "dnsmasq" "hostapd" "firewall" "network")
    local facilities=("auth" "daemon" "kern" "mail" "news" "syslog" "user" "local0")
    local priorities=("emerg" "alert" "crit" "err" "warning" "notice" "info" "debug")
    local messages=(
        "Connection established from 192.168.1.100"
        "Service started successfully"
        "Configuration loaded"
        "User authentication failed"
        "Network interface up"
        "Memory usage: 45%"
        "Process completed successfully"
        "Error: file not found"
        "Warning: disk space low"
        "Info: backup completed"
        "Debug: variable value = 42"
        "Critical: system overheating"
    )

    local program=${programs[$RANDOM % ${#programs[@]}]}
    local facility=${facilities[$RANDOM % ${#facilities[@]}]}
    local priority=${priorities[$RANDOM % ${#priorities[@]}]}
    local message=${messages[$RANDOM % ${#messages[@]}]}

    echo "$program|$facility|$priority|$message"
}

# Function to send log via logger command
send_log() {
    local program="$1"
    local facility="$2"
    local priority="$3"
    local message="$4"

    # Use logger to send to syslog
    logger -t "$program" -p "${facility}.${priority}" "$message"
    echo "[$(date '+%H:%M:%S')] Sent: [$program.$facility.$priority] $message"
}

# Function to run normal test
run_normal_test() {
    echo "Running normal log generation test..."

    for i in $(seq 1 $LOG_COUNT); do
        log_data=$(generate_random_log)
        IFS='|' read -r program facility priority message <<< "$log_data"

        send_log "$program" "$facility" "$priority" "$message"

        if [ $DELAY -gt 0 ]; then
            sleep $DELAY
        fi
    done
}

# Function to run burst test
run_burst_test() {
    echo "Running burst log generation test..."
    echo "Sending $LOG_COUNT logs as fast as possible..."

    for i in $(seq 1 $LOG_COUNT); do
        log_data=$(generate_random_log)
        IFS='|' read -r program facility priority message <<< "$log_data"

        send_log "$program" "$facility" "$priority" "BURST-$i: $message"

        # Small delay to avoid overwhelming the system
        sleep 0.01
    done
}

# Function to run stress test
run_stress_test() {
    echo "Running stress test with high log volume..."
    local stress_count=$((LOG_COUNT * 10))

    echo "Generating $stress_count logs rapidly..."

    for i in $(seq 1 $stress_count); do
        log_data=$(generate_random_log)
        IFS='|' read -r program facility priority message <<< "$log_data"

        send_log "stress-test" "$facility" "$priority" "STRESS-$i: High volume test message"

        # Very small delay
        sleep 0.001
    done
}

# Function to run filtered test (tests collector filtering)
run_filtered_test() {
    echo "Running filtered log test to verify collector filtering..."

    # Send logs that should be filtered out
    send_log "kernel" "kern" "debug" "This kernel message should be filtered"
    send_log "collector" "daemon" "info" "This collector message should be filtered"
    send_log "test" "user" "debug" "DEBUG: This debug message should be filtered"
    send_log "test" "user" "info" ""  # Empty message should be filtered
    send_log "test" "user" "info" "X"  # Very short message should be filtered

    sleep 1

    # Send logs that should pass through
    send_log "nginx" "daemon" "info" "Valid log message that should pass through"
    send_log "sshd" "auth" "warning" "Authentication attempt from unknown host"
    send_log "system" "daemon" "error" "Service failed to start properly"

    echo "Sent mix of filtered and valid messages"
}

# Function to run batch test
run_batch_test() {
    echo "Running batch test to verify batching behavior..."

    # Send exactly the batch size number of logs
    local batch_size=50

    echo "Sending $batch_size logs to trigger batch processing..."

    for i in $(seq 1 $batch_size); do
        send_log "batch-test" "user" "info" "Batch test message $i of $batch_size"
        sleep 0.1
    done

    echo "Waiting for batch to be processed..."
    sleep 5

    # Send a few more to trigger timeout-based batching
    echo "Sending additional logs for timeout-based batching..."
    for i in $(seq 1 5); do
        send_log "timeout-test" "user" "info" "Timeout batch test message $i"
        sleep 1
    done
}

# Function to display usage
show_usage() {
    echo "Usage: $0 [count] [delay] [test_type]"
    echo ""
    echo "Parameters:"
    echo "  count     Number of log messages to generate (default: 10)"
    echo "  delay     Delay between messages in seconds (default: 1)"
    echo "  test_type Test type to run (default: normal)"
    echo ""
    echo "Test types:"
    echo "  normal    Normal log generation with specified delay"
    echo "  burst     Fast burst of logs with minimal delay"
    echo "  stress    High volume stress test"
    echo "  filtered  Test filtering functionality"
    echo "  batch     Test batch processing behavior"
    echo ""
    echo "Examples:"
    echo "  $0 20 2 normal     # 20 messages, 2 second delay"
    echo "  $0 100 0 burst     # 100 messages as fast as possible"
    echo "  $0 1000 0 stress   # Stress test with 10,000 messages"
    echo "  $0 10 1 filtered   # Test filtering with mixed messages"
    echo "  $0 50 0 batch      # Test batch processing"
}

# Main execution
case "$TEST_TYPE" in
    "normal")
        run_normal_test
        ;;
    "burst")
        run_burst_test
        ;;
    "stress")
        run_stress_test
        ;;
    "filtered")
        run_filtered_test
        ;;
    "batch")
        run_batch_test
        ;;
    "help"|"-h"|"--help")
        show_usage
        exit 0
        ;;
    *)
        echo "Error: Unknown test type '$TEST_TYPE'"
        echo ""
        show_usage
        exit 1
        ;;
esac

echo ""
echo "Test completed. Check collector logs for processing results."
echo "In development mode, you should see:"
echo "- Log entries being enqueued"
echo "- Batch processing messages"
echo "- HTTP state machine transitions"
echo "- Queue statistics"
