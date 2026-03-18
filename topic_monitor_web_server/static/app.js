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
const graphZoomInBtn = document.getElementById("graphZoomInBtn");
const graphZoomOutBtn = document.getElementById("graphZoomOutBtn");
const graphZoomResetBtn = document.getElementById("graphZoomResetBtn");
const graphCanvas = document.getElementById("graphCanvas");
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

const graphRenderer = {
  elk: typeof ELK === "function" ? new ELK() : null,
  layoutRequestId: 0,
  model: {
    nodesById: new Map(),
    edgesById: new Map(),
    outgoingByNodeId: new Map(),
    incomingByNodeId: new Map()
  },
  view: {
    scale: 1,
    minScale: 0.4,
    maxScale: 2.5,
    baseWidth: 0,
    baseHeight: 0,
    isDragging: false,
    dragStartX: 0,
    dragStartY: 0,
    scrollLeftStart: 0,
    scrollTopStart: 0
  }
};

function getGraphNode(nodeId) {
  return graphRenderer.model.nodesById.get(String(nodeId || ""));
}

function getGraphEdge(edgeId) {
  return graphRenderer.model.edgesById.get(String(edgeId || ""));
}

function getGraphOutgoingEdges(nodeId) {
  return graphRenderer.model.outgoingByNodeId.get(String(nodeId || "")) || [];
}

function getGraphIncomingEdges(nodeId) {
  return graphRenderer.model.incomingByNodeId.get(String(nodeId || "")) || [];
}

function updateGraphZoomLabel() {
  if (graphZoomResetBtn) {
    graphZoomResetBtn.textContent = `${Math.round(graphRenderer.view.scale * 100)}%`;
  }
}

function clampGraphScale(scale) {
  return Math.min(graphRenderer.view.maxScale, Math.max(graphRenderer.view.minScale, scale));
}

function applyGraphZoom(nextScale, focusClientX = null, focusClientY = null) {
  const svg = graphCanvas?.querySelector(".graph-svg");
  if (!graphCanvas || !svg || !graphRenderer.view.baseWidth || !graphRenderer.view.baseHeight) return;

  const prevScale = graphRenderer.view.scale;
  const clampedScale = clampGraphScale(nextScale);
  if (Math.abs(clampedScale - prevScale) < 0.001) {
    updateGraphZoomLabel();
    return;
  }

  const rect = graphCanvas.getBoundingClientRect();
  const localX = focusClientX === null ? rect.width / 2 : focusClientX - rect.left;
  const localY = focusClientY === null ? rect.height / 2 : focusClientY - rect.top;
  const contentX = (graphCanvas.scrollLeft + localX) / prevScale;
  const contentY = (graphCanvas.scrollTop + localY) / prevScale;

  graphRenderer.view.scale = clampedScale;
  svg.setAttribute("width", `${Math.round(graphRenderer.view.baseWidth * clampedScale)}`);
  svg.setAttribute("height", `${Math.round(graphRenderer.view.baseHeight * clampedScale)}`);

  graphCanvas.scrollLeft = Math.max(0, contentX * clampedScale - localX);
  graphCanvas.scrollTop = Math.max(0, contentY * clampedScale - localY);
  updateGraphZoomLabel();
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
      edges.map((e) => normalizeQosValue(
        typeof e?.data === "function" ? e.data(field.key) : e?.[field.key]
      ))
    );
  });
  return merged;
}

// Derive sender/receiver role and counterpart sets from edge direction.
function findSenderReceiverProfilesFromEdge(edge) {
  const d = typeof edge?.data === "function" ? edge.data() : edge;
  const source = String(d.source || "");
  const target = String(d.target || "");
  const sourceType = String(getGraphNode(source)?.node_type || "");
  const targetType = String(getGraphNode(target)?.node_type || "");

  if (sourceType === "topic" && targetType === "node") {
    const senderEdges = getGraphIncomingEdges(source);
    const senderNodes = Array.from(new Set(senderEdges.map((e) => String(e.source || "-"))));
    return {
      topic: source,
      senderNodes,
      receiverNodes: [target],
      sender: mergeQosProfiles(senderEdges),
      receiver: extractQosProfile(d)
    };
  }

  if (sourceType === "node" && targetType === "topic") {
    const receiverEdges = getGraphOutgoingEdges(target);
    const receiverNodes = Array.from(new Set(receiverEdges.map((e) => String(e.target || "-"))));
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
    const d = getGraphNode(state.selectedElementId);
    if (!d) {
      clearGraphDetail();
      return;
    }

    const rows = [
      detailRow("Kind", "Node"),
      detailRow("Node ID", d.id || "-", true),
      detailRow("Label", d.label || "-", true),
      detailRow("Type", d.node_type || "-"),
      detailRow("Status", d.status || "-"),
      detailRow("Incoming", String(getGraphIncomingEdges(d.id).length)),
      detailRow("Outgoing", String(getGraphOutgoingEdges(d.id).length)),
      detailRow(
        "Connected",
        String(getGraphIncomingEdges(d.id).length + getGraphOutgoingEdges(d.id).length)
      )
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
    const edge = getGraphEdge(state.selectedElementId);
    if (!edge) {
      clearGraphDetail();
      return;
    }

    const d = edge;
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
    } else {
      hideGraphHoverInfo();
      clearGraphDetail();
      updateGraphSelectionClasses();
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

function graphStrokeColor(qos) {
  if (qos === "reliable") return "#22c55e";
  if (qos === "best_effort") return "#f59e0b";
  return "#93c5fd";
}

function graphTextColor(qos) {
  if (qos === "reliable") return "#86efac";
  if (qos === "best_effort") return "#fcd34d";
  return "#cbd5e1";
}

function graphDashArray(qos) {
  if (qos === "best_effort") return "7 6";
  if (qos === "unknown") return "2 6";
  return "";
}

function measureNodeSize(node) {
  const label = String(node.label || node.id || "");
  if (node.node_type === "node") return { width: 92, height: 92 };
  if (node.node_type === "topic") {
    return {
      width: Math.max(140, Math.min(320, 72 + label.length * 7)),
      height: 48
    };
  }
  if (node.node_type === "namespace") return { width: 180, height: 72 };
  if (node.node_type === "host") return { width: 220, height: 92 };
  return { width: 120, height: 64 };
}

function buildGraphModel(nodes, edges) {
  const nodesById = new Map();
  const edgesById = new Map();
  const outgoingByNodeId = new Map();
  const incomingByNodeId = new Map();

  nodes.forEach((node) => {
    nodesById.set(node.id, { ...node });
    outgoingByNodeId.set(node.id, []);
    incomingByNodeId.set(node.id, []);
  });

  edges.forEach((edge) => {
    edgesById.set(edge.id, { ...edge });
    if (!outgoingByNodeId.has(edge.source)) outgoingByNodeId.set(edge.source, []);
    if (!incomingByNodeId.has(edge.target)) incomingByNodeId.set(edge.target, []);
    outgoingByNodeId.get(edge.source).push(edgesById.get(edge.id));
    incomingByNodeId.get(edge.target).push(edgesById.get(edge.id));
  });

  graphRenderer.model = { nodesById, edgesById, outgoingByNodeId, incomingByNodeId };
}

function wrapLabel(label, maxCharsPerLine = 22, maxLines = 3) {
  const text = String(label || "");
  if (!text) return [""];
  const chunks = [];
  let cursor = 0;
  while (cursor < text.length && chunks.length < maxLines) {
    chunks.push(text.slice(cursor, cursor + maxCharsPerLine));
    cursor += maxCharsPerLine;
  }
  if (cursor < text.length && chunks.length) {
    chunks[chunks.length - 1] = `${chunks[chunks.length - 1].slice(0, Math.max(0, maxCharsPerLine - 1))}…`;
  }
  return chunks;
}

function svgEl(name, attrs = {}) {
  const el = document.createElementNS("http://www.w3.org/2000/svg", name);
  Object.entries(attrs).forEach(([key, value]) => {
    if (value !== undefined && value !== null) el.setAttribute(key, String(value));
  });
  return el;
}

function flattenElkNodes(node, offsetX = 0, offsetY = 0, acc = []) {
  const currentX = offsetX + safeNum(node.x, 0);
  const currentY = offsetY + safeNum(node.y, 0);
  (node.children || []).forEach((child) => {
    acc.push({
      id: child.id,
      x: currentX + safeNum(child.x, 0),
      y: currentY + safeNum(child.y, 0),
      width: safeNum(child.width, 0),
      height: safeNum(child.height, 0)
    });
    flattenElkNodes(child, currentX, currentY, acc);
  });
  return acc;
}

function collectElkEdges(node, acc = []) {
  (node.edges || []).forEach((edge) => acc.push(edge));
  (node.children || []).forEach((child) => collectElkEdges(child, acc));
  return acc;
}

function edgePointsFromSection(section) {
  const points = [];
  if (section.startPoint) points.push(section.startPoint);
  (section.bendPoints || []).forEach((point) => points.push(point));
  if (section.endPoint) points.push(section.endPoint);
  return points.map((point) => ({ x: safeNum(point.x), y: safeNum(point.y) }));
}

function splinePathFromPoints(points) {
  if (!points.length) return "";
  if (points.length === 1) return `M ${points[0].x} ${points[0].y}`;
  if (points.length === 2) return `M ${points[0].x} ${points[0].y} L ${points[1].x} ${points[1].y}`;

  let d = `M ${points[0].x} ${points[0].y}`;
  for (let i = 0; i < points.length - 1; i += 1) {
    const p0 = points[Math.max(0, i - 1)];
    const p1 = points[i];
    const p2 = points[i + 1];
    const p3 = points[Math.min(points.length - 1, i + 2)];
    const cp1x = p1.x + (p2.x - p0.x) / 6;
    const cp1y = p1.y + (p2.y - p0.y) / 6;
    const cp2x = p2.x - (p3.x - p1.x) / 6;
    const cp2y = p2.y - (p3.y - p1.y) / 6;
    d += ` C ${cp1x} ${cp1y}, ${cp2x} ${cp2y}, ${p2.x} ${p2.y}`;
  }
  return d;
}

function ensureGraphDefs(svg) {
  const defs = svgEl("defs");
  const colors = [
    { id: "graph-arrow-default", color: "#93c5fd" },
    { id: "graph-arrow-reliable", color: "#22c55e" },
    { id: "graph-arrow-best-effort", color: "#f59e0b" },
    { id: "graph-arrow-unknown", color: "#94a3b8" }
  ];
  colors.forEach((entry) => {
    const marker = svgEl("marker", {
      id: entry.id,
      viewBox: "0 0 10 10",
      refX: "9",
      refY: "5",
      markerWidth: "7",
      markerHeight: "7",
      orient: "auto-start-reverse"
    });
    marker.appendChild(svgEl("path", { d: "M 0 0 L 10 5 L 0 10 z", fill: entry.color }));
    defs.appendChild(marker);
  });
  svg.appendChild(defs);
}

function markerIdForQos(qos) {
  if (qos === "reliable") return "graph-arrow-reliable";
  if (qos === "best_effort") return "graph-arrow-best-effort";
  if (qos === "unknown") return "graph-arrow-unknown";
  return "graph-arrow-default";
}

function renderGraphNode(layer, node) {
  const g = svgEl("g", {
    class: `graph-node graph-node-${node.node_type}${state.selectedElementType === "node" && state.selectedElementId === node.id ? " is-selected" : ""}`,
    transform: `translate(${node.x},${node.y})`,
    "data-node-id": node.id,
    "data-node-type": node.node_type
  });

  if (node.node_type === "host" || node.node_type === "namespace") {
    g.appendChild(svgEl("rect", {
      class: "graph-group-box",
      rx: 18,
      ry: 18,
      width: node.width,
      height: node.height
    }));
    g.appendChild(svgEl("text", {
      class: "graph-group-label",
      x: node.width / 2,
      y: 24,
      "text-anchor": "middle"
    })).textContent = String(node.label || node.id);
  } else if (node.node_type === "node") {
    g.appendChild(svgEl("ellipse", {
      class: "graph-leaf-shape",
      cx: node.width / 2,
      cy: node.height / 2,
      rx: node.width / 2,
      ry: node.height / 2
    }));
  } else {
    g.appendChild(svgEl("rect", {
      class: "graph-leaf-shape",
      rx: 12,
      ry: 12,
      width: node.width,
      height: node.height,
      fill: graphTopicStatusColor(node.status || "neutral")
    }));
  }

  if (node.node_type === "node" || node.node_type === "topic") {
    const lines = wrapLabel(node.label || node.id, node.node_type === "topic" ? 26 : 18, node.node_type === "topic" ? 2 : 3);
    const text = svgEl("text", {
      class: `graph-node-label graph-node-label-${node.node_type}`,
      x: node.width / 2,
      y: node.height / 2 - ((lines.length - 1) * 7),
      "text-anchor": "middle"
    });
    lines.forEach((line, index) => {
      const tspan = svgEl("tspan", { x: node.width / 2, dy: index === 0 ? 0 : 14 });
      tspan.textContent = line;
      text.appendChild(tspan);
    });
    g.appendChild(text);
  }

  g.appendChild(svgEl("title")).textContent = String(node.label || node.id);
  g.addEventListener("click", (evt) => {
    evt.stopPropagation();
    state.selectedElementType = "node";
    state.selectedElementId = node.id;
    updateGraphSelectionClasses();
    renderSelectedElementDetail();
  });

  if (node.node_type === "topic") {
    g.addEventListener("mouseenter", (evt) => {
      state.hoveredTopicName = node.id;
      state.hoverPoint = { x: evt.clientX, y: evt.clientY };
      renderGraphHoverInfo();
    });
    g.addEventListener("mousemove", (evt) => {
      if (!state.hoveredTopicName) return;
      state.hoverPoint = { x: evt.clientX, y: evt.clientY };
      updateGraphHoverInfoPosition();
    });
    g.addEventListener("mouseleave", () => {
      hideGraphHoverInfo();
    });
  }

  layer.appendChild(g);
}

function isGraphGroupNode(node) {
  return node.node_type === "host" || node.node_type === "namespace";
}

function renderGraphEdge(layer, edge, pathData) {
  const qos = String(edge.qos || "unknown");
  const edgeGroup = svgEl("g", {
    class: `graph-edge graph-edge-${qos}${state.selectedElementType === "edge" && state.selectedElementId === edge.id ? " is-selected" : ""}`,
    "data-edge-id": edge.id
  });
  const visiblePath = svgEl("path", {
    class: "graph-edge-path",
    d: pathData,
    stroke: graphStrokeColor(qos),
    "stroke-dasharray": graphDashArray(qos),
    "marker-end": `url(#${markerIdForQos(qos)})`
  });
  const hitPath = svgEl("path", {
    class: "graph-edge-hit",
    d: pathData
  });
  edgeGroup.appendChild(visiblePath);
  edgeGroup.appendChild(hitPath);
  const clickHandler = (evt) => {
    evt.stopPropagation();
    state.selectedElementType = "edge";
    state.selectedElementId = edge.id;
    updateGraphSelectionClasses();
    renderSelectedElementDetail();
  };
  visiblePath.addEventListener("click", clickHandler);
  hitPath.addEventListener("click", clickHandler);
  layer.appendChild(edgeGroup);

  if (edge.qos) {
    const pathLength = visiblePath.getTotalLength();
    const midpoint = visiblePath.getPointAtLength(pathLength / 2);
    const label = svgEl("text", {
      class: "graph-edge-label",
      x: midpoint.x,
      y: midpoint.y - 6,
      "text-anchor": "middle",
      fill: graphTextColor(qos)
    });
    label.textContent = edge.qos;
    layer.appendChild(label);
  }
}

function updateGraphSelectionClasses() {
  if (!graphCanvas) return;
  graphCanvas.querySelectorAll("[data-node-id]").forEach((el) => {
    el.classList.toggle(
      "is-selected",
      state.selectedElementType === "node" && state.selectedElementId === el.getAttribute("data-node-id")
    );
  });
  graphCanvas.querySelectorAll("[data-edge-id]").forEach((el) => {
    el.classList.toggle(
      "is-selected",
      state.selectedElementType === "edge" && state.selectedElementId === el.getAttribute("data-edge-id")
    );
  });
}

function updateGraphStats() {
  if (graphCounts) {
    graphCounts.textContent =
      `${safeNum(graphRenderer.model.nodesById.size, state.graph.node_count || state.graph.nodes.length)} nodes / ` +
      `${safeNum(graphRenderer.model.edgesById.size, state.graph.edge_count || state.graph.edges.length)} edges`;
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
  graphRenderer.model.nodesById.forEach((node, nodeId) => {
    if (node.node_type !== "topic") return;
    const topic = state.topicRuntimeByName.get(nodeId);
    if (!topic) return;
    node.status = topicStatusFromTopic(topic);
    const shape = graphCanvas.querySelector(`[data-node-id="${CSS.escape(nodeId)}"] .graph-leaf-shape`);
    if (shape) {
      shape.setAttribute("fill", graphTopicStatusColor(node.status));
    }
  });

  if (state.hoveredTopicName) {
    renderGraphHoverInfo();
  }
  if (state.selectedElementId) {
    renderSelectedElementDetail();
  }
}

function normalizeGraphElements() {
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

  return { nodes, edges };
}

function buildElkLayoutGraph(nodes, edges) {
  const nodeMap = new Map(nodes.map((node) => [node.id, node]));
  const childMap = new Map();
  nodes.forEach((node) => {
    const parentId = node.parent_id && nodeMap.has(node.parent_id) ? node.parent_id : "__root__";
    if (!childMap.has(parentId)) childMap.set(parentId, []);
    childMap.get(parentId).push(node);
  });

  const makeElkNode = (node) => {
    const size = measureNodeSize(node);
    const children = (childMap.get(node.id) || []).map(makeElkNode);
    return {
      id: node.id,
      width: size.width,
      height: size.height,
      layoutOptions: children.length
        ? {
            "elk.padding": "[top=28,left=20,bottom=20,right=20]",
            "elk.spacing.nodeNode": "30"
          }
        : undefined,
      children
    };
  };

  return {
    id: "root",
    layoutOptions: {
      "elk.algorithm": "layered",
      "elk.direction": "RIGHT",
      "elk.edgeRouting": "SPLINES",
      "elk.layered.nodePlacement.strategy": "NETWORK_SIMPLEX",
      "elk.layered.crossingMinimization.strategy": "LAYER_SWEEP",
      "elk.layered.considerModelOrder.strategy": "NODES_AND_EDGES",
      "elk.hierarchyHandling": "INCLUDE_CHILDREN",
      "elk.spacing.nodeNode": "36",
      "elk.layered.spacing.nodeNodeBetweenLayers": "80",
      "elk.spacing.edgeNode": "28",
      "elk.padding": "[top=28,left=28,bottom=28,right=28]"
    },
    children: (childMap.get("__root__") || []).map(makeElkNode),
    edges: edges.map((edge) => ({
      id: edge.id,
      sources: [edge.source],
      targets: [edge.target]
    }))
  };
}

async function renderElkGraph(nodes, edges) {
  if (!graphRenderer.elk) {
    if (graphCounts) graphCounts.textContent = "ELK.js failed to load.";
    return;
  }

  const requestId = ++graphRenderer.layoutRequestId;
  const elkGraph = buildElkLayoutGraph(nodes, edges);
  const layout = await graphRenderer.elk.layout(elkGraph);
  if (requestId !== graphRenderer.layoutRequestId) return;

  const layoutNodes = flattenElkNodes(layout)
    .map((entry) => {
      const meta = getGraphNode(entry.id);
      return meta ? { ...meta, ...entry } : null;
    })
    .filter(Boolean);
  const layoutEdges = collectElkEdges(layout);

  graphCanvas.innerHTML = "";
  const width = Math.max(960, Math.ceil(safeNum(layout.width, 0) + 80));
  const height = Math.max(720, Math.ceil(safeNum(layout.height, 0) + 80));
  graphRenderer.view.baseWidth = width;
  graphRenderer.view.baseHeight = height;
  const svg = svgEl("svg", {
    class: "graph-svg",
    viewBox: `0 0 ${width} ${height}`,
    width,
    height
  });
  ensureGraphDefs(svg);

  const groupsLayer = svgEl("g", { class: "graph-layer graph-layer-groups" });
  const edgesLayer = svgEl("g", { class: "graph-layer graph-layer-edges" });
  const nodesLayer = svgEl("g", { class: "graph-layer graph-layer-nodes" });
  svg.appendChild(groupsLayer);
  svg.appendChild(edgesLayer);
  svg.appendChild(nodesLayer);

  layoutEdges.forEach((elkEdge) => {
    const edgeMeta = getGraphEdge(elkEdge.id);
    const section = elkEdge.sections && elkEdge.sections[0];
    if (!edgeMeta || !section) return;
    const points = edgePointsFromSection(section);
    if (points.length < 2) return;
    renderGraphEdge(edgesLayer, edgeMeta, splinePathFromPoints(points));
  });

  layoutNodes
    .filter((node) => isGraphGroupNode(node))
    .sort((a, b) => {
      const rankA = a.node_type === "host" ? 0 : 1;
      const rankB = b.node_type === "host" ? 0 : 1;
      return rankA - rankB;
    })
    .forEach((node) => renderGraphNode(groupsLayer, node));

  layoutNodes
    .filter((node) => !isGraphGroupNode(node))
    .forEach((node) => renderGraphNode(nodesLayer, node));

  svg.addEventListener("click", (evt) => {
    if (evt.target !== svg) return;
    clearGraphDetail();
    updateGraphSelectionClasses();
  });

  graphCanvas.appendChild(svg);
  applyGraphZoom(graphRenderer.view.scale);
  refreshGraphRuntimeVisuals();
  updateGraphSelectionClasses();
}

// Graph refresh strategy:
// - Rebuild SVG only when node/edge structure changed.
// - Otherwise keep the routed geometry and update runtime colors/details.
async function updateGraph() {
  const { nodes, edges } = normalizeGraphElements();
  buildGraphModel(nodes, edges);

  const graphSignature = JSON.stringify({
    n: nodes.map((n) => n.id).sort(),
    e: edges.map((e) => e.id).sort()
  });
  const isStructureChanged = graphSignature !== state.lastGraphSignature;

  if (isStructureChanged) {
    nodes.forEach((node) => {
      const topicRuntime = node.node_type === "topic" ? state.topicRuntimeByName.get(node.id) : null;
      graphRenderer.model.nodesById.set(node.id, {
        ...node,
        status: node.node_type === "topic"
          ? (topicRuntime ? topicStatusFromTopic(topicRuntime) : (node.status || "neutral"))
          : (node.status || "neutral")
      });
    });
    await renderElkGraph(nodes, edges);
    state.lastGraphSignature = graphSignature;
  }

  if (state.selectedElementId) {
    const selectedExists = state.selectedElementType === "node"
      ? Boolean(getGraphNode(state.selectedElementId))
      : Boolean(getGraphEdge(state.selectedElementId));
    if (!selectedExists) {
      clearGraphDetail();
    } else {
      renderSelectedElementDetail();
    }
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
    await updateGraph();
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

if (graphZoomInBtn) {
  graphZoomInBtn.addEventListener("click", () => {
    applyGraphZoom(graphRenderer.view.scale * 1.2);
  });
}

if (graphZoomOutBtn) {
  graphZoomOutBtn.addEventListener("click", () => {
    applyGraphZoom(graphRenderer.view.scale / 1.2);
  });
}

if (graphZoomResetBtn) {
  graphZoomResetBtn.addEventListener("click", () => {
    applyGraphZoom(1);
  });
}

if (graphCanvas) {
  graphCanvas.addEventListener("wheel", (evt) => {
    evt.preventDefault();
    const factor = evt.deltaY < 0 ? 1.12 : 1 / 1.12;
    applyGraphZoom(graphRenderer.view.scale * factor, evt.clientX, evt.clientY);
  }, { passive: false });

  graphCanvas.addEventListener("mousedown", (evt) => {
    if (evt.button !== 0) return;
    const target = evt.target;
    if (target instanceof Element && target.closest("[data-node-id], [data-edge-id]")) return;
    graphRenderer.view.isDragging = true;
    graphRenderer.view.dragStartX = evt.clientX;
    graphRenderer.view.dragStartY = evt.clientY;
    graphRenderer.view.scrollLeftStart = graphCanvas.scrollLeft;
    graphRenderer.view.scrollTopStart = graphCanvas.scrollTop;
    graphCanvas.classList.add("is-dragging");
    evt.preventDefault();
  });
}

document.addEventListener("mousemove", (evt) => {
  if (!graphRenderer.view.isDragging || !graphCanvas) return;
  const dx = evt.clientX - graphRenderer.view.dragStartX;
  const dy = evt.clientY - graphRenderer.view.dragStartY;
  graphCanvas.scrollLeft = graphRenderer.view.scrollLeftStart - dx;
  graphCanvas.scrollTop = graphRenderer.view.scrollTopStart - dy;
});

document.addEventListener("mouseup", () => {
  if (!graphRenderer.view.isDragging || !graphCanvas) return;
  graphRenderer.view.isDragging = false;
  graphCanvas.classList.remove("is-dragging");
});

document.getElementById("graphCanvas").addEventListener("mouseleave", () => {
  hideGraphHoverInfo();
});

updateGraphZoomLabel();
restartPolling();
fetchStatsAndUpdate();
