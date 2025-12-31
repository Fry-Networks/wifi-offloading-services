#!/usr/bin/env python3

"""
Fry OS Collector - Mock Backend Server
Development server for testing collector log submission functionality
"""

import json
import time
import random
from datetime import datetime
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs
import argparse
import threading

class MockBackendHandler(BaseHTTPRequestHandler):
    """HTTP request handler for mock backend server"""

    def __init__(self, *args, config=None, **kwargs):
        self.config = config or {}
        super().__init__(*args, **kwargs)

    def do_POST(self):
        """Handle POST requests from collector"""
        if self.path == '/v1/logs':
            self._handle_logs_submission()
        else:
            self._send_error(404, "Not Found")

    def do_GET(self):
        """Handle GET requests for status/health checks"""
        if self.path == '/health':
            self._handle_health_check()
        elif self.path == '/stats':
            self._handle_stats_request()
        else:
            self._send_error(404, "Not Found")

    def _handle_logs_submission(self):
        """Process log submission from collector"""
        try:
            # Read request body
            content_length = int(self.headers.get('Content-Length', 0))
            if content_length == 0:
                self._send_error(400, "Empty request body")
                return

            post_data = self.rfile.read(content_length)

            # Parse JSON
            try:
                log_data = json.loads(post_data.decode('utf-8'))
            except json.JSONDecodeError as e:
                self._send_error(400, f"Invalid JSON: {e}")
                return

            # Validate log data structure
            if not self._validate_log_data(log_data):
                self._send_error(400, "Invalid log data structure")
                return

            # Simulate processing time
            if self.config.get('simulate_delay', False):
                delay = random.uniform(0.1, 2.0)
                time.sleep(delay)

            # Simulate failures
            if self.config.get('simulate_failures', False):
                failure_rate = self.config.get('failure_rate', 0.1)
                if random.random() < failure_rate:
                    self._send_error(500, "Simulated server error")
                    return

            # Log the received data
            self._log_received_data(log_data)

            # Send success response
            response = {
                "status": "success",
                "received_count": log_data.get('count', 0),
                "timestamp": datetime.utcnow().isoformat(),
                "message": "Logs processed successfully"
            }

            self._send_json_response(200, response)

        except Exception as e:
            print(f"Error processing request: {e}")
            self._send_error(500, "Internal server error")

    def _handle_health_check(self):
        """Handle health check requests"""
        health_data = {
            "status": "healthy",
            "timestamp": datetime.utcnow().isoformat(),
            "uptime": time.time() - getattr(self.server, 'start_time', time.time()),
            "version": "mock-1.0.0"
        }
        self._send_json_response(200, health_data)

    def _handle_stats_request(self):
        """Handle statistics requests"""
        stats = getattr(self.server, 'stats', {
            'total_requests': 0,
            'total_logs': 0,
            'errors': 0
        })

        stats_data = {
            "statistics": stats,
            "timestamp": datetime.utcnow().isoformat()
        }
        self._send_json_response(200, stats_data)

    def _validate_log_data(self, data):
        """Validate log data structure"""
        required_fields = ['logs', 'count', 'collector_version']

        # Check required top-level fields
        for field in required_fields:
            if field not in data:
                print(f"Missing required field: {field}")
                return False

        # Validate logs array
        logs = data.get('logs', [])
        if not isinstance(logs, list):
            print("Logs field must be an array")
            return False

        # Validate individual log entries
        for i, log_entry in enumerate(logs):
            if not self._validate_log_entry(log_entry, i):
                return False

        return True

    def _validate_log_entry(self, entry, index):
        """Validate individual log entry"""
        required_fields = ['program', 'message', 'timestamp']

        for field in required_fields:
            if field not in entry:
                print(f"Log entry {index}: missing required field: {field}")
                return False

        # Validate data types
        if not isinstance(entry['timestamp'], int):
            print(f"Log entry {index}: timestamp must be integer")
            return False

        if not isinstance(entry['message'], str) or len(entry['message']) == 0:
            print(f"Log entry {index}: message must be non-empty string")
            return False

        return True

    def _log_received_data(self, data):
        """Log received data for debugging"""
        timestamp = datetime.now().strftime('%H:%M:%S')
        count = data.get('count', 0)
        version = data.get('collector_version', 'unknown')

        print(f"[{timestamp}] Received batch: {count} logs from collector v{version}")

        # Update server statistics
        if not hasattr(self.server, 'stats'):
            self.server.stats = {'total_requests': 0, 'total_logs': 0, 'errors': 0}

        self.server.stats['total_requests'] += 1
        self.server.stats['total_logs'] += count

        # Print log details in verbose mode
        if self.config.get('verbose', False):
            for i, log_entry in enumerate(data.get('logs', [])):
                program = log_entry.get('program', 'unknown')
                message = log_entry.get('message', '')
                facility = log_entry.get('facility', '')
                priority = log_entry.get('priority', '')

                print(f"  [{i+1}] {program}.{facility}.{priority}: {message[:100]}")

    def _send_json_response(self, status_code, data):
        """Send JSON response"""
        response_json = json.dumps(data, indent=2)

        self.send_response(status_code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(response_json)))
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()

        self.wfile.write(response_json.encode('utf-8'))

    def _send_error(self, status_code, message):
        """Send error response"""
        error_data = {
            "error": message,
            "status_code": status_code,
            "timestamp": datetime.utcnow().isoformat()
        }

        # Update error count
        if hasattr(self.server, 'stats'):
            self.server.stats['errors'] += 1

        print(f"Error {status_code}: {message}")
        self._send_json_response(status_code, error_data)

    def log_message(self, format, *args):
        """Override to customize logging"""
        if self.config.get('verbose', False):
            timestamp = datetime.now().strftime('%H:%M:%S')
            print(f"[{timestamp}] {format % args}")

class ConfigurableHTTPServer(HTTPServer):
    """HTTP server that passes configuration to handlers"""

    def __init__(self, server_address, RequestHandlerClass, config=None):
        self.config = config or {}
        self.start_time = time.time()
        self.stats = {'total_requests': 0, 'total_logs': 0, 'errors': 0}
        super().__init__(server_address, RequestHandlerClass)

    def finish_request(self, request, client_address):
        """Override to pass config to handler"""
        self.RequestHandlerClass(request, client_address, self, config=self.config)

def print_startup_info(host, port, config):
    """Print server startup information"""
    print("=" * 60)
    print("Fry OS Collector - Mock Backend Server")
    print("=" * 60)
    print(f"Server URL: http://{host}:{port}")
    print(f"Logs endpoint: http://{host}:{port}/v1/logs")
    print(f"Health check: http://{host}:{port}/health")
    print(f"Statistics: http://{host}:{port}/stats")
    print()
    print("Configuration:")
    print(f"  Verbose logging: {config.get('verbose', False)}")
    print(f"  Simulate failures: {config.get('simulate_failures', False)}")
    if config.get('simulate_failures'):
        print(f"  Failure rate: {config.get('failure_rate', 0.1) * 100:.1f}%")
    print(f"  Simulate delays: {config.get('simulate_delay', False)}")
    print()
    print("Test your collector with:")
    print(f"  curl -X POST http://{host}:{port}/v1/logs -d '{{\"logs\":[], \"count\":0, \"collector_version\":\"test\"}}'")
    print()
    print("Press Ctrl+C to stop the server")
    print("=" * 60)

def main():
    parser = argparse.ArgumentParser(description='Mock backend server for collector development')
    parser.add_argument('--host', default='localhost', help='Server host (default: localhost)')
    parser.add_argument('--port', type=int, default=8080, help='Server port (default: 8080)')
    parser.add_argument('--verbose', action='store_true', help='Enable verbose logging')
    parser.add_argument('--simulate-failures', action='store_true', help='Simulate random failures')
    parser.add_argument('--failure-rate', type=float, default=0.1, help='Failure rate (0.0-1.0, default: 0.1)')
    parser.add_argument('--simulate-delay', action='store_true', help='Simulate processing delays')

    args = parser.parse_args()

    # Configuration
    config = {
        'verbose': args.verbose,
        'simulate_failures': args.simulate_failures,
        'failure_rate': max(0.0, min(1.0, args.failure_rate)),
        'simulate_delay': args.simulate_delay
    }

    # Create and start server
    server = ConfigurableHTTPServer((args.host, args.port), MockBackendHandler, config)

    print_startup_info(args.host, args.port, config)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down server...")
        server.shutdown()
        server.server_close()

        # Print final statistics
        stats = getattr(server, 'stats', {})
        uptime = time.time() - getattr(server, 'start_time', time.time())

        print("\nFinal Statistics:")
        print(f"  Uptime: {uptime:.1f} seconds")
        print(f"  Total requests: {stats.get('total_requests', 0)}")
        print(f"  Total logs processed: {stats.get('total_logs', 0)}")
        print(f"  Errors: {stats.get('errors', 0)}")

        if stats.get('total_requests', 0) > 0:
            avg_logs = stats.get('total_logs', 0) / stats.get('total_requests', 1)
            print(f"  Average logs per request: {avg_logs:.1f}")

        print("Server stopped.")

if __name__ == '__main__':
    main()
