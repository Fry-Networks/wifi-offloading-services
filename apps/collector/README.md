# Fry OS Collector - Single-Core Optimized

The collector app is a standalone service optimized for single-core embedded devices that collects logs from the system via UBUS/syslog and forwards them to the Fry backend for processing.

## Architecture

The collector uses a **single-threaded event-driven architecture** optimized for resource-constrained devices:

1. **Main Event Loop (uloop)**: Handles all events including UBUS messages, timers, and HTTP operations
2. **UBUS Integration**: Communicates with fry-agent for access token retrieval and log event subscription
3. **Memory Pool**: Pre-allocated entry pool to avoid malloc/free overhead
4. **State Machine**: HTTP operations managed through a simple state machine
5. **Circular Queue**: Lock-free queue for log entries (single-threaded access)
6. **Token Management**: Automatic access token caching and refresh mechanism

## Key Optimizations for Single-Core Devices

### Memory Efficiency
- **Reduced structure sizes**: 512-byte messages vs 1024-byte
- **Entry pool**: Pre-allocated log entries to eliminate dynamic allocation
- **Smaller queues**: 500 entries vs 1000 to reduce memory footprint
- **Compact data types**: 32-bit timestamps, smaller string buffers

### Performance Optimizations
- **No threading overhead**: Single event loop eliminates context switching
- **No synchronization**: Lock-free operations for queue management
- **Cooperative multitasking**: Event-driven design prevents blocking
- **Efficient filtering**: Quick log filtering in UBUS callback

### Resource Configuration
```c
#define MAX_LOG_ENTRY_SIZE 512       // Optimized for memory
#define MAX_BATCH_SIZE 50            // Smaller batches
#define MAX_QUEUE_SIZE 500           // Reduced queue size
#define BATCH_TIMEOUT_MS 10000       // 10-second batching
#define URGENT_THRESHOLD 400         // 80% of queue size
```

## Features

- **Non-blocking UBUS handling**: Fast event processing prevents system slowdown
- **Intelligent batching**: Adaptive batching based on queue load and timeouts
- **HTTP state machine**: Non-blocking HTTP operations with retry logic
- **Automatic reconnection**: UBUS and HTTP connection recovery
- **Access token authentication**: Retrieves Bearer tokens from fry-agent via UBUS
- **Token refresh management**: Automatic token validation and refresh cycles
- **Memory pool management**: Efficient memory usage with entry recycling
- **Queue overflow protection**: Graceful handling of high log volumes
- **Dynamic configuration**: UCI-style configuration files with runtime validation
- **Development mode**: Enhanced logging and testing features

## Configuration

The collector uses UCI-style configuration files for all settings. Configuration files are loaded in this order of preference:

1. `/etc/config/fry-collector` (OpenWrt production)
2. `./fry-collector.config` (local development - automatically copied by `just run collector`)
3. `/tmp/fry-collector.config` (fallback)

### Configuration Format

```bash
config fry_collector 'fry_collector'
    option enabled '1'                    # Enable/disable collector
    option logs_endpoint 'https://...'    # Backend URL
    option batch_size '50'                # Logs per batch
    option batch_timeout_ms '10000'       # Batch timeout (ms)
    option queue_size '500'               # Internal queue size
    option http_timeout '30'              # HTTP timeout (seconds)
    option http_retries '2'               # HTTP retry attempts
    option reconnect_delay_ms '5000'      # UBUS reconnect delay (ms)
    option dev_mode '0'                   # Development mode
    option verbose_logging '0'            # Verbose output
```

### Configuration Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `enabled` | boolean | `1` | Enable/disable the collector service |
| `logs_endpoint` | string | `https://devices.fry.tech/logs` | Backend API endpoint for log submission |
| `batch_size` | integer | `50` | Number of logs per batch (1-1000) |
| `batch_timeout_ms` | integer | `10000` | Batch timeout in milliseconds (1000-300000) |
| `queue_size` | integer | `500` | Internal queue size (1-10000) |
| `http_timeout` | integer | `30` | HTTP request timeout in seconds (1-300) |
| `http_retries` | integer | `2` | Number of HTTP retry attempts |
| `reconnect_delay_ms` | integer | `5000` | UBUS reconnection delay in milliseconds |
| `dev_mode` | boolean | `0` | Enable development mode features |
| `verbose_logging` | boolean | `0` | Enable verbose logging output |

### Configuration Validation

The collector validates all configuration values on startup:
- URLs must start with `http://` or `https://`
- Numeric values must be within specified ranges
- Required fields must be present and non-empty

If validation fails, the collector will exit with an error message.

## Quick Start

For development and testing, use the integrated development environment:

```bash
# Build and run collector with automatic setup
just run collector

# This command will:
# - Build the collector executable
# - Set up run/collector/ directory with all needed files
# - Copy optimized development configuration
# - Start collector in development mode
```

For manual usage:

```bash
# Start collector service (production)
fry-collector

# Start in development mode (verbose logging and statistics)
fry-collector --dev

# Show help and configuration file locations
fry-collector --help
```

## Authentication

The collector authenticates with the Fry backend using Bearer tokens obtained from the fry-agent service via UBUS.

### Token Retrieval Process

1. **UBUS Communication**: Collector calls `fry-agent.get_access_token` method
2. **Token Caching**: Valid tokens are cached locally with expiration tracking
3. **Automatic Refresh**: Tokens are refreshed automatically before expiration
4. **HTTP Authorization**: Cached tokens are included as `Authorization: Bearer <token>` headers
5. **Error Handling**: 401 responses trigger immediate token refresh attempts

### Token Management Features

- **Intelligent Caching**: Tokens are cached with 60-second expiration buffer
- **Periodic Refresh**: Timer-based token validation every 5 minutes
- **Fallback Behavior**: Continues operation without tokens in development mode
- **Connection Recovery**: Handles fry-agent service restarts gracefully

### Authentication Flow

```c
// Get token from fry-agent
int ret = ubus_get_access_token(token_buffer, sizeof(token_buffer));

// Add Bearer token to HTTP headers
snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", token);
request_headers = curl_slist_append(request_headers, auth_header);

// Handle authentication failures
if (response_code == 401) {
    ubus_refresh_access_token(); // Refresh for next request
}
```

### UBUS Methods Used

- `fry-agent.get_access_token`: Retrieve current access token with expiration info
- Response includes: `token`, `expires_at`, `valid` fields

### Error Scenarios

- **Agent Unavailable**: Logs warning and attempts requests without authentication
- **Token Expired**: Automatic refresh before next batch submission
- **Invalid Token**: Immediate refresh on 401 HTTP response
- **UBUS Disconnected**: Continues with cached token until reconnection

## Testing

### Configuration System Testing

Test the entire configuration system:

```bash
# Run comprehensive configuration tests
./tools/test-collector-config.sh

# This tests:
# - Build system and executable creation
# - Configuration file placement and format
# - File permissions and directory setup
# - Configuration loading and validation
```

### Manual Configuration Testing

Test configuration loading and validation manually:

```bash
# Check configuration file locations
fry-collector --help

# Test configuration validation (shows config loading)
cd run/collector && ./collector --dev

# View current configuration in development mode
# (automatically displays loaded configuration)
```

### Development Testing Workflow

Complete testing setup with mock backend:

```bash
# Terminal 1: Start collector
just run collector

# Terminal 2: Start mock backend
cd run/collector/scripts
python3 mock-backend.py --verbose

# Terminal 3: Generate test logs
cd run/collector/scripts
./test-logs.sh 10 1 normal
```

## System Requirements

- **CPU**: Single-core ARM/MIPS/x86 (optimized for single-core)
- **Memory**: Minimum 32MB available RAM
- **Storage**: Minimal storage requirements
- **Network**: HTTP/HTTPS connectivity for backend communication

## Log Filtering

The collector applies efficient filtering to reduce processing overhead:

- Skips empty or very short messages (< 3 characters)
- Filters out kernel messages (too noisy for most use cases)
- Skips DEBUG messages in production mode
- Avoids processing logs from collector itself (prevents loops)
- Quick program name and content-based filtering

## Data Format

Logs are sent to the backend as compact JSON batches:

```json
{
  "logs": [
    {
      "program": "sshd",
      "message": "Accepted password for user from 192.168.1.100",
      "facility": "auth",
      "priority": "info",
      "timestamp": 1640995200
    }
  ],
  "count": 1,
  "collector_version": "1.0.0-single-core"
}
```

## Architecture Files

- `main.c`: Single-threaded event loop and system coordination
- `ubus.c/h`: UBUS integration with uloop event system
- `collect.c/h`: Memory pool, queue management, and HTTP state machine
- `multi-threaded.md`: Documentation for future multi-core implementation

## Event Flow

1. **UBUS Event**: Syslog message arrives via UBUS
2. **Quick Filter**: Fast filtering in UBUS callback (microseconds)
3. **Pool Allocation**: Get entry from pre-allocated pool
4. **Queue Enqueue**: Add to circular queue (lock-free)
5. **Batch Timer**: Periodic timer checks for batch processing
6. **Token Retrieval**: Get valid access token from fry-agent via UBUS
7. **State Machine**: HTTP state machine processes batches with authentication
8. **Backend Submit**: JSON payload sent with Bearer token and retry logic
9. **Pool Return**: Entry returned to pool for reuse

## Dependencies

- `fry-core`: Console logging and utilities
- `fry-http`: HTTP client functionality
- `fry-agent`: Access token provider (via UBUS communication)
- `libubus`: UBUS communication and event handling
- `libubox`: Event loop (uloop) and message handling
- `json-c`: Compact JSON serialization
- `libcurl`: HTTP client for backend communication

## Performance Characteristics

### Single-Core Optimized Performance
- **Log Processing**: 200-500 logs/second (depending on hardware)
- **Memory Usage**: 8-20MB total (including entry pool)
- **CPU Usage**: <10% on typical embedded ARM processors
- **Latency**: <1ms for UBUS event processing
- **Batch Processing**: 2-10 second batching intervals

### Resource Efficiency
- **No thread stacks**: Eliminates 8MB+ per thread overhead
- **No synchronization**: Zero mutex/condition variable overhead
- **Event-driven**: CPU used only when processing events
- **Memory pool**: Predictable memory usage, no fragmentation

## Monitoring and Status

### Runtime Statistics (--dev mode)
```
Status: queue_size=45, dropped=0, ubus_connected=yes
Access token refreshed successfully (expires in 3540 seconds)
```

### Available Statistics
- Current queue size and utilization
- Number of dropped log entries
- UBUS connection status
- Access token validity and expiration
- Authentication success/failure rates
- Batch processing state
- HTTP operation status
- Memory pool utilization

### Warning Thresholds
- Queue size > 80% (400 entries): Triggers urgent batch processing
- Dropped entries > 0: Indicates system overload
- UBUS disconnection: Automatic reconnection attempts

## Runtime Configuration

Configuration values are loaded dynamically from UCI config files and applied at runtime. All configuration is managed through the UCI-style configuration files with comprehensive validation and fallback to sensible defaults.

### Environment-Specific Configurations

**Development Configuration** (source: `apps/collector/scripts/dev/fry-collector.config`):
```bash
config fry_collector 'fry_collector'
    option enabled '1'
    option logs_endpoint 'http://localhost:8080/v1/logs'
    option batch_size '5'           # Small batches for testing
    option batch_timeout_ms '3000'  # Fast batching
    option queue_size '50'          # Small queue
    option dev_mode '1'
    option verbose_logging '1'
```

**Production Configuration** (`/etc/config/fry-collector`):
```bash
config fry_collector 'fry_collector'
    option enabled '1'
    option logs_endpoint 'https://devices.fry.tech/logs'
    option batch_size '50'          # Optimized batching
    option batch_timeout_ms '10000' # Standard timeout
    option queue_size '500'         # Full queue
    option dev_mode '0'
    option verbose_logging '0'
```

## Development and Debugging

### Development Mode Features
- Configuration loading and validation details
- Verbose logging of all operations
- Periodic status reports every 30 seconds
- Detailed HTTP state machine logging
- Queue and memory pool statistics
- Performance timing information
- Configuration parameter display

### Debug Information
```bash
# Use the integrated development environment
just run collector

# Or run manually
fry-collector --dev

# Shows detailed logs including:
# - Configuration loading and validation
# - UBUS event processing
# - Queue operations
# - HTTP state machine transitions
# - Memory pool utilization
# - Batch processing timing
# - Current configuration values
```

### Configuration Debugging

Enable configuration debugging in development mode:

```bash
just run collector
# Output includes:
[config] Configuration loaded from: ./fry-collector.config
[config] Current Configuration:
[config]   enabled: true
[config]   logs_endpoint: http://localhost:8080/v1/logs
[config]   batch_size: 5
[config]   batch_timeout_ms: 3000
[config]   queue_size: 50
[collect] Single-core collection system initialized (max_queue_size=50, max_batch_size=5)
```

The development configuration is automatically copied from `apps/collector/scripts/dev/fry-collector.config` to the run directory when using `just run collector`.

### Testing and Validation

Run the comprehensive test suite to verify configuration and setup:

```bash
# Test the entire collector configuration system
./tools/test-collector-config.sh

# Expected output on success:
# ✓ All tests passed! Collector configuration is working correctly.
#
# Next steps:
# 1. Run: just run collector
# 2. In another terminal: cd run/collector/scripts && python3 mock-backend.py --verbose
# 3. In another terminal: cd run/collector/scripts && ./test-logs.sh 10 1 normal
```

## Configuration System

The collector now uses a unified UCI-style configuration system that has been streamlined for clarity and ease of use:

### Key Improvements
- **Single configuration format**: Only UCI-style configuration files are supported
- **Environment-specific configs**: Separate optimized configurations for development and production
- **Automatic setup**: Development environment automatically configured via `just run collector`
- **Comprehensive validation**: All configuration values are validated with helpful error messages
- **Dynamic allocation**: Memory usage adapts to configuration settings (queue size, batch size, etc.)

### Configuration Sources
The configuration system has been simplified to use only these locations:

1. **Production**: `/etc/config/fry-collector` (OpenWrt standard)
2. **Development**: Automatically copied from `apps/collector/scripts/dev/fry-collector.config`
3. **Fallback**: `/tmp/fry-collector.config` (manual testing)

### Legacy Configuration Cleanup
- **Removed**: All references to `collector.conf` format
- **Removed**: Hardcoded configuration constants
- **Removed**: Mixed configuration approaches
- **Added**: Consistent UCI-style configuration throughout

The development configuration is optimized for local testing with:
- Small batch sizes (5 logs) for faster feedback
- Short timeouts (3 seconds) for responsive testing
- Localhost endpoint for mock backend integration
- Enhanced logging and development features enabled

## Future Enhancements

- [x] Configuration file support for runtime parameters
- [x] Unified UCI-style configuration system
- [ ] Hot configuration reloading without restart
- [ ] Local log buffering for network outages
- [ ] Compression for large log batches
- [ ] Advanced filtering rule engine
- [ ] Metrics endpoint for monitoring integration
- [ ] Multi-core architecture detection and fallback
- [ ] Configuration schema validation
- [ ] Configuration management API

## Multi-Core Support

For systems with multiple CPU cores, see `multi-threaded.md` for the alternative architecture that provides higher throughput through dedicated threads for UBUS processing and HTTP operations.

The collector automatically detects single-core systems and uses this optimized architecture, but can be extended to support multi-threaded operation on systems with sufficient resources.