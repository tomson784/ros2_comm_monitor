"""ROS 2 web server for browser-based topic monitor UI."""

import json
import threading
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Optional

import rclpy
from ament_index_python.packages import get_package_share_directory
from rclpy.node import Node
from std_msgs.msg import String


class SharedState:
    """Thread-safe in-memory storage for the latest JSON payloads."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._latest_stats_json = json.dumps(
            {"generated_at_sec": None, "topic_count": 0, "topics": []}
        )
        self._latest_graph_json = json.dumps(
            {"node_count": 0, "edge_count": 0, "nodes": [], "edges": []}
        )

    def set_latest_stats_json(self, data: str) -> None:
        """Store latest stats JSON produced by ROS backend."""
        with self._lock:
            self._latest_stats_json = data

    def get_latest_stats_json(self) -> str:
        """Return latest stats JSON as-is (already serialized string)."""
        with self._lock:
            return self._latest_stats_json

    def set_latest_graph_json(self, data: str) -> None:
        """Store latest graph JSON produced by ROS backend."""
        with self._lock:
            self._latest_graph_json = data

    def get_latest_graph_json(self) -> str:
        """Return latest graph JSON as-is (already serialized string)."""
        with self._lock:
            return self._latest_graph_json


class TopicMonitorWebServerNode(Node):
    """ROS node bridging topic payloads into a lightweight HTTP server."""

    def __init__(self) -> None:
        super().__init__("topic_monitor_web_server")

        self.declare_parameter("host", "0.0.0.0")
        self.declare_parameter("port", 8080)
        self.declare_parameter("stats_topic", "/topic_monitor/stats_json")
        self.declare_parameter("graph_topic", "/topic_monitor/graph_json")

        self._host = self.get_parameter("host").value
        self._port = int(self.get_parameter("port").value)
        self._stats_topic = self.get_parameter("stats_topic").value
        self._graph_topic = self.get_parameter("graph_topic").value

        self._shared_state = SharedState()
        self._http_server: Optional[ThreadingHTTPServer] = None
        self._http_thread: Optional[threading.Thread] = None

        self._stats_subscription = self.create_subscription(
            String, self._stats_topic, self._stats_callback, 10
        )
        self._graph_subscription = self.create_subscription(
            String, self._graph_topic, self._graph_callback, 10
        )

        self._start_http_server()

    def destroy_node(self) -> bool:
        """Ensure HTTP server thread is stopped before node destruction."""
        self._stop_http_server()
        return super().destroy_node()

    def _stats_callback(self, msg: String) -> None:
        """ROS callback for stats JSON stream."""
        self._shared_state.set_latest_stats_json(msg.data)

    def _graph_callback(self, msg: String) -> None:
        """ROS callback for graph JSON stream."""
        self._shared_state.set_latest_graph_json(msg.data)

    def _start_http_server(self) -> None:
        """Start threaded HTTP server that serves static files and APIs."""
        static_dir = Path(get_package_share_directory("topic_monitor_web_server")) / "static"
        handler_cls = make_request_handler(self._shared_state, static_dir)

        self._http_server = ThreadingHTTPServer((self._host, self._port), handler_cls)
        self._http_thread = threading.Thread(
            target=self._http_server.serve_forever,
            daemon=True,
        )
        self._http_thread.start()

    def _stop_http_server(self) -> None:
        """Stop HTTP server and join serving thread."""
        if self._http_server is not None:
            self._http_server.shutdown()
            self._http_server.server_close()
            self._http_server = None

        if self._http_thread is not None and self._http_thread.is_alive():
            self._http_thread.join(timeout=2.0)
            self._http_thread = None


def make_request_handler(shared_state: SharedState, static_dir: Path):
    """Create request handler class bound to shared state and static root."""

    class RequestHandler(BaseHTTPRequestHandler):
        server_version = "TopicMonitorWebServer/1.0"

        def do_GET(self) -> None:  # noqa: N802
            # Static assets used by browser UI.
            if self.path == "/" or self.path == "/index.html":
                self._serve_file(static_dir / "index.html", "text/html; charset=utf-8")
                return

            if self.path == "/styles.css":
                self._serve_file(static_dir / "styles.css", "text/css; charset=utf-8")
                return

            if self.path == "/app.js":
                self._serve_file(static_dir / "app.js", "application/javascript; charset=utf-8")
                return

            # API endpoints providing latest ROS snapshots.
            if self.path == "/api/stats":
                payload = shared_state.get_latest_stats_json().encode("utf-8")
                self.send_response(HTTPStatus.OK)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Cache-Control", "no-store")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
                return

            if self.path == "/api/graph":
                payload = shared_state.get_latest_graph_json().encode("utf-8")
                self.send_response(HTTPStatus.OK)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Cache-Control", "no-store")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
                return

            self.send_error(HTTPStatus.NOT_FOUND, "Not Found")

        def log_message(self, format: str, *args) -> None:
            # Keep ROS console clean by suppressing per-request HTTP logs.
            return

        def _serve_file(self, path: Path, content_type: str) -> None:
            # Guard against missing files and return explicit 404.
            if not path.exists() or not path.is_file():
                self.send_error(HTTPStatus.NOT_FOUND, "File not found")
                return

            payload = path.read_bytes()
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", content_type)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

    return RequestHandler


def main(args=None) -> None:
    """Process entrypoint used by `ros2 run topic_monitor_web_server ...`."""
    rclpy.init(args=args)
    node = TopicMonitorWebServerNode()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        node.destroy_node()
        rclpy.shutdown()
