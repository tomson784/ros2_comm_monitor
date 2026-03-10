// Global mutable state used by polling, graph interaction and panel rendering.
const state = {
  stats: { topics: [], topic_count: 0, generated_at_sec: null },
  graph: { nodes: [], edges: [], node_count: 0, edge_count: 0 },
  topicRuntimeByName: new Map(),
  refreshMs: 1000,
  timerId: null,
  lastGraphSignature: "",
  hoveredTopicName: "",
  hoverPoint: null,
  selectedElementType: "",
  selectedElementId: ""
};

const filterInput = document.getElementById("filterInput");
const sortKey = document.getElementById("sortKey");
const sortOrder = document.getElementById("sortOrder");
const refreshMsSelect = document.getElementById("refreshMs");
const unhealthyOnly = document.getElementById("unhealthyOnly");

const topicsBody = document.getElementById("topicsBody");
const generatedAt = document.getElementById("generatedAt");
const topicCount = document.getElementById("topicCount");
const aliveCount = document.getElementById("aliveCount");
const badCount = document.getElementById("badCount");
const totalBandwidth = document.getElementById("totalBandwidth");
const topTopic = document.getElementById("topTopic");
const topHzTopic = document.getElementById("topHzTopic");
const graphCounts = document.getElementById("graphCounts");
const graphRefreshBtn = document.getElementById("graphRefreshBtn");
const graphHoverInfo = document.getElementById("graphHoverInfo");
const graphDetailBody = document.getElementById("graphDetailBody");

// Defensive numeric conversion used for all API payload fields.
function safeNum(v, fallback = 0) {
  return Number.isFinite(Number(v)) ? Number(v) : fallback;
}

function format(v, digits = 2) {
  return safeNum(v).toFixed(digits);
}

function formatAge(v) {
  return v === null || v === undefined ? "inf" : safeNum(v).toFixed(3);
}

function statusBadge(topic) {
  if (!topic.alive) return '<span class="badge badge-bad">DOWN</span>';
  if (topic.stale) return '<span class="badge badge-warn">STALE</span>';
  return '<span class="badge badge-ok">OK</span>';
}

function graphTopicStatusColor(status) {
  if (status === "down") return "#ef4444";
  if (status === "stale") return "#f59e0b";
  return "#22c55e";
}

function escapeHtml(value) {
  return String(value || "")
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#39;");
}

function topicStatusFromTopic(topic) {
  if (!topic || topic.alive === undefined) return "neutral";
  if (!topic.alive) return "down";
  if (topic.stale) return "stale";
  return "ok";
}

function graphStatusBadge(status) {
  if (status === "down") return '<span class="badge badge-bad">DOWN</span>';
  if (status === "stale") return '<span class="badge badge-warn">STALE</span>';
  if (status === "ok") return '<span class="badge badge-ok">OK</span>';
  return '<span class="badge badge-warn">N/A</span>';
}

// Keep topic runtime values in O(1) lookup map for graph hover/details.
function rebuildTopicRuntimeMap() {
  state.topicRuntimeByName = new Map((state.stats.topics || []).map((t) => [t.name, t]));
}

function detailRow(label, value, mono = false) {
  const valClass = mono ? "detail-val mono" : "detail-val";
  return `<div class="detail-row"><span class="detail-key">${escapeHtml(label)}</span><span class="${valClass}">${escapeHtml(value)}</span></div>`;
}

function makeSafeEdgeId(edge, index) {
  const raw = String(edge.id || "").trim();
  if (raw && /^[A-Za-z0-9_.-]+$/.test(raw)) {
    return raw;
  }
  const seed = `${raw}|${String(edge.source || "")}|${String(edge.target || "")}|${String(edge.qos || "unknown")}|${index}`;
  let hash = 2166136261;
  for (let i = 0; i < seed.length; i += 1) {
    hash ^= seed.charCodeAt(i);
    hash += (hash << 1) + (hash << 4) + (hash << 7) + (hash << 8) + (hash << 24);
  }
  const normalized = seed.replace(/[^A-Za-z0-9_.-]/g, "_").slice(0, 48);
  return `e_${normalized}_${Math.abs(hash >>> 0).toString(16)}`;
}

// Reset right panel to default placeholder state.
function clearGraphDetail() {
  state.selectedElementType = "";
  state.selectedElementId = "";
  if (graphDetailBody) {
    graphDetailBody.textContent = "Click a node or link to view details.";
  }
}

const QOS_FIELDS = [
  { key: "qos_reliability", label: "Reliability" },
  { key: "qos_durability", label: "Durability" },
  { key: "qos_history", label: "History" },
  { key: "qos_depth", label: "Depth" },
  { key: "qos_liveliness", label: "Liveliness" },
  { key: "qos_deadline_sec", label: "Deadline (s)" },
  { key: "qos_lifespan_sec", label: "Lifespan (s)" },
  { key: "qos_liveliness_lease_duration_sec", label: "Lease Duration (s)" },
  { key: "qos_avoid_ros_namespace_conventions", label: "Avoid ROS NS Conventions" }
];

// Collapse duplicate values so one QoS field can represent multiple links.
function qosSummary(values) {
  const set = Array.from(new Set(values.filter(Boolean)));
  if (!set.length) return "-";
  return set.join(", ");
}

function normalizeQosValue(value) {
  if (typeof value === "boolean") return value ? "true" : "false";
  if (value === null || value === undefined || value === "") return "-";
  return String(value);
}

function extractQosProfile(edgeData) {
  const profile = {};
  QOS_FIELDS.forEach((field) => {
    profile[field.key] = normalizeQosValue(edgeData[field.key]);
  });
  return profile;
}

// Merge profiles when one clicked edge logically represents many peers.
function mergeQosProfiles(edges) {
  const merged = {};
  QOS_FIELDS.forEach((field) => {
    merged[field.key] = qosSummary(
      edges.map((e) => normalizeQosValue(e.data(field.key)))
    );
  });
  return merged;
}

// Derive sender/receiver role and counterpart sets from edge direction.
function findSenderReceiverProfilesFromEdge(edge) {
  const d = edge.data();
  const source = String(d.source || "");
  const target = String(d.target || "");
  const sourceType = String(cy.getElementById(source).data("node_type") || "");
  const targetType = String(cy.getElementById(target).data("node_type") || "");

  if (sourceType === "topic" && targetType === "node") {
    const senderEdges = cy.edges().filter((e) => e.data("target") === source);
    const senderNodes = Array.from(new Set(senderEdges.map((e) => String(e.data("source") || "-"))));
    return {
      topic: source,
      senderNodes,
      receiverNodes: [target],
      sender: mergeQosProfiles(senderEdges),
      receiver: extractQosProfile(d)
    };
  }

  if (sourceType === "node" && targetType === "topic") {
    const receiverEdges = cy.edges().filter((e) => e.data("source") === target);
    const receiverNodes = Array.from(new Set(receiverEdges.map((e) => String(e.data("target") || "-"))));
    return {
      topic: target,
      senderNodes: [source],
      receiverNodes,
      sender: extractQosProfile(d),
      receiver: mergeQosProfiles(receiverEdges)
    };
  }

  return {
    topic: "-",
    senderNodes: [source],
    receiverNodes: [target],
    sender: extractQosProfile(d),
    receiver: {}
  };
}

// Build one column in sender/receiver QoS comparison view.
function renderQosColumn(title, profile) {
  const rows = QOS_FIELDS.map((field) => `
    <div class="qos-row">
      <span class="qos-key">${escapeHtml(field.label)}</span>
      <span class="qos-val">${escapeHtml(profile[field.key] || "-")}</span>
    </div>
  `).join("");
  return `
    <section class="qos-col">
      <div class="qos-col-head">${escapeHtml(title)}</div>
      <div class="qos-col-body">${rows}</div>
    </section>
  `;
}

// Main renderer for right-side detail panel.
// - Node click: regular metadata + topic runtime values.
// - Edge click: edge metadata (1 column) + QoS comparison (2 columns).
function renderSelectedElementDetail() {
  if (!graphDetailBody || !state.selectedElementId || !state.selectedElementType) return;

  if (state.selectedElementType === "node") {
    const node = cy.getElementById(state.selectedElementId);
    if (!node || node.empty()) {
      clearGraphDetail();
      return;
    }

    const d = node.data();
    const rows = [
      detailRow("Kind", "Node"),
      detailRow("Node ID", d.id || "-", true),
      detailRow("Label", d.label || "-", true),
      detailRow("Type", d.node_type || "-"),
      detailRow("Status", d.status || "-"),
      detailRow("Incoming", String(node.indegree(false))),
      detailRow("Outgoing", String(node.outdegree(false))),
      detailRow("Connected", String(node.connectedEdges().length))
    ];

    if (d.node_type === "topic") {
      const topic = state.topicRuntimeByName.get(d.id);
      if (topic) {
        rows.push(detailRow("Hz", `${format(topic.hz)} Hz`));
        rows.push(detailRow("Bandwidth", `${format(topic.bandwidth_mib_per_sec)} MiB/s`));
        rows.push(detailRow("Message Size", `${format(topic.latest_message_size_mib)} MiB`));
        rows.push(detailRow("Publishers", String(safeNum(topic.publisher_count, 0))));
        rows.push(detailRow("Subscribers", String(safeNum(topic.subscriber_count, 0))));
        rows.push(detailRow("Age", `${formatAge(topic.age_sec)} s`));
      }
    }

    graphDetailBody.innerHTML = rows.join("");
    return;
  }

  if (state.selectedElementType === "edge") {
    const edge = cy.getElementById(state.selectedElementId);
    if (!edge || edge.empty()) {
      clearGraphDetail();
      return;
    }

    const d = edge.data();
    const pair = findSenderReceiverProfilesFromEdge(edge);
    const senderPath = pair.senderNodes && pair.senderNodes.length ? pair.senderNodes.join(", ") : "-";
    const receiverPath = pair.receiverNodes && pair.receiverNodes.length ? pair.receiverNodes.join(", ") : "-";
    const route = `${senderPath} -> ${pair.topic || "-"} -> ${receiverPath}`;

    graphDetailBody.innerHTML = `
      <div class="edge-meta">
        ${detailRow("Kind", "Link")}
        ${detailRow("Edge ID", d.raw_id || d.id || "-", true)}
        ${detailRow("Source", d.source || "-", true)}
        ${detailRow("Target", d.target || "-", true)}
        ${detailRow("Route", route, true)}
      </div>
      <div class="qos-two-col">
        ${renderQosColumn("Sender", pair.sender)}
        ${renderQosColumn("Receiver", pair.receiver)}
      </div>
    `;
  }
}

// Tab switching is intentionally explicit to avoid hidden implicit state.
document.querySelectorAll(".tab").forEach((btn) => {
  btn.addEventListener("click", async () => {
    document.querySelectorAll(".tab").forEach((t) => t.classList.remove("active"));
    document.querySelectorAll(".panel").forEach((p) => p.classList.remove("active"));
    btn.classList.add("active");
    document.getElementById(btn.dataset.tab).classList.add("active");

    if (btn.dataset.tab === "graph") {
      await fetchGraphAndUpdate();
      setTimeout(() => {
        cy.resize();
        cy.fit(undefined, 36);
      }, 100);
    } else {
      hideGraphHoverInfo();
      cy.elements().unselect();
      clearGraphDetail();
    }
  });
});

const MAX_POINTS = 60;
const CHART_PALETTE = [
  "#60a5fa", "#34d399", "#f59e0b", "#f472b6", "#a78bfa",
  "#22d3ee", "#fb7185", "#84cc16", "#f97316", "#c084fc",
  "#38bdf8", "#2dd4bf"
];

function colorFromTopic(topicName) {
  let hash = 0;
  for (let i = 0; i < topicName.length; i += 1) {
    hash = ((hash << 5) - hash) + topicName.charCodeAt(i);
    hash |= 0;
  }
  return CHART_PALETTE[Math.abs(hash) % CHART_PALETTE.length];
}

const hzChart = new Chart(document.getElementById("hzChart"), {
  type: "line",
  data: { labels: [], datasets: [] },
  options: {
    animation: false,
    responsive: true,
    maintainAspectRatio: false,
    normalized: true,
    interaction: { mode: "nearest", intersect: false },
    plugins: {
      legend: {
        display: true,
        labels: { color: "#e5e7eb", boxWidth: 12, usePointStyle: true }
      }
    },
    scales: {
      x: { ticks: { color: "#94a3b8" }, grid: { color: "rgba(148,163,184,0.12)" } },
      y: { beginAtZero: true, ticks: { color: "#94a3b8" }, grid: { color: "rgba(148,163,184,0.12)" } }
    }
  }
});

const bwChart = new Chart(document.getElementById("bwChart"), {
  type: "line",
  data: { labels: [], datasets: [] },
  options: {
    animation: false,
    responsive: true,
    maintainAspectRatio: false,
    normalized: true,
    interaction: { mode: "nearest", intersect: false },
    plugins: {
      legend: {
        display: true,
        labels: { color: "#e5e7eb", boxWidth: 12, usePointStyle: true }
      }
    },
    scales: {
      x: { ticks: { color: "#94a3b8" }, grid: { color: "rgba(148,163,184,0.12)" } },
      y: { beginAtZero: true, ticks: { color: "#94a3b8" }, grid: { color: "rgba(148,163,184,0.12)" } }
    }
  }
});

function ensureDataset(chart, topicName) {
  let ds = chart.data.datasets.find((d) => d.label === topicName);
  if (!ds) {
    const color = colorFromTopic(topicName);
    ds = {
      label: topicName,
      data: [],
      tension: 0.22,
      borderWidth: 2,
      pointRadius: 0,
      pointHoverRadius: 2,
      borderColor: color,
      backgroundColor: color,
      spanGaps: true
    };
    chart.data.datasets.push(ds);
  }
  return ds;
}

function pruneDatasets(chart, topicNames) {
  chart.data.datasets = chart.data.datasets.filter((d) => topicNames.has(d.label));
}

function getFilteredTopics() {
  const filter = filterInput.value.trim().toLowerCase();
  const onlyUnhealthy = unhealthyOnly.checked;
  const key = sortKey.value;
  const desc = sortOrder.value === "desc";

  return [...(state.stats.topics || [])]
    .filter((t) => {
      if (onlyUnhealthy && t.alive && !t.stale) return false;
      if (!filter) return true;
      return (
        String(t.name || "").toLowerCase().includes(filter) ||
        String(t.type || "").toLowerCase().includes(filter)
      );
    })
    .sort((a, b) => {
      const av = a[key];
      const bv = b[key];
      let cmp;
      if (typeof av === "string" || typeof bv === "string") {
        cmp = String(av || "").localeCompare(String(bv || ""));
      } else {
        cmp = safeNum(av) - safeNum(bv);
      }
      return desc ? -cmp : cmp;
    });
}

function updateSummary() {
  const topics = state.stats.topics || [];
  const alive = topics.filter((t) => t.alive).length;
  const bad = topics.filter((t) => !t.alive || t.stale).length;
  const bw = topics.reduce((sum, t) => sum + safeNum(t.bandwidth_mib_per_sec), 0);

  const topBw = [...topics].sort(
    (a, b) => safeNum(b.bandwidth_mib_per_sec) - safeNum(a.bandwidth_mib_per_sec)
  )[0];
  const topHz = [...topics].sort((a, b) => safeNum(b.hz) - safeNum(a.hz))[0];

  topicCount.textContent = String(state.stats.topic_count || topics.length || 0);
  aliveCount.textContent = String(alive);
  badCount.textContent = String(bad);
  totalBandwidth.textContent = `${format(bw)} MiB/s`;
  topTopic.textContent = topBw ? `${topBw.name} (${format(topBw.bandwidth_mib_per_sec)} MiB/s)` : "-";
  topHzTopic.textContent = topHz ? `${topHz.name} (${format(topHz.hz)} Hz)` : "-";
  generatedAt.textContent = state.stats.generated_at_sec
    ? new Date(Number(state.stats.generated_at_sec) * 1000).toLocaleString()
    : "-";
}

function updateTable() {
  const topics = getFilteredTopics();

  if (!topics.length) {
    topicsBody.innerHTML = '<tr><td colspan="10" class="empty-cell">No topics matched.</td></tr>';
    return;
  }

  topicsBody.innerHTML = topics.map((t) => `
    <tr>
      <td>${statusBadge(t)}</td>
      <td class="mono">${t.name || "-"}</td>
      <td class="mono">${t.type || "-"}</td>
      <td>${format(t.hz)}</td>
      <td>${format(t.bandwidth_mib_per_sec)}</td>
      <td>${format(t.latest_message_size_mib)}</td>
      <td>${safeNum(t.publisher_count, 0)}</td>
      <td>${safeNum(t.subscriber_count, 0)}</td>
      <td>${safeNum(t.message_count, 0)}</td>
      <td>${formatAge(t.age_sec)}</td>
    </tr>
  `).join("");
}

function updateCharts() {
  const topics = state.stats.topics || [];
  const nowLabel = new Date().toLocaleTimeString();
  const topicNameSet = new Set(topics.map((t) => t.name));

  pruneDatasets(hzChart, topicNameSet);
  pruneDatasets(bwChart, topicNameSet);

  hzChart.data.labels.push(nowLabel);
  bwChart.data.labels.push(nowLabel);

  topics.forEach((topic) => {
    ensureDataset(hzChart, topic.name).data.push(safeNum(topic.hz));
    ensureDataset(bwChart, topic.name).data.push(safeNum(topic.bandwidth_mib_per_sec));
  });

  hzChart.data.datasets.forEach((ds) => {
    if (ds.data.length < hzChart.data.labels.length) ds.data.push(null);
  });
  bwChart.data.datasets.forEach((ds) => {
    if (ds.data.length < bwChart.data.labels.length) ds.data.push(null);
  });

  while (hzChart.data.labels.length > MAX_POINTS) {
    hzChart.data.labels.shift();
    hzChart.data.datasets.forEach((ds) => ds.data.shift());
  }
  while (bwChart.data.labels.length > MAX_POINTS) {
    bwChart.data.labels.shift();
    bwChart.data.datasets.forEach((ds) => ds.data.shift());
  }

  hzChart.update("none");
  bwChart.update("none");
}

// Graph style keeps Reliability label visible on edge itself,
// while full QoS is shown only in the side detail panel.
const cy = cytoscape({
  container: document.getElementById("graphCanvas"),
  elements: [],
  style: [
    {
      selector: 'node[node_type="host"]',
      style: {
        label: "data(label)",
        "background-opacity": 0.08,
        "background-color": "#1e293b",
        "border-color": "#64748b",
        "border-width": 2,
        color: "#cbd5e1",
        "font-size": 16,
        "font-weight": 700,
        "text-valign": "top",
        "text-halign": "center",
        shape: "round-rectangle",
        padding: "28px"
      }
    },
    {
      selector: 'node[node_type="namespace"]',
      style: {
        label: "data(label)",
        "background-opacity": 0.08,
        "background-color": "#0f172a",
        "border-color": "#334155",
        "border-width": 1.5,
        color: "#94a3b8",
        "font-size": 12,
        "font-weight": 600,
        "text-valign": "top",
        "text-halign": "center",
        shape: "round-rectangle",
        padding: "18px"
      }
    },
    {
      selector: 'node[node_type="node"]',
      style: {
        label: "data(label)",
        "background-color": "#2563eb",
        color: "#e5e7eb",
        "text-wrap": "wrap",
        "text-max-width": 220,
        "font-size": 11,
        "text-valign": "center",
        "text-halign": "center",
        width: 78,
        height: 78,
        "border-color": "#334155",
        "border-width": 2,
        shape: "ellipse"
      }
    },
    {
      selector: 'node[node_type="topic"]',
      style: {
        label: "data(label)",
        "background-color": "data(color)",
        color: "#0b1220",
        "font-size": 11,
        "font-weight": 700,
        "text-wrap": "wrap",
        "text-max-width": 220,
        "text-valign": "center",
        "text-halign": "center",
        width: "label",
        height: 42,
        padding: "12px",
        shape: "round-rectangle",
        "border-color": "#475569",
        "border-width": 2
      }
    },
    {
      selector: "node:selected",
      style: {
        "border-color": "#f8fafc",
        "border-width": 4
      }
    },
    {
      selector: "edge",
      style: {
        width: 2,
        "curve-style": "bezier",
        "line-color": "#93c5fd",
        "target-arrow-shape": "triangle",
        "target-arrow-color": "#93c5fd",
        "arrow-scale": 0.9,
        label: "data(qos)",
        color: "#cbd5e1",
        "font-size": 10,
        "text-background-opacity": 1,
        "text-background-color": "#0f172a",
        "text-background-padding": 2
      }
    },
    {
      selector: 'edge[qos="reliable"]',
      style: {
        width: 3,
        "curve-style": "bezier",
        "line-color": "#22c55e",
        "target-arrow-shape": "triangle",
        "target-arrow-color": "#22c55e",
        label: "data(qos)",
        color: "#86efac",
        "font-size": 10,
        "text-background-opacity": 1,
        "text-background-color": "#0f172a",
        "text-background-padding": 2
      }
    },
    {
      selector: 'edge[qos="best_effort"]',
      style: {
        width: 2,
        "curve-style": "bezier",
        "line-style": "dashed",
        "line-color": "#f59e0b",
        "target-arrow-shape": "triangle",
        "target-arrow-color": "#f59e0b",
        label: "data(qos)",
        color: "#fcd34d",
        "font-size": 10,
        "text-background-opacity": 1,
        "text-background-color": "#0f172a",
        "text-background-padding": 2
      }
    },
    {
      selector: 'edge[qos="unknown"]',
      style: {
        width: 2,
        "curve-style": "bezier",
        "line-style": "dotted",
        "line-color": "#94a3b8",
        "target-arrow-shape": "triangle",
        "target-arrow-color": "#94a3b8",
        label: "data(qos)",
        color: "#cbd5e1",
        "font-size": 10,
        "text-background-opacity": 1,
        "text-background-color": "#0f172a",
        "text-background-padding": 2
      }
    }
  ],
  layout: { name: "grid", animate: false, padding: 20 }
});

cy.on("mouseover", 'node[node_type="topic"]', (evt) => {
  state.hoveredTopicName = evt.target.id();
  if (evt.originalEvent) {
    state.hoverPoint = {
      x: evt.originalEvent.clientX,
      y: evt.originalEvent.clientY
    };
  }
  renderGraphHoverInfo();
});

cy.on("mousemove", 'node[node_type="topic"]', (evt) => {
  if (!state.hoveredTopicName || !evt.originalEvent) return;
  state.hoverPoint = {
    x: evt.originalEvent.clientX,
    y: evt.originalEvent.clientY
  };
  updateGraphHoverInfoPosition();
});

cy.on("mouseout", 'node[node_type="topic"]', () => {
  hideGraphHoverInfo();
});

cy.on("tap", "node", (evt) => {
  state.selectedElementType = "node";
  state.selectedElementId = evt.target.id();
  evt.target.select();
  renderSelectedElementDetail();
});

cy.on("tap", "edge", (evt) => {
  state.selectedElementType = "edge";
  state.selectedElementId = evt.target.id();
  evt.target.select();
  renderSelectedElementDetail();
});

cy.on("tap", (evt) => {
  if (evt.target !== cy) return;
  cy.elements().unselect();
  clearGraphDetail();
});

function updateGraphStats() {
  const drawnNodeCount = cy.nodes().length;
  const drawnEdgeCount = cy.edges().length;
  if (graphCounts) {
    graphCounts.textContent =
      `${safeNum(drawnNodeCount, state.graph.node_count || state.graph.nodes.length)} nodes / ` +
      `${safeNum(drawnEdgeCount, state.graph.edge_count || state.graph.edges.length)} edges`;
  }
}

function hideGraphHoverInfo() {
  state.hoveredTopicName = "";
  state.hoverPoint = null;
  if (graphHoverInfo) {
    graphHoverInfo.style.display = "none";
  }
}

function updateGraphHoverInfoPosition() {
  if (!graphHoverInfo || !state.hoverPoint) return;
  graphHoverInfo.style.left = `${state.hoverPoint.x + 14}px`;
  graphHoverInfo.style.top = `${state.hoverPoint.y + 14}px`;
}

function renderGraphHoverInfo() {
  if (!graphHoverInfo || !state.hoveredTopicName) return;

  const topicName = state.hoveredTopicName;
  const topic = state.topicRuntimeByName.get(topicName);
  const status = topicStatusFromTopic(topic);
  const hz = topic ? format(topic.hz) : "-";
  const age = topic ? formatAge(topic.age_sec) : "-";

  graphHoverInfo.innerHTML = `
    <div class="hover-title mono">${escapeHtml(topicName)}</div>
    <div class="hover-line"><span>Status</span>${graphStatusBadge(status)}</div>
    <div class="hover-line"><span>Hz</span><span>${hz}</span></div>
    <div class="hover-line"><span>Age</span><span>${age}</span></div>
  `;
  updateGraphHoverInfoPosition();
  graphHoverInfo.style.display = "block";
}

// Keep topic color/status synchronized with the latest stats payload
// without rebuilding the graph structure.
function refreshGraphRuntimeVisuals() {
  cy.nodes('node[node_type="topic"]').forEach((node) => {
    const topic = state.topicRuntimeByName.get(node.id());
    if (!topic) return;
    const status = topicStatusFromTopic(topic);
    node.data("status", status);
    node.data("color", graphTopicStatusColor(status));
  });

  if (state.hoveredTopicName) {
    renderGraphHoverInfo();
  }
  if (state.selectedElementId) {
    renderSelectedElementDetail();
  }
}

// Graph refresh strategy:
// - Rebuild Cytoscape elements only when node/edge structure changed.
// - Otherwise keep layout and update dynamic visual data only.
function updateGraph() {
  const nodes = (state.graph.nodes || []).map((n) => ({
    ...n,
    id: String(n.id || "").trim(),
    parent_id: n.parent_id ? String(n.parent_id).trim() : ""
  })).filter((n) => n.id);
  const edges = (state.graph.edges || []).map((e, index) => ({
    ...e,
    raw_id: String(e.id || "").trim(),
    id: makeSafeEdgeId(e, index),
    source: String(e.source || "").trim(),
    target: String(e.target || "").trim(),
    qos: String(e.qos || "unknown").trim().toLowerCase()
  })).filter((e) => e.id && e.source && e.target);

  const graphSignature = JSON.stringify({
    n: nodes.map((n) => n.id).sort(),
    e: edges.map((e) => e.id).sort()
  });
  const isStructureChanged = graphSignature !== state.lastGraphSignature;

  if (isStructureChanged) {
    const nodeIds = new Set(nodes.map((n) => n.id));
    const elements = [];

    nodes.forEach((n) => {
      const topicRuntime = n.node_type === "topic" ? state.topicRuntimeByName.get(n.id) : null;
      const status = n.node_type === "topic"
        ? (topicRuntime ? topicStatusFromTopic(topicRuntime) : (n.status || "neutral"))
        : n.status || "neutral";

      elements.push({
        data: {
          id: n.id,
          label: n.label,
          node_type: n.node_type,
          parent: n.parent_id || undefined,
          status,
          color: n.node_type === "topic" ? graphTopicStatusColor(status) : "#2563eb"
        }
      });
    });

    edges.forEach((e) => {
      if (!nodeIds.has(e.source) || !nodeIds.has(e.target)) return;
      elements.push({
        data: {
          id: e.id,
          raw_id: e.raw_id,
          source: e.source,
          target: e.target,
          qos: e.qos,
          qos_reliability: e.qos_reliability,
          qos_durability: e.qos_durability,
          qos_history: e.qos_history,
          qos_depth: e.qos_depth,
          qos_liveliness: e.qos_liveliness,
          qos_deadline_sec: e.qos_deadline_sec,
          qos_lifespan_sec: e.qos_lifespan_sec,
          qos_liveliness_lease_duration_sec: e.qos_liveliness_lease_duration_sec,
          qos_avoid_ros_namespace_conventions: e.qos_avoid_ros_namespace_conventions
        }
      });
    });

    cy.elements().remove();
    cy.add(elements);
    cy.style().update();
  }

  if (state.selectedElementId) {
    const selected = cy.getElementById(state.selectedElementId);
    if (selected && !selected.empty()) {
      selected.select();
      renderSelectedElementDetail();
    } else {
      clearGraphDetail();
    }
  }

  if (isStructureChanged) {
    cy.layout({ name: "cose", animate: false, padding: 36 }).run();
    state.lastGraphSignature = graphSignature;
  } else {
    // Keep current layout and only refresh runtime styling/data.
    cy.fit(undefined, 36);
  }

  refreshGraphRuntimeVisuals();
  updateGraphStats();
}

// Periodic polling for stats/table/charts.
async function fetchStatsAndUpdate() {
  try {
    const statsRes = await fetch("/api/stats", { cache: "no-store" });
    if (!statsRes.ok) throw new Error(`stats HTTP ${statsRes.status}`);

    const data = await statsRes.json();

    state.stats = {
      generated_at_sec: data.generated_at_sec ?? null,
      topic_count: data.topic_count ?? (data.topics || []).length,
      topics: (data.topics || []).map((t) => ({
        ...t,
        bandwidth_mib_per_sec: safeNum(t.bandwidth_mib_per_sec),
        latest_message_size_mib: safeNum(t.latest_message_size_mib),
        hz: safeNum(t.hz)
      }))
    };
    rebuildTopicRuntimeMap();

    updateSummary();
    updateTable();
    updateCharts();
    refreshGraphRuntimeVisuals();
    updateGraphStats();
  } catch (err) {
    topicsBody.innerHTML =
      `<tr><td colspan="10" class="empty-cell">Failed to fetch stats: ${err}</td></tr>`;
  }
}

// Manual refresh for graph topology payload.
async function fetchGraphAndUpdate() {
  try {
    const graphRes = await fetch("/api/graph", { cache: "no-store" });
    if (!graphRes.ok) throw new Error(`graph HTTP ${graphRes.status}`);
    const graph = await graphRes.json();

    const nextNodes = graph.nodes || [];
    const nextEdges = graph.edges || [];
    const hasCurrentGraph = (state.graph.nodes || []).length > 0 || (state.graph.edges || []).length > 0;
    const statsTopicCount = safeNum(state.stats.topic_count, (state.stats.topics || []).length);
    const isUnexpectedEmpty = nextNodes.length === 0 && nextEdges.length === 0 && hasCurrentGraph && statsTopicCount > 0;
    if (isUnexpectedEmpty) {
      if (graphCounts) {
        graphCounts.textContent = "Graph payload is empty (previous graph retained).";
      }
      return;
    }

    state.graph = {
      node_count: graph.node_count ?? nextNodes.length,
      edge_count: graph.edge_count ?? nextEdges.length,
      nodes: nextNodes,
      edges: nextEdges
    };
    updateGraph();
  } catch (err) {
    if (graphCounts) {
      graphCounts.textContent = `Failed to fetch graph: ${err}`;
    }
  }
}

function restartPolling() {
  if (state.timerId) clearInterval(state.timerId);
  state.refreshMs = Number(refreshMsSelect.value);
  state.timerId = setInterval(fetchStatsAndUpdate, state.refreshMs);
}

[filterInput, sortKey, sortOrder, unhealthyOnly].forEach((el) => {
  el.addEventListener("input", updateTable);
  el.addEventListener("change", updateTable);
});

refreshMsSelect.addEventListener("change", () => {
  restartPolling();
  fetchStatsAndUpdate();
});

if (graphRefreshBtn) {
  graphRefreshBtn.addEventListener("click", () => {
    fetchGraphAndUpdate();
  });
}

document.getElementById("graphCanvas").addEventListener("mouseleave", () => {
  hideGraphHoverInfo();
});

restartPolling();
fetchStatsAndUpdate();
