"""Assertions for the docker multi-host test (stdlib only)."""
import json
import sys
from pathlib import Path

OUT = Path(sys.argv[1])
failures = []


def check(cond, message):
    print(("  PASS  " if cond else "  FAIL  ") + message)
    if not cond:
        failures.append(message)


def load(name):
    return json.loads((OUT / name).read_text())


def rows(stats):
    return {(t["host"], t["name"]): t for t in stats["topics"]}


def node_parent(graph, label):
    return sorted(n["parent_id"] for n in graph["nodes"]
                  if n["node_type"] == "node" and n["label"] == label)


rx = dict(line.split() for line in (OUT / "rx.txt").read_text().splitlines())
rx = {k: int(v) for k, v in rx.items()}
KIB, MIB = 1024, 1024 * 1024

print("[agents]")
print((OUT / "agents.txt").read_text().rstrip())

print("[phase 1: auto]")
stats, graph, hosts = load("auto_stats.json"), load("auto_graph.json"), load("auto_hosts.json")
h = {x["host"]: x for x in hosts["hosts"]}
check(sorted(h) == ["host-a", "host-b", "host-c"], f"3 hosts reported: {sorted(h)}")
check(all(x["online"] for x in h.values()), "all hosts online")
check(h.get("host-c", {}).get("rmw") == "rmw_fastrtps_cpp", "Humble agent (host-c) reports")
r = rows(stats)
big = r.get(("host-b", "/big"), {})
check(big.get("locality") == "local" and big.get("monitored") is True,
      "/big measured on host-b as local")
check(8.0 <= big.get("hz", 0) <= 12.0, f"/big hz ~10 (got {big.get('hz')})")
check(abs(big.get("bandwidth_bytes_per_sec", 0) - 2048000) < 0.1 * 2048000,
      f"/big bandwidth ~2 MB/s (got {big.get('bandwidth_bytes_per_sec')})")
check(("host-a", "/big") not in r, "/big NOT measured on host-a")
check(r.get(("host-a", "/a_chatter"), {}).get("monitored") is True, "/a_chatter measured on host-a")
check(r.get(("host-c", "/c_chatter"), {}).get("monitored") is True, "/c_chatter measured on host-c (Humble)")
check(r.get(("host-b", "/b_chatter"), {}).get("monitored") is True, "/b_chatter measured on host-b")
check(sorted(n for (_, n) in r) == ["/a_chatter", "/b_chatter", "/big", "/c_chatter"],
      "each topic reported exactly once")
# Humble <-> Jazzy: remote node names are unknown (_NODE_NAME_UNKNOWN_); such endpoints
# must still be counted, and only the monitor's own participants excluded.
check(r.get(("host-a", "/a_chatter"), {}).get("subscriber_count") == 1,
      "/a_chatter counts the Humble subscriber on host-c (unknown node name) "
      f"(got {r.get(('host-a', '/a_chatter'), {}).get('subscriber_count')})")
check(node_parent(graph, "/big_publisher") == ["ns://host-b::/"], "graph: /big_publisher under host-b")
host_nodes = {n["id"]: n["status"] for n in graph["nodes"] if n["node_type"] == "host"}
check(host_nodes == {"host://host-a": "ok", "host://host-b": "ok", "host://host-c": "ok"},
      f"graph: host nodes ok {host_nodes}")
# Cross-host edges: /b_chatter is subscribed on host-a, /a_chatter on host-c.
edges = {e["id"] for e in graph["edges"]}
check(any(e.startswith("/b_chatter->node://host-a::") for e in edges), "graph: host-a subscribes /b_chatter")
check(any(e.startswith("/a_chatter->node://host-c::") for e in edges), "graph: host-c subscribes /a_chatter")
check(rx["auto"] < 100 * KIB, f"auto: master rx {rx['auto'] / KIB:.1f} KiB/s < 100 KiB/s")

print("[phase 2: all]")
stats, hosts = load("all_stats.json"), load("all_hosts.json")
h = {x["host"]: x for x in hosts["hosts"]}
check(h.get("host-a", {}).get("locality_mode") == "all", "master agent runs locality_mode=all")
check(rows(stats).get(("host-a", "/big"), {}).get("monitored") is True, "/big now also measured on host-a")
c_row = rows(stats).get(("host-c", "/c_chatter"), {})
check(c_row.get("subscriber_count") == 0,
      "host-c excludes the master agent's /c_chatter subscription (unknown name, tool participant) "
      f"(got {c_row.get('subscriber_count')})")
check(rx["all"] > 1.5 * MIB, f"all: master rx {rx['all'] / MIB:.2f} MiB/s > 1.5 MiB/s")
check(rx["all"] > 50 * rx["auto"], f"all/auto ratio {rx['all'] / max(rx['auto'], 1):.0f}x > 50x")

print("[phase 3: host_b stopped]")
stats, graph, hosts = load("offline_stats.json"), load("offline_graph.json"), load("offline_hosts.json")
h = {x["host"]: x for x in hosts["hosts"]}
check(h.get("host-b", {}).get("online") is False,
      f"host-b offline (last_seen_age_sec={h.get('host-b', {}).get('last_seen_age_sec')})")
check(h.get("host-a", {}).get("online") and h.get("host-c", {}).get("online"), "host-a/host-c still online")
b_rows = [t for t in stats["topics"] if t["host"] == "host-b"]
check(b_rows and all(t["stale"] and t["host_online"] is False for t in b_rows),
      "host-b rows: stale=true, host_online=false")
host_nodes = {n["id"]: n["status"] for n in graph["nodes"] if n["node_type"] == "host"}
check(host_nodes.get("host://host-b") == "down", "graph: host-b node down")

print(f"\nrx: auto={rx['auto']} B/s ({rx['auto'] / KIB:.1f} KiB/s), "
      f"all={rx['all']} B/s ({rx['all'] / MIB:.2f} MiB/s)")
print("RESULT:", "PASS" if not failures else f"FAIL ({len(failures)} checks)")
sys.exit(1 if failures else 0)
