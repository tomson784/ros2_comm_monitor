import json
from io import BytesIO
from pathlib import Path
from tempfile import TemporaryDirectory

from topic_monitor_web_server.web_server import SharedState, make_request_handler


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
