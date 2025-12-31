# Collector Development Quick Start Guide

This guide helps you quickly set up and test the Fry OS Collector in a local development environment.

## Quick Start

### 1. Configure the Collector

The collector uses configuration files to control its behavior. The `just run collector` command automatically copies the development configuration:

```bash
# The configuration is automatically set up when you run:
just run collector

# To manually edit the configuration:
# 1. First run the collector once to set up the environment
# 2. Edit the configuration file in the run directory
vim run/collector/fry-collector.config

# 3. Restart the collector to apply changes
```

### 2. Build and Run Collector

```bash
# From project root (this handles everything automatically)
just run collector

# The command will:
# - Build the collector with cmake
# - Set up the run/collector directory
# - Copy the executable and configuration files
# - Copy development scripts
# - Start the collector in development mode
```

### 3. Start Mock Backend (Optional)

In a separate terminal:

```bash
cd run/collector/scripts
python3 mock-backend.py --verbose
```

The mock backend will start on `http://localhost:8080` and accept log submissions. The development configuration is already set to use this endpoint.

### 4. Generate Test Logs

In another terminal:

```bash
cd run/collector/scripts

# Generate 10 normal test logs
./test-logs.sh 10 1 normal

# Quick burst test
./test-logs.sh 50 0 burst

# Test filtering functionality
./test-logs.sh 10 1 filtered

# Test batch processing
./test-logs.sh 50 0 batch
```

Note: The development configuration uses small batch sizes (5 logs) so you'll see batches processed quickly.

## Development Files

- **`fry-collector.config`** - UCI-style configuration file
- **`test-logs.sh`** - Script to generate test syslog messages
- **`mock-backend.py`** - Local HTTP server for testing
- **`README.md`** - This guide

## Configuration

The collector reads configuration from UCI-style config files in this order:

1. `/etc/config/fry-collector` (OpenWrt production)
2. `./fry-collector.config` (local development)
3. `/tmp/fry-collector.config` (fallback)

### Configuration Options

```bash
config fry_collector 'fry_collector'
    option enabled '1'                    # Enable/disable collector
    option logs_endpoint 'http://...'     # Backend URL
    option batch_size '10'                # Logs per batch
    option batch_timeout_ms '5000'        # Batch timeout
    option queue_size '100'               # Internal queue size
    option http_timeout '15'              # HTTP timeout (seconds)
    option http_retries '2'               # HTTP retry attempts
    option reconnect_delay_ms '3000'      # UBUS reconnect delay
    option dev_mode '1'                   # Development mode
    option verbose_logging '1'            # Verbose output
```

### Development vs Production Settings

**Development** (small values for faster testing):
- `batch_size`: 5-10 logs
- `batch_timeout_ms`: 3000-5000ms
- `queue_size`: 50-100 entries
- `logs_endpoint`: `http://localhost:8080/v1/logs`

**Production** (optimized for efficiency):
- `batch_size`: 50 logs
- `batch_timeout_ms`: 10000ms
- `queue_size`: 500 entries
- `logs_endpoint`: `https://devices.fry.tech/logs`

## What You'll See

### Collector Output (--dev mode)
```
[collector] Collector service started in development mode (single-core optimized)
[config] Configuration loaded from: ./fry-collector.config
[config] Current Configuration:
[config]   enabled: true
[config]   logs_endpoint: http://localhost:8080/v1/logs
[config]   batch_size: 10
[config]   batch_timeout_ms: 5000
[config]   queue_size: 100
[collector] Detected 4 CPU core(s) - using single-threaded event loop
[ubus] UBUS initialization complete (single-core mode)
[collect] Single-core collection system initialized (max_queue_size=100, max_batch_size=10)
[collector] Status: queue_size=8, dropped=0, ubus_connected=yes
[collect] Starting batch: reached max size (10)
[collect] Successfully sent batch of 10 logs
```

### Mock Backend Output
```
[14:30:25] Received batch: 50 logs from collector v1.0.0-single-core
  [1] sshd.auth.info: Connection established from 192.168.1.100
  [2] nginx.daemon.warning: Warning: disk space low
  ...
```

## Testing Scenarios

### 1. Normal Operation
```bash
./test-logs.sh 20 2 normal
```
- Generates 20 logs with 2-second intervals
- Tests normal batching behavior
- Verifies queue management

### 2. Burst Testing
```bash
./test-logs.sh 100 0 burst
```
- Rapid log generation
- Tests queue handling under load
- Verifies urgent batch processing

### 3. Filter Testing
```bash
./test-logs.sh 10 1 filtered
```
- Mixed valid and filtered messages
- Tests collector filtering logic
- Shows which logs are processed/dropped

### 4. Batch Testing
```bash
./test-logs.sh 50 0 batch
```
- Exactly batch-size number of logs
- Tests batch size triggering
- Tests timeout-based batching

### 5. Stress Testing
```bash
./test-logs.sh 100 0 stress
```
- High volume log generation
- Tests system under stress
- Monitors queue overflow protection



## Mock Backend Options

```bash
# Basic server
python3 mock-backend.py

# Verbose logging
python3 mock-backend.py --verbose

# Simulate failures (10% failure rate)
python3 mock-backend.py --simulate-failures --failure-rate 0.1

# Simulate network delays
python3 mock-backend.py --simulate-delay

# Custom host/port
python3 mock-backend.py --host 0.0.0.0 --port 9090
```

## Monitoring

### Real-time Status
The collector logs status every 30 seconds in dev mode:
```
Status: queue_size=15, dropped=0, ubus_connected=yes
```

### Backend Statistics
Check mock backend stats:
```bash
curl http://localhost:8080/stats
```

### Health Check
```bash
curl http://localhost:8080/health
```

## Troubleshooting

### Collector Not Receiving Logs
1. Check if collector is enabled: `option enabled '1'` in config
2. Check if `logd` service is running
3. Verify UBUS connection: `ubus list | grep log`
4. Check collector logs for UBUS connection errors
5. Verify configuration is loaded correctly (check config output in --dev mode)

### No Logs Reaching Backend
1. Check `logs_endpoint` in configuration matches backend URL
2. Verify mock backend is running on correct port
3. Check collector HTTP state machine logs
4. Test backend manually with curl
5. Verify `http_timeout` and `http_retries` settings

### High Memory Usage
1. Check queue size in status messages
2. Verify logs are being processed and sent
3. Check for backend connectivity issues

### Queue Overflows
1. Increase `queue_size` in configuration
2. Reduce `batch_size` for more frequent processing
3. Reduce `batch_timeout_ms` for faster batching
4. Check backend response times
5. Increase `http_retries` if backend is unreliable

## Advanced Testing

### Manual HTTP Testing
```bash
curl -X POST http://localhost:8080/v1/logs \
  -H "Content-Type: application/json" \
  -d '{
    "logs": [
      {
        "program": "test",
        "message": "Manual test message",
        "facility": "user",
        "priority": "info",
        "timestamp": 1640995200
      }
    ],
    "count": 1,
    "collector_version": "manual-test"
  }'
```

### UBUS Testing
```bash
# List UBUS objects
ubus list

# Subscribe to log events manually
ubus subscribe log

# Send test log
logger -t "test-app" -p user.info "Test message"
```

### Performance Testing
Monitor collector performance:
```bash
# CPU usage
top -p $(pgrep collector)

# Memory usage
cat /proc/$(pgrep collector)/status | grep VmRSS

# Network connections
netstat -tp | grep collector
```

## Development Tips

1. **Use smaller batch sizes** (`batch_size: 5-10`) for faster testing feedback
2. **Enable verbose logging** (`verbose_logging: 1`) to see internal operations
3. **Use mock backend** to avoid external dependencies
4. **Test different log volumes** to verify scalability
5. **Monitor queue statistics** to understand behavior
6. **Test network failures** by stopping mock backend
7. **Modify configuration** without recompiling to test different settings
8. **Use development configuration** optimized for local testing

## Integration Testing

To test with actual system logs:
```bash
# Generate real system activity
sudo systemctl restart networking
ssh localhost  # Generate auth logs
wget http://example.com  # Generate network logs
```

The collector will automatically pick up these real system logs along with your test messages.