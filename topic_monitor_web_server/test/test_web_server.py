import json
from io import BytesIO
from pathlib import Path
from tempfile import TemporaryDirectory

from topic_monitor_web_server.web_server import (
    ClusterState,
    LEGACY_HOST,
    SharedState,
    make_request_handler,
)


def _invoke_get(handler_cls, path):
    # We invoke the handler directly without opening sockets.
    # This keeps tests stable in restricted CI/sandbox environments.
    class Dummy:
        def __init__(self, req_path):
            self.path = req_path
            self.status = None
            self.headers = {}
            self.wfile = BytesIO()
            self.error = None

        def send_response(self, code):
            self.status = code

        def send_header(self, key, value):
            self.headers[key] = value

        def end_headers(self):
            return

        def send_error(self, code, message=""):
            self.status = code
            self.error = message

    dummy = Dummy(path)
    dummy._serve_file = lambda p, ct: handler_cls._serve_file(dummy, p, ct)
    handler_cls.do_GET(dummy)
    return dummy


def test_shared_state_defaults_and_setters():
    # Intent: verify default payload shape and thread-safe setters/getters.
    state = SharedState()

    stats = json.loads(state.get_latest_stats_json())
    graph = json.loads(state.get_latest_graph_json())
    assert stats["topics"] == []
    assert graph["nodes"] == []
    assert graph["edges"] == []

    state.set_latest_stats_json('{"topic_count":1,"topics":[{"name":"/foo"}]}')
    state.set_latest_graph_json('{"node_count":1,"edge_count":0,"nodes":[{"id":"n1"}],"edges":[]}')

    assert json.loads(state.get_latest_stats_json())["topic_count"] == 1
    assert json.loads(state.get_latest_graph_json())["node_count"] == 1


def test_http_handler_serves_api_and_static_files():
    # Intent: validate routing behavior for APIs/static files/404.
    shared_state = SharedState()
    shared_state.set_latest_stats_json('{"topic_count":2,"topics":[{"name":"/a"},{"name":"/b"}]}')
    shared_state.set_latest_graph_json(
        '{"node_count":2,"edge_count":1,"nodes":[{"id":"n1"},{"id":"n2"}],"edges":[{"id":"e1"}]}'
    )

    with TemporaryDirectory() as tmpdir:
        static_dir = Path(tmpdir)
        (static_dir / "index.html").write_text("<html>ok</html>", encoding="utf-8")
        (static_dir / "styles.css").write_text("body{color:#fff;}", encoding="utf-8")
        (static_dir / "app.js").write_text("console.log('ok');", encoding="utf-8")

        handler_cls = make_request_handler(shared_state, static_dir)

        stats_res = _invoke_get(handler_cls, "/api/stats")
        assert stats_res.status == 200
        assert json.loads(stats_res.wfile.getvalue().decode("utf-8"))["topic_count"] == 2

        graph_res = _invoke_get(handler_cls, "/api/graph")
        assert graph_res.status == 200
        assert json.loads(graph_res.wfile.getvalue().decode("utf-8"))["edge_count"] == 1

        index_res = _invoke_get(handler_cls, "/index.html")
        assert index_res.status == 200
        assert "<html>ok</html>" in index_res.wfile.getvalue().decode("utf-8")

        not_found_res = _invoke_get(handler_cls, "/does-not-exist")
        assert not_found_res.status == 404


# --------------------------------------------------------------------------
# Distributed (multi-host) monitoring
# --------------------------------------------------------------------------


class FakeClock:
    """Injectable monotonic clock for deterministic timeout tests."""

    def __init__(self, start=1000.0):
        self.now = start

    def __call__(self):
        return self.now

    def advance(self, sec):
        self.now += sec


def _stats(host, topics, generated_at=100.0):
    return json.dumps({
        "schema_version": 2,
        "host": host,
        "agent_node": f"/comm_monitor_agent_{host}",
        "rmw": "rmw_fastrtps_cpp",
        "locality_mode": "auto",
        "subscribe_policy": "local_only",
        "generated_at_sec": generated_at,
        "topic_count": len(topics),
        "topics": topics,
    })


def _topic(name, host, alive=True, stale=False, monitored=True, locality="local", hz=10.0):
    return {
        "name": name, "type": "std_msgs/msg/String", "host": host,
        "monitored": monitored, "locality": locality,
        "local_publisher_count": 1, "remote_publisher_count": 0,
        "publisher_count": 1, "subscriber_count": 0,
        "alive": alive, "stale": stale, "hz": hz,
    }


def _graph(host, nodes, edges=()):
    host_node = {"id": f"host://{host}", "label": host, "node_type": "host",
                 "status": "ok", "host": host, "node_namespace": "", "parent_id": ""}
    all_nodes = [host_node] + list(nodes)
    return json.dumps({"schema_version": 2, "host": host, "node_count": len(all_nodes),
                       "edge_count": len(edges), "nodes": all_nodes, "edges": list(edges)})


def _topic_node(name, status):
    return {"id": name, "label": name, "node_type": "topic", "status": status,
            "host": "", "node_namespace": "", "parent_id": ""}


def _ros_node(host, fqn):
    return {"id": f"node://{host}::{fqn}", "label": fqn, "node_type": "node",
            "status": "neutral", "host": host, "node_namespace": "/", "parent_id": ""}


def test_shared_state_alias():
    # Intent: the former class name must keep working for existing callers.
    assert SharedState is ClusterState


def test_cluster_merges_stats_from_multiple_hosts():
    # Intent: one row per (host, topic); same topic on two hosts yields two rows.
    clock = FakeClock()
    state = ClusterState(clock=clock)
    assert state.set_latest_stats_json(_stats("pc_a", [_topic("/cmd", "pc_a"), _topic("/odom", "pc_a")], 100.0))
    assert state.set_latest_stats_json(_stats("pc_b", [_topic("/odom", "pc_b", monitored=False, locality="mixed")], 105.0))

    merged = state.get_merged_stats()
    rows = [(t["name"], t["host"]) for t in merged["topics"]]
    assert rows == [("/cmd", "pc_a"), ("/odom", "pc_a"), ("/odom", "pc_b")]
    assert merged["topic_count"] == 3
    assert merged["host_count"] == 2
    assert merged["online_host_count"] == 2
    assert merged["generated_at_sec"] == 105.0
    assert all(t["host_online"] for t in merged["topics"])
    odom_b = [t for t in merged["topics"] if t["host"] == "pc_b"][0]
    assert odom_b["monitored"] is False

    # A newer payload from the same host replaces (not appends) its rows.
    state.set_latest_stats_json(_stats("pc_a", [_topic("/cmd", "pc_a")]))
    assert [(t["name"], t["host"]) for t in state.get_merged_stats()["topics"]] == [
        ("/cmd", "pc_a"), ("/odom", "pc_b")]


def test_cluster_marks_offline_and_forgets_hosts():
    # Intent: silent host -> offline (rows stale, host_online false) -> removed.
    clock = FakeClock()
    state = ClusterState(agent_timeout_sec=5.0, forget_after_sec=60.0, clock=clock)
    state.set_latest_stats_json(_stats("pc_a", [_topic("/a", "pc_a")]))
    state.set_latest_stats_json(_stats("pc_b", [_topic("/b", "pc_b")]))

    clock.advance(4.0)
    state.set_latest_stats_json(_stats("pc_a", [_topic("/a", "pc_a")]))
    clock.advance(2.0)  # pc_b silent for 6s, pc_a for 2s

    merged = state.get_merged_stats()
    by_host = {t["host"]: t for t in merged["topics"]}
    assert by_host["pc_a"]["host_online"] is True
    assert by_host["pc_a"]["stale"] is False
    assert by_host["pc_b"]["host_online"] is False
    assert by_host["pc_b"]["stale"] is True
    assert merged["online_host_count"] == 1

    hosts = {h["host"]: h for h in state.get_hosts()["hosts"]}
    assert hosts["pc_b"]["online"] is False
    assert hosts["pc_b"]["last_seen_age_sec"] == 6.0

    # Keep pc_a alive; pc_b goes past forget_after_sec.
    for _ in range(12):
        clock.advance(5.0)
        state.set_latest_stats_json(_stats("pc_a", [_topic("/a", "pc_a")]))
    assert [h["host"] for h in state.get_hosts()["hosts"]] == ["pc_a"]
    assert [t["host"] for t in state.get_merged_stats()["topics"]] == ["pc_a"]

    # A forgotten host comes back as soon as it reports again.
    state.set_latest_stats_json(_stats("pc_b", [_topic("/b", "pc_b")]))
    assert state.get_hosts()["online_host_count"] == 2


def test_cluster_merges_graph_with_status_priority():
    # Intent: node/edge union; shared topic takes best status ok > stale > down > neutral.
    clock = FakeClock()
    state = ClusterState(agent_timeout_sec=5.0, clock=clock)
    edge_a = {"id": "node://pc_a::/talker->/chatter#pub", "source": "node://pc_a::/talker",
              "target": "/chatter", "qos": "reliable"}
    edge_b = {"id": "/chatter->node://pc_b::/listener#sub", "source": "/chatter",
              "target": "node://pc_b::/listener", "qos": "reliable"}
    state.set_latest_graph_json(_graph("pc_a", [
        _topic_node("/chatter", "ok"), _topic_node("/x", "down"), _topic_node("/y", "stale"),
        _ros_node("pc_a", "/talker")], [edge_a]))
    state.set_latest_graph_json(_graph("pc_b", [
        _topic_node("/chatter", "neutral"), _topic_node("/x", "neutral"), _topic_node("/y", "down"),
        _topic_node("/z", "neutral"), _ros_node("pc_b", "/listener")], [edge_b]))

    graph = state.get_merged_graph()
    nodes = {n["id"]: n for n in graph["nodes"]}
    assert graph["node_count"] == len(graph["nodes"]) == 8
    assert graph["edge_count"] == 2
    assert {e["id"] for e in graph["edges"]} == {edge_a["id"], edge_b["id"]}
    assert nodes["/chatter"]["status"] == "ok"
    assert nodes["/x"]["status"] == "down"
    assert nodes["/y"]["status"] == "stale"
    assert nodes["/z"]["status"] == "neutral"
    assert nodes["host://pc_a"]["status"] == "ok"
    assert nodes["host://pc_b"]["status"] == "ok"
    assert nodes["node://pc_a::/talker"]["host"] == "pc_a"
    assert nodes["node://pc_b::/listener"]["host"] == "pc_b"

    # pc_a goes offline: its host node turns down and it can no longer vouch for "ok".
    clock.advance(4.0)
    state.set_latest_graph_json(_graph("pc_b", [_topic_node("/chatter", "neutral")]))
    clock.advance(2.0)
    nodes = {n["id"]: n for n in state.get_merged_graph()["nodes"]}
    assert nodes["host://pc_a"]["status"] == "down"
    assert nodes["host://pc_b"]["status"] == "ok"
    assert nodes["/chatter"]["status"] == "stale"


def test_cluster_accepts_legacy_payload_without_host():
    # Intent: single-host payloads from older agents are grouped under "local".
    state = ClusterState(clock=FakeClock())
    assert state.set_latest_stats_json('{"topic_count":1,"topics":[{"name":"/foo","alive":true}]}')
    assert state.set_latest_graph_json('{"nodes":[{"id":"/foo","node_type":"topic","status":"ok"}],"edges":[]}')

    merged = state.get_merged_stats()
    assert merged["topics"][0]["host"] == LEGACY_HOST
    assert merged["topics"][0]["monitored"] is True
    assert [h["host"] for h in state.get_hosts()["hosts"]] == [LEGACY_HOST]
    assert state.get_merged_graph()["nodes"][0]["id"] == "/foo"


def test_cluster_rejects_invalid_json():
    # Intent: malformed payloads are dropped, counted, and do not corrupt state.
    state = ClusterState(clock=FakeClock())
    state.set_latest_stats_json(_stats("pc_a", [_topic("/a", "pc_a")]))

    assert state.set_latest_stats_json("{not json") is False
    assert state.set_latest_stats_json("[1, 2, 3]") is False
    assert state.set_latest_graph_json("") is False
    assert state.invalid_payload_count == 3

    merged = state.get_merged_stats()
    assert [t["name"] for t in merged["topics"]] == ["/a"]
    assert state.get_hosts()["host_count"] == 1


def test_http_handler_serves_hosts_api():
    # Intent: /api/hosts lists hosts with online state and topic counts.
    clock = FakeClock()
    state = ClusterState(agent_timeout_sec=5.0, clock=clock)
    state.set_latest_stats_json(_stats("pc_a", [
        _topic("/a", "pc_a"), _topic("/m", "pc_a", monitored=False, locality="mixed")]))
    state.set_latest_graph_json(_graph("pc_a", []))
    clock.advance(10.0)
    state.set_latest_stats_json(_stats("pc_b", [_topic("/b", "pc_b")]))

    with TemporaryDirectory() as tmpdir:
        handler_cls = make_request_handler(state, Path(tmpdir))
        res = _invoke_get(handler_cls, "/api/hosts")
        assert res.status == 200
        assert res.headers["Content-Type"].startswith("application/json")
        body = json.loads(res.wfile.getvalue().decode("utf-8"))

        # Query strings must not break routing (browsers add cache busters).
        assert _invoke_get(handler_cls, "/api/hosts?t=1").status == 200

    assert body["host_count"] == 2
    assert body["online_host_count"] == 1
    hosts = {h["host"]: h for h in body["hosts"]}
    assert hosts["pc_a"]["online"] is False
    assert hosts["pc_a"]["topic_count"] == 2
    assert hosts["pc_a"]["monitored_topic_count"] == 1
    assert hosts["pc_a"]["has_graph"] is True
    assert hosts["pc_a"]["rmw"] == "rmw_fastrtps_cpp"
    assert hosts["pc_b"]["online"] is True
    assert hosts["pc_b"]["has_graph"] is False
