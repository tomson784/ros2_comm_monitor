"""ROS 2 web server for browser-based topic monitor UI.

The server aggregates stats/graph JSON published by one or more monitor agents
(one agent per host) and serves a merged view to the browser.
"""

import json
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional

import rclpy
from ament_index_python.packages import get_package_share_directory
from rclpy.node import Node
from rclpy.qos import HistoryPolicy, QoSProfile, ReliabilityPolicy
from std_msgs.msg import String

# Host name assigned to payloads from agents that predate multi-host support.
LEGACY_HOST = "local"

# Higher is better when several hosts report the same topic in the graph.
_TOPIC_STATUS_RANK = {"ok": 3, "stale": 2, "down": 1, "neutral": 0}


def topic_status_key(status: Any, host_online: bool) -> int:
    """Sort key for choosing a topic's merged status (higher wins).

    A judgement from an online host (ok / stale / down) beats anything an offline
    host last reported, which in turn beats "neutral" (not measured).
    Within each tier: ok > stale > down.
    """
    rank = _TOPIC_STATUS_RANK.get(status, 0)
    if rank == 0:
        return 0
    return rank + (10 if host_online else 5)


class _HostEntry:
    """Latest payloads and receive times for one agent host."""

    __slots__ = ("stats", "graph", "stats_rx", "graph_rx")

    def __init__(self) -> None:
        self.stats: Optional[Dict[str, Any]] = None
        self.graph: Optional[Dict[str, Any]] = None
        self.stats_rx: Optional[float] = None
        self.graph_rx: Optional[float] = None

    def last_seen(self) -> float:
        return max(t for t in (self.stats_rx, self.graph_rx) if t is not None)


class ClusterState:
    """Thread-safe per-host storage of agent payloads with merged views.

    The method names of the former single-host ``SharedState`` are kept so that
    existing callers keep working (``SharedState`` is an alias of this class).
    """

    def __init__(
        self,
        agent_timeout_sec: float = 5.0,
        forget_after_sec: float = 300.0,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self._lock = threading.Lock()
        self._hosts: Dict[str, _HostEntry] = {}
        self._clock = clock
        self.agent_timeout_sec = float(agent_timeout_sec)
        self.forget_after_sec = float(forget_after_sec)
        self.invalid_payload_count = 0

    # ------------------------------------------------------------------ ingest

    def _parse(self, data: str) -> Optional[Dict[str, Any]]:
        try:
            payload = json.loads(data)
        except (TypeError, ValueError):
            payload = None
        if not isinstance(payload, dict):
            self.invalid_payload_count += 1
            return None
        return payload

    @staticmethod
    def _host_of(payload: Dict[str, Any]) -> str:
        host = payload.get("host")
        if isinstance(host, str) and host:
            return host
        return LEGACY_HOST

    def set_latest_stats_json(self, data: str) -> bool:
        """Store a stats JSON payload. Returns False (and ignores it) when invalid."""
        with self._lock:
            payload = self._parse(data)
            if payload is None:
                return False
            entry = self._hosts.setdefault(self._host_of(payload), _HostEntry())
            entry.stats = payload
            entry.stats_rx = self._clock()
            return True

    def set_latest_graph_json(self, data: str) -> bool:
        """Store a graph JSON payload. Returns False (and ignores it) when invalid."""
        with self._lock:
            payload = self._parse(data)
            if payload is None:
                return False
            entry = self._hosts.setdefault(self._host_of(payload), _HostEntry())
            entry.graph = payload
            entry.graph_rx = self._clock()
            return True

    # ------------------------------------------------------------------ status

    def _prune_locked(self, now: float) -> None:
        expired = [
            host for host, entry in self._hosts.items()
            if now - entry.last_seen() > self.forget_after_sec
        ]
        for host in expired:
            del self._hosts[host]

    def _is_online(self, entry: _HostEntry, now: float) -> bool:
        return now - entry.last_seen() <= self.agent_timeout_sec

    def _host_summaries_locked(self, now: float) -> List[Dict[str, Any]]:
        summaries = []
        for host in sorted(self._hosts):
            entry = self._hosts[host]
            stats = entry.stats or {}
            topics = [t for t in stats.get("topics") or [] if isinstance(t, dict)]
            summaries.append({
                "host": host,
                "online": self._is_online(entry, now),
                "last_seen_age_sec": round(now - entry.last_seen(), 3),
                "agent_node": stats.get("agent_node"),
                "rmw": stats.get("rmw"),
                "locality_mode": stats.get("locality_mode"),
                "subscribe_policy": stats.get("subscribe_policy"),
                "schema_version": stats.get("schema_version", 1),
                "generated_at_sec": stats.get("generated_at_sec"),
                "topic_count": len(topics),
                "monitored_topic_count": sum(1 for t in topics if t.get("monitored", True)),
                "has_stats": entry.stats is not None,
                "has_graph": entry.graph is not None,
            })
        return summaries

    # ------------------------------------------------------------------ views

    def get_hosts(self) -> Dict[str, Any]:
        """Return per-host status (online/offline, last seen, topic counts)."""
        with self._lock:
            now = self._clock()
            self._prune_locked(now)
            hosts = self._host_summaries_locked(now)
        return {
            "agent_timeout_sec": self.agent_timeout_sec,
            "forget_after_sec": self.forget_after_sec,
            "host_count": len(hosts),
            "online_host_count": sum(1 for h in hosts if h["online"]),
            "hosts": hosts,
        }

    def get_hosts_json(self) -> str:
        return json.dumps(self.get_hosts())

    def get_merged_stats(self) -> Dict[str, Any]:
        """Merge stats from every host: one row per (host, topic)."""
        with self._lock:
            now = self._clock()
            self._prune_locked(now)
            hosts = self._host_summaries_locked(now)
            online = {h["host"]: h["online"] for h in hosts}
            rows: List[Dict[str, Any]] = []
            generated_at = None
            for host in sorted(self._hosts):
                stats = self._hosts[host].stats
                if not stats:
                    continue
                gen = stats.get("generated_at_sec")
                if isinstance(gen, (int, float)) and (generated_at is None or gen > generated_at):
                    generated_at = gen
                for topic in stats.get("topics") or []:
                    if not isinstance(topic, dict):
                        continue
                    row = dict(topic)
                    row["host"] = host
                    row.setdefault("monitored", True)
                    row["host_online"] = online[host]
                    if not online[host]:
                        row["stale"] = True
                    rows.append(row)

        rows.sort(key=lambda r: (str(r.get("name", "")), str(r.get("host", ""))))
        return {
            "schema_version": 2,
            "generated_at_sec": generated_at,
            "topic_count": len(rows),
            "host_count": len(hosts),
            "online_host_count": sum(1 for h in hosts if h["online"]),
            "hosts": hosts,
            "topics": rows,
        }

    def get_merged_graph(self) -> Dict[str, Any]:
        """Union per-host graphs.

        Topic status: the best of the hosts' statuses (ok > stale > down > neutral),
        where online hosts' judgements take precedence over offline hosts' (see
        ``topic_status_key``).
        """
        with self._lock:
            now = self._clock()
            self._prune_locked(now)
            nodes: Dict[str, Dict[str, Any]] = {}
            topic_keys: Dict[str, int] = {}
            edges: Dict[str, Dict[str, Any]] = {}
            for host in sorted(self._hosts):
                entry = self._hosts[host]
                if not entry.graph:
                    continue
                host_online = self._is_online(entry, now)
                for node in entry.graph.get("nodes") or []:
                    if not isinstance(node, dict) or not node.get("id"):
                        continue
                    node = dict(node)
                    node_type = node.get("node_type")
                    if node_type == "host":
                        node["status"] = "ok" if host_online else "down"
                    elif node_type == "topic" and not host_online and node.get("status") == "ok":
                        # Data from an offline agent cannot vouch for a healthy topic.
                        node["status"] = "stale"
                    existing = nodes.get(node["id"])
                    key = topic_status_key(node.get("status"), host_online)
                    if existing is None:
                        nodes[node["id"]] = node
                        topic_keys[node["id"]] = key
                    elif node_type == "topic" and key > topic_keys.get(node["id"], 0):
                        existing["status"] = node.get("status")
                        topic_keys[node["id"]] = key
                for edge in entry.graph.get("edges") or []:
                    if isinstance(edge, dict) and edge.get("id") and edge["id"] not in edges:
                        edges[edge["id"]] = dict(edge)

        node_list = sorted(
            nodes.values(), key=lambda n: (str(n.get("node_type", "")), str(n.get("id", "")))
        )
        edge_list = sorted(edges.values(), key=lambda e: str(e.get("id", "")))
        return {
            "node_count": len(node_list),
            "edge_count": len(edge_list),
            "nodes": node_list,
            "edges": edge_list,
        }

    # Backward-compatible accessors (formerly returned the raw latest payload).
    def get_latest_stats_json(self) -> str:
        """Return merged stats JSON."""
        return json.dumps(self.get_merged_stats())

    def get_latest_graph_json(self) -> str:
        """Return merged graph JSON."""
        return json.dumps(self.get_merged_graph())


# Backward-compatible name.
SharedState = ClusterState


class TopicMonitorWebServerNode(Node):
    """ROS node bridging topic payloads into a lightweight HTTP server."""

    def __init__(self) -> None:
        super().__init__("topic_monitor_web_server")

        self.declare_parameter("host", "0.0.0.0")
        self.declare_parameter("port", 8080)
        self.declare_parameter("stats_topic", "/topic_monitor/stats_json")
        self.declare_parameter("graph_topic", "/topic_monitor/graph_json")
        self.declare_parameter("agent_timeout_sec", 5.0)
        self.declare_parameter("forget_after_sec", 300.0)

        self._host = self.get_parameter("host").value
        self._port = int(self.get_parameter("port").value)
        self._stats_topic = self.get_parameter("stats_topic").value
        self._graph_topic = self.get_parameter("graph_topic").value

        self._shared_state = ClusterState(
            agent_timeout_sec=float(self.get_parameter("agent_timeout_sec").value),
            forget_after_sec=float(self.get_parameter("forget_after_sec").value),
        )
        self._http_server: Optional[ThreadingHTTPServer] = None
        self._http_thread: Optional[threading.Thread] = None

        # Reliable with a deep queue: several agents publish at about the same
        # time and must not overwrite each other before the callback runs.
        qos = QoSProfile(
            history=HistoryPolicy.KEEP_LAST,
            depth=50,
            reliability=ReliabilityPolicy.RELIABLE,
        )
        self._stats_subscription = self.create_subscription(
            String, self._stats_topic, self._stats_callback, qos
        )
        self._graph_subscription = self.create_subscription(
            String, self._graph_topic, self._graph_callback, qos
        )

        self._start_http_server()

    def destroy_node(self) -> bool:
        """Ensure HTTP server thread is stopped before node destruction."""
        self._stop_http_server()
        return super().destroy_node()

    def _warn_invalid(self, kind: str) -> None:
        self.get_logger().warn(
            f"Ignoring invalid {kind} JSON payload "
            f"(total invalid: {self._shared_state.invalid_payload_count})",
            throttle_duration_sec=10.0,
        )

    def _stats_callback(self, msg: String) -> None:
        """ROS callback for stats JSON stream."""
        if not self._shared_state.set_latest_stats_json(msg.data):
            self._warn_invalid("stats")

    def _graph_callback(self, msg: String) -> None:
        """ROS callback for graph JSON stream."""
        if not self._shared_state.set_latest_graph_json(msg.data):
            self._warn_invalid("graph")

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


def _write_json(handler: BaseHTTPRequestHandler, text: str) -> None:
    """Write a JSON response (module-level so handler stubs in tests can use it)."""
    payload = text.encode("utf-8")
    handler.send_response(HTTPStatus.OK)
    handler.send_header("Content-Type", "application/json; charset=utf-8")
    handler.send_header("Cache-Control", "no-store")
    handler.send_header("Content-Length", str(len(payload)))
    handler.end_headers()
    handler.wfile.write(payload)


def make_request_handler(shared_state: ClusterState, static_dir: Path):
    """Create request handler class bound to shared state and static root."""

    class RequestHandler(BaseHTTPRequestHandler):
        server_version = "TopicMonitorWebServer/1.0"

        def do_GET(self) -> None:  # noqa: N802
            path = self.path.split("?", 1)[0]

            # Static assets used by browser UI.
            if path == "/" or path == "/index.html":
                self._serve_file(static_dir / "index.html", "text/html; charset=utf-8")
                return

            if path == "/styles.css":
                self._serve_file(static_dir / "styles.css", "text/css; charset=utf-8")
                return

            if path == "/app.js":
                self._serve_file(static_dir / "app.js", "application/javascript; charset=utf-8")
                return

            # API endpoints providing merged ROS snapshots.
            if path == "/api/stats":
                _write_json(self, shared_state.get_latest_stats_json())
                return

            if path == "/api/graph":
                _write_json(self, shared_state.get_latest_graph_json())
                return

            if path == "/api/hosts":
                _write_json(self, shared_state.get_hosts_json())
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
