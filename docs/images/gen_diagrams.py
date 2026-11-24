"""Generate the hand-laid-out SVG diagrams used by the docs.

    python3 docs/images/gen_diagrams.py docs/images

Outputs (each in English and Japanese, *.ja.svg):
  architecture.svg  - overall architecture (agents per PC, master aggregation)
  agent_scan.svg    - what an agent does in one scan
  host_state.svg    - host online / offline / removed state transitions
"""
import sys
from pathlib import Path

OUT = Path(sys.argv[1])

T = {
    "ja": {
        "pc_a": "PC A（master）", "pc_b": "PC B", "pc_c": "PC C",
        "user": "ユーザーノード",
        "pub_a": "/a_chatter を publish", "pub_b": "/big を publish", "pub_c": "/c_chatter を publish",
        "payload": "データ本体", "payload2": "（PC 内だけ）",
        "agent_note": "自 PC のトピックだけ計測",
        "web": "web server（集約）", "web2": "PC ごとの JSON を保持・統合",
        "json": "stats / graph JSON（数 KiB/s）",
        "browser": "ブラウザ",
        "effect": "効果（Docker 実測）",
        "eff1": "PC B が 200 KiB × 10 Hz を publish したとき、",
        "eff2": "マスターが受信する量",
        "eff3": "auto（分散監視）", "eff3v": "約 14 KiB/s",
        "eff4": "all（全トピック購読）", "eff4v": "約 2.06 MiB/s",
        "lg1": "データ本体（PC 内だけ）", "lg2": "統計 JSON（数 KiB/s）", "lg3": "PC 間には流さない",
        "lg4": "HTTP（UI / API）",
    },
    "en": {
        "pc_a": "PC A (master)", "pc_b": "PC B", "pc_c": "PC C",
        "user": "User nodes",
        "pub_a": "publish /a_chatter", "pub_b": "publish /big", "pub_c": "publish /c_chatter",
        "payload": "payloads", "payload2": "(inside the PC)",
        "agent_note": "this PC's topics only",
        "web": "web server (aggregator)", "web2": "keeps and merges per-PC JSON",
        "json": "stats / graph JSON (a few KiB/s)",
        "browser": "Browser",
        "effect": "Effect (measured with Docker)",
        "eff1": "PC B publishes 200 KiB x 10 Hz;",
        "eff2": "traffic received by the master:",
        "eff3": "auto (distributed)", "eff3v": "~14 KiB/s",
        "eff4": "all (subscribe to all)", "eff4v": "~2.06 MiB/s",
        "lg1": "payloads (inside the PC)", "lg2": "statistics JSON (a few KiB/s)", "lg3": "never crosses PCs",
        "lg4": "HTTP (UI / API)",
    },
}

FONT = "'Noto Sans JP','Noto Sans CJK JP','Hiragino Sans','Yu Gothic','Meiryo',Inter,'Segoe UI',sans-serif"
MONO = "ui-monospace,'SFMono-Regular',Menlo,Consolas,monospace"

BLUE, ORANGE, SLATE, RED = "#2563eb", "#ea580c", "#334155", "#dc2626"


def svg(lang):
    t = T[lang]
    W, H = 1200, 660
    o = []
    a = o.append
    a(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
      f'font-family="{FONT}" role="img" aria-label="ros2_comm_monitor architecture">')
    a(f'''<defs>
  <marker id="ah-blue" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="5" markerHeight="5" orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="{BLUE}"/></marker>
  <marker id="ah-orange" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="{ORANGE}"/></marker>
  <marker id="ah-slate" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="{SLATE}"/></marker>
  <filter id="sh" x="-5%" y="-5%" width="110%" height="120%"><feDropShadow dx="0" dy="2" stdDeviation="3" flood-color="#0f172a" flood-opacity=".12"/></filter>
</defs>''')
    # background card (keeps contrast in both light and dark GitHub themes)
    a(f'<rect x="0" y="0" width="{W}" height="{H}" rx="18" fill="#f8fafc"/>')

    def pc(x, y, w, h, label, master=False):
        fill = "#eef2ff" if master else "#ffffff"
        stroke = "#6366f1" if master else "#94a3b8"
        a(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="16" fill="{fill}" stroke="{stroke}" stroke-width="2" filter="url(#sh)"/>')
        # monitor icon + label
        a(f'<g transform="translate({x + 18},{y + 16})" fill="none" stroke="{SLATE}" stroke-width="2">'
          f'<rect x="0" y="0" width="24" height="16" rx="2"/><path d="M8 21h8M12 16v5"/></g>')
        a(f'<text x="{x + 52}" y="{y + 32}" font-size="18" font-weight="700" fill="#0f172a">{label}</text>')

    def box(x, y, w, h, title, sub, fill, stroke, mono_sub=False, title_fill="#0f172a"):
        a(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="12" fill="{fill}" stroke="{stroke}" stroke-width="2"/>')
        a(f'<text x="{x + w / 2}" y="{y + h / 2 - 4}" text-anchor="middle" font-size="16" font-weight="700" fill="{title_fill}">{title}</text>')
        fam = f' font-family="{MONO}"' if mono_sub else ""
        a(f'<text x="{x + w / 2}" y="{y + h / 2 + 17}" text-anchor="middle" font-size="12.5" fill="#475569"{fam}>{sub}</text>')

    cols = [(30, t["pc_a"], t["pub_a"], "host_a", True), (430, t["pc_b"], t["pub_b"], "host_b", False),
            (830, t["pc_c"], t["pub_c"], "host_c", False)]
    for x, label, pub, host, master in cols:
        h = 520 if master else 300
        pc(x, 30, 340, h, label, master)
        cx = x + 170
        box(x + 60, 66, 220, 62, t["user"], pub, "#ffffff", "#94a3b8")
        # payload arrow (thick blue, stays inside the PC)
        a(f'<line x1="{cx}" y1="130" x2="{cx}" y2="196" stroke="{BLUE}" stroke-width="7" marker-end="url(#ah-blue)"/>')
        a(f'<text x="{cx + 14}" y="160" font-size="13" font-weight="700" fill="{BLUE}">{t["payload"]}</text>')
        a(f'<text x="{cx + 14}" y="177" font-size="12" fill="{BLUE}">{t["payload2"]}</text>')
        box(x + 50, 202, 240, 66, "agent", f"comm_monitor_agent_{host}", "#dbeafe", BLUE, mono_sub=True)
        # note sits right of the dashed JSON line that leaves the agent at cx
        a(f'<text x="{cx + 12}" y="292" font-size="12.5" fill="#475569">{t["agent_note"]}</text>')

    # "payloads never cross PCs" markers in the gaps between PC boxes
    for gx in (400, 800):
        a(f'<line x1="{gx - 26}" y1="97" x2="{gx + 26}" y2="97" stroke="#94a3b8" stroke-width="7" stroke-dasharray="6 5"/>')
        a(f'<circle cx="{gx}" cy="97" r="15" fill="#ffffff" stroke="{RED}" stroke-width="3"/>')
        a(f'<path d="M{gx - 7},{97 - 7} L{gx + 7},{97 + 7} M{gx + 7},{97 - 7} L{gx - 7},{97 + 7}" stroke="{RED}" stroke-width="3.5" stroke-linecap="round"/>')

    # web server inside the master PC
    box(60, 430, 280, 80, t["web"], t["web2"], "#ffedd5", ORANGE)

    # JSON paths (dashed orange): agent A straight down; agents B and C via a bus
    dash = 'stroke-dasharray="9 6" stroke-linecap="round" fill="none"'
    a(f'<path d="M200,268 V424" stroke="{ORANGE}" stroke-width="3.5" {dash} marker-end="url(#ah-orange)"/>')
    a(f'<path d="M1000,268 V380 H600" stroke="{ORANGE}" stroke-width="3.5" {dash}/>')
    a(f'<path d="M600,268 V380 H290 V424" stroke="{ORANGE}" stroke-width="3.5" {dash} marker-end="url(#ah-orange)"/>')
    a(f'<rect x="{800 - 150}" y="356" width="300" height="24" rx="6" fill="#f8fafc"/>')
    a(f'<text x="800" y="373" text-anchor="middle" font-size="14" font-weight="700" fill="{ORANGE}">{t["json"]}</text>')

    # browser (outside, bottom middle) + HTTP arrow
    bx, by = 470, 438
    a(f'<rect x="{bx}" y="{by}" width="230" height="86" rx="12" fill="#ffffff" stroke="{SLATE}" stroke-width="2" filter="url(#sh)"/>')
    a(f'<rect x="{bx}" y="{by}" width="230" height="22" rx="12" fill="#e2e8f0"/>'
      f'<rect x="{bx}" y="{by + 12}" width="230" height="10" fill="#e2e8f0"/>')
    for i, c in enumerate(("#f87171", "#fbbf24", "#34d399")):
        a(f'<circle cx="{bx + 16 + i * 14}" cy="{by + 11}" r="4.5" fill="{c}"/>')
    a(f'<text x="{bx + 115}" y="{by + 50}" text-anchor="middle" font-size="16" font-weight="700" fill="#0f172a">{t["browser"]}</text>')
    a(f'<text x="{bx + 115}" y="{by + 71}" text-anchor="middle" font-size="12.5" fill="#475569" font-family="{MONO}">http://&lt;master&gt;:8080</text>')
    a(f'<line x1="342" y1="481" x2="{bx - 6}" y2="481" stroke="{SLATE}" stroke-width="3" marker-end="url(#ah-slate)"/>')
    a(f'<text x="{bx + 115}" y="{by + 110}" text-anchor="middle" font-size="12.5" fill="#475569" font-family="{MONO}">/api/stats · /api/graph · /api/hosts</text>')

    # effect callout
    ex, ey = 760, 420
    a(f'<rect x="{ex}" y="{ey}" width="410" height="130" rx="14" fill="#ffffff" stroke="#cbd5e1" stroke-width="1.5" filter="url(#sh)"/>')
    a(f'<text x="{ex + 20}" y="{ey + 28}" font-size="15" font-weight="700" fill="#0f172a">{t["effect"]}</text>')
    a(f'<text x="{ex + 20}" y="{ey + 50}" font-size="12.5" fill="#475569">{t["eff1"]}</text>')
    a(f'<text x="{ex + 20}" y="{ey + 67}" font-size="12.5" fill="#475569">{t["eff2"]}</text>')
    a(f'<text x="{ex + 20}" y="{ey + 94}" font-size="14" fill="#0f172a">{t["eff3"]}</text>')
    a(f'<text x="{ex + 390}" y="{ey + 94}" text-anchor="end" font-size="16" font-weight="700" fill="#16a34a">{t["eff3v"]}</text>')
    a(f'<text x="{ex + 20}" y="{ey + 118}" font-size="14" fill="#0f172a">{t["eff4"]}</text>')
    a(f'<text x="{ex + 390}" y="{ey + 118}" text-anchor="end" font-size="16" font-weight="700" fill="{RED}">{t["eff4v"]}</text>')

    # legend
    ly = 610
    a(f'<line x1="40" y1="{ly}" x2="90" y2="{ly}" stroke="{BLUE}" stroke-width="7"/>')
    a(f'<text x="100" y="{ly + 5}" font-size="14" fill="#0f172a">{t["lg1"]}</text>')
    a(f'<line x1="330" y1="{ly}" x2="380" y2="{ly}" stroke="{ORANGE}" stroke-width="3.5" stroke-dasharray="9 6"/>')
    a(f'<text x="390" y="{ly + 5}" font-size="14" fill="#0f172a">{t["lg2"]}</text>')
    a(f'<line x1="660" y1="{ly}" x2="710" y2="{ly}" stroke="{SLATE}" stroke-width="3"/>')
    a(f'<text x="720" y="{ly + 5}" font-size="14" fill="#0f172a">{t["lg4"]}</text>')
    a(f'<circle cx="930" cy="{ly}" r="11" fill="#ffffff" stroke="{RED}" stroke-width="2.5"/>')
    a(f'<path d="M925,{ly - 5} L935,{ly + 5} M935,{ly - 5} L925,{ly + 5}" stroke="{RED}" stroke-width="3" stroke-linecap="round"/>')
    a(f'<text x="950" y="{ly + 5}" font-size="14" fill="#0f172a">{t["lg3"]}</text>')
    a('</svg>')
    return "\n".join(o) + "\n"



# ---------------------------------------------------------------- agent scan

SCAN_T = {
    "ja": {
        "s1": ("トピック一覧を取得", "get_topic_names_and_types()"),
        "s2": ("フィルタ", "allowlist / denylist / hidden / internal"),
        "s3": ("endpoint 情報を取得", "publisher / subscriber ごと"),
        "q1": "監視ツール自身？",
        "q1n": "agent / web server",
        "q2": "自 PC の endpoint？",
        "q2n": "判定順は 3.1 を参照",
        "s4": ("トピックを分類", "local / mixed / orphan / remote"),
        "s5": ("subscribe を作成・削除", "自 PC のトピックだけ購読"),
        "s6": ("キャッシュに保存", "stats / graph JSON の生成に再利用"),
        "yes": "はい", "no": "いいえ",
        "skip": ("件数とグラフから除外", ""),
        "every": "scan_period_ms ごと（既定 1 秒）",
        "json": "report_period_ms ごとに stats / graph JSON を publish",
    },
    "en": {
        "s1": ("List topics", "get_topic_names_and_types()"),
        "s2": ("Filter", "allowlist / denylist / hidden / internal"),
        "s3": ("Get endpoint info", "per publisher / subscriber"),
        "q1": "Monitor itself?",
        "q1n": "agent / web server",
        "q2": "On this PC?",
        "q2n": "see 3.1 for the order",
        "s4": ("Classify topic", "local / mixed / orphan / remote"),
        "s5": ("Create / drop subscriptions", "subscribe to this PC's topics only"),
        "s6": ("Cache the result", "reused for stats / graph JSON"),
        "yes": "yes", "no": "no",
        "skip": ("Excluded from counts and graph", ""),
        "every": "every scan_period_ms (default 1 s)",
        "json": "stats / graph JSON published every report_period_ms",
    },
}


def scan_svg(lang):
    t = SCAN_T[lang]
    W, H = 1200, 430
    o = []
    a = o.append
    a(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
      f'font-family="{FONT}" role="img" aria-label="agent scan flow">')
    a(f'''<defs><marker id="ah" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="{SLATE}"/></marker></defs>''')
    a(f'<rect width="{W}" height="{H}" rx="18" fill="#f8fafc"/>')
    a(f'<text x="30" y="36" font-size="14" font-weight="700" fill="#475569">⟳ {t["every"]}</text>')

    def step(x, y, w, h, title, sub, fill="#ffffff", stroke="#94a3b8"):
        a(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="12" fill="{fill}" stroke="{stroke}" stroke-width="2"/>')
        a(f'<text x="{x + w / 2}" y="{y + h / 2 - (4 if sub else -5)}" text-anchor="middle" font-size="15" font-weight="700" fill="#0f172a">{title}</text>')
        if sub:
            fam = f' font-family="{MONO}"' if "(" in sub or "/" in sub else ""
            a(f'<text x="{x + w / 2}" y="{y + h / 2 + 16}" text-anchor="middle" font-size="12" fill="#475569"{fam}>{sub}</text>')

    def diamond(cx, cy, w, h, title, note):
        a(f'<path d="M{cx},{cy - h / 2} L{cx + w / 2},{cy} L{cx},{cy + h / 2} L{cx - w / 2},{cy} z" fill="#fef9c3" stroke="#ca8a04" stroke-width="2"/>')
        a(f'<text x="{cx}" y="{cy - 2}" text-anchor="middle" font-size="14" font-weight="700" fill="#0f172a">{title}</text>')
        a(f'<text x="{cx}" y="{cy + 16}" text-anchor="middle" font-size="11.5" fill="#475569">{note}</text>')

    def arrow(x1, y1, x2, y2, label=None, lx=None, ly=None):
        a(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{SLATE}" stroke-width="2.5" marker-end="url(#ah)"/>')
        if label:
            a(f'<text x="{lx}" y="{ly}" font-size="13" font-weight="700" fill="{SLATE}">{label}</text>')

    # row 1: list -> filter -> endpoints -> monitor itself?
    y1, h = 60, 70
    step(30, y1, 230, h, *t["s1"])
    arrow(262, y1 + h / 2, 296, y1 + h / 2)
    step(300, y1, 270, h, *t["s2"])
    arrow(572, y1 + h / 2, 606, y1 + h / 2)
    step(610, y1, 230, h, *t["s3"])
    arrow(842, y1 + h / 2, 886, y1 + h / 2)
    diamond(1010, y1 + h / 2, 240, 110, t["q1"], t["q1n"])
    # yes -> excluded (down, right column)
    arrow(1010, y1 + h / 2 + 55, 1010, 218, t["yes"], 1020, 196)
    step(880, 220, 260, 56, t["skip"][0], "", fill="#f1f5f9", stroke="#cbd5e1")
    # no -> on this PC? (row 2, leftwards)
    y2 = 310
    # no -> leaves the right vertex, runs down the right margin into the next diamond
    a(f'<path d="M1130,{y1 + h / 2} H1165 V{y2 + 35} H1136" fill="none" stroke="{SLATE}" stroke-width="2.5" marker-end="url(#ah)"/>')
    a(f'<text x="1137" y="{y1 + h / 2 - 8}" font-size="13" font-weight="700" fill="{SLATE}">{t["no"]}</text>')
    diamond(1010, y2 + 35, 240, 110, t["q2"], t["q2n"])
    arrow(888, y2 + 35, 846, y2 + 35)
    step(610, y2, 230, h, *t["s4"])
    arrow(608, y2 + 35, 574, y2 + 35)
    step(300, y2, 270, h, *t["s5"], fill="#dbeafe", stroke=BLUE)
    arrow(298, y2 + 35, 264, y2 + 35)
    step(30, y2, 230, h, *t["s6"])
    a(f'<text x="30" y="{y2 + h + 30}" font-size="13" fill="{ORANGE}" font-weight="700">→ {t["json"]}</text>')
    a('</svg>')
    return "\n".join(o) + "\n"


# ---------------------------------------------------------------- host state

STATE_T = {
    "ja": {
        "online": "online", "offline": "offline", "removed": "一覧から削除",
        "rx": "JSON を受信", "timeout": "agent_timeout_sec（既定 5 秒）受信なし",
        "back": "JSON を受信", "forget": "forget_after_sec（既定 300 秒）受信なし",
        "on_note": "行は通常どおり表示", "off_note": "行は stale・HOST OFFLINE、host ノードは down",
    },
    "en": {
        "online": "online", "offline": "offline", "removed": "removed from list",
        "rx": "JSON received", "timeout": "nothing for agent_timeout_sec (default 5 s)",
        "back": "JSON received", "forget": "nothing for forget_after_sec (default 300 s)",
        "on_note": "rows shown normally", "off_note": "rows stale / HOST OFFLINE, host node down",
    },
}


def state_svg(lang):
    t = STATE_T[lang]
    W, H = 1200, 200
    o = []
    a = o.append
    a(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
      f'font-family="{FONT}" role="img" aria-label="host state transitions">')
    a(f'''<defs><marker id="ah2" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="{SLATE}"/></marker></defs>''')
    a(f'<rect width="{W}" height="{H}" rx="18" fill="#f8fafc"/>')
    cy = 100

    def state(x, w, label, note, fill, stroke, dash=""):
        d = f' stroke-dasharray="{dash}"' if dash else ""
        a(f'<rect x="{x}" y="{cy - 34}" width="{w}" height="68" rx="34" fill="{fill}" stroke="{stroke}" stroke-width="2.5"{d}/>')
        a(f'<text x="{x + w / 2}" y="{cy + 6}" text-anchor="middle" font-size="18" font-weight="700" fill="#0f172a">{label}</text>')
        a(f'<text x="{x + w / 2}" y="{cy + 62}" text-anchor="middle" font-size="12.5" fill="#475569">{note}</text>')

    # start dot
    a(f'<circle cx="50" cy="{cy}" r="10" fill="{SLATE}"/>')
    a(f'<line x1="62" y1="{cy}" x2="146" y2="{cy}" stroke="{SLATE}" stroke-width="2.5" marker-end="url(#ah2)"/>')
    a(f'<text x="104" y="{cy - 12}" text-anchor="middle" font-size="12.5" fill="{SLATE}">{t["rx"]}</text>')
    state(150, 200, t["online"], t["on_note"], "#dcfce7", "#16a34a")
    # self loop on online
    a(f'<path d="M215,{cy - 34} C215,{cy - 84} 285,{cy - 84} 285,{cy - 38}" fill="none" stroke="{SLATE}" stroke-width="2" marker-end="url(#ah2)"/>')
    a(f'<text x="250" y="{cy - 74}" text-anchor="middle" font-size="12.5" fill="{SLATE}">{t["rx"]}</text>')
    state(620, 200, t["offline"], t["off_note"], "#fee2e2", "#dc2626", "7 5")
    # online -> offline (upper), offline -> online (lower)
    a(f'<line x1="352" y1="{cy - 12}" x2="614" y2="{cy - 12}" stroke="{SLATE}" stroke-width="2.5" marker-end="url(#ah2)"/>')
    a(f'<text x="484" y="{cy - 22}" text-anchor="middle" font-size="12.5" fill="{SLATE}">{t["timeout"]}</text>')
    a(f'<line x1="618" y1="{cy + 12}" x2="356" y2="{cy + 12}" stroke="{SLATE}" stroke-width="2.5" marker-end="url(#ah2)"/>')
    a(f'<text x="484" y="{cy + 32}" text-anchor="middle" font-size="12.5" fill="{SLATE}">{t["back"]}</text>')
    # offline -> removed
    a(f'<line x1="822" y1="{cy}" x2="986" y2="{cy}" stroke="{SLATE}" stroke-width="2.5" marker-end="url(#ah2)"/>')
    a(f'<text x="904" y="{cy - 12}" text-anchor="middle" font-size="12" fill="{SLATE}">{t["forget"].split("（")[0].split(" (")[0]}</text>')
    rest = t["forget"][len(t["forget"].split("（")[0].split(" (")[0]):].strip()
    a(f'<text x="904" y="{cy + 22}" text-anchor="middle" font-size="12" fill="{SLATE}">{rest}</text>')
    a(f'<circle cx="1010" cy="{cy}" r="14" fill="none" stroke="{SLATE}" stroke-width="2.5"/><circle cx="1010" cy="{cy}" r="8" fill="{SLATE}"/>')
    a(f'<text x="1034" y="{cy + 5}" font-size="14" font-weight="700" fill="#0f172a">{t["removed"]}</text>')
    a('</svg>')
    return "\n".join(o) + "\n"


for name, fn in (("architecture", svg), ("agent_scan", scan_svg), ("host_state", state_svg)):
    (OUT / f"{name}.ja.svg").write_text(fn("ja"), encoding="utf-8")
    (OUT / f"{name}.svg").write_text(fn("en"), encoding="utf-8")
print("written")
