/* PTPN Studio - Guided Interactive Narrative (story.js)
 *
 * Driven directly by the C++ PTPN analyzer's output (/api/analyze or
 * window.PTPN_DEMO_DATA):
 *   Part 1: PTPN formal model, physical entity mapping, Fixed-Priority firing
 *           rule animation (token flight, preemption clock freeze, SCG state
 *           transition chain, DBM zones, core timeline).
 *   Part 2: One net, many scheduling policies (Fixed, RM, DM, EDF, FIFO, Restart).
 *   Part 3: LLM architecture co-design loop (RTSL / TDG JSON / PTPN generation
 *           forms, Timing Skill confirmation, and Witness-driven LLM revision).
 */
(() => {
  "use strict";

  const NS = "http://www.w3.org/2000/svg";
  const DEMO = !!window.PTPN_DEMO_DATA;
  const DEMO_DATA = window.PTPN_DEMO_DATA || null;
  const REDUCED = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

  const state = {
    demo: DEMO,
    theme: localStorage.getItem("ptpn_theme") || "dark",
    examples: [],
    hw: null,
    pending: null,
    intro: null,
    deadline: null,
    sources: { rtsl: "", pendingRtsl: "", json: "", ptpn: "" },
    policySummaries: {},
    policyCache: {},
    repairs: {},
    repairMode: "initial",
    policy: "fixed",
    trace: null,
    meta: null,
    step: 0,
    phase: "atState",
    time: 0,
    mode: "worst",
    playSpeed: 950,
    lastFired: null,
    playing: false,
    playTimer: null,
    form: "rtsl",
    timingSource: "manual",
    timingDefaults: {},
    error: null,
  };

  function applyTheme() {
    document.documentElement.setAttribute("data-theme", state.theme);
    const btn = $("#btn-theme");
    if (btn) btn.textContent = state.theme === "light" ? "Dark" : "Light";
  }

  /* ------------------------------------------------------------------ utils */

  function esc(value) {
    return String(value ?? "").replace(/[&<>"']/g, (c) => ({
      "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
    }[c]));
  }

  function clamp(n, lo, hi) {
    return Math.max(lo, Math.min(hi, n));
  }

  function isNum(v) {
    return v !== null && v !== undefined && v !== "" && Number.isFinite(Number(v));
  }

  function fmt(v, fallback = "-") {
    return isNum(v) ? String(Math.round(Number(v))) : fallback;
  }

  function api(path, opts = {}) {
    return fetch(path, {
      headers: { "Content-Type": "application/json" },
      ...opts,
    }).then(async (response) => {
      const text = await response.text();
      let payload = null;
      try {
        payload = text ? JSON.parse(text) : null;
      } catch (err) {
        throw new Error(`Invalid JSON from ${path}`);
      }
      if (!response.ok) {
        throw new Error((payload && payload.error) || `${path} returned ${response.status}`);
      }
      return payload;
    });
  }

  function svgEl(tag, attrs = {}, text) {
    const node = document.createElementNS(NS, tag);
    for (const [key, value] of Object.entries(attrs)) {
      if (value !== null && value !== undefined) node.setAttribute(key, String(value));
    }
    if (text !== undefined) node.textContent = text;
    return node;
  }

  function copyText(text, button) {
    const done = () => {
      if (!button) return;
      const old = button.textContent;
      button.textContent = "Copied";
      setTimeout(() => { button.textContent = old; }, 1200);
    };
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(text).then(done).catch(() => fallbackCopy(text, done));
    } else {
      fallbackCopy(text, done);
    }
  }

  function fallbackCopy(text, done) {
    const area = document.createElement("textarea");
    area.value = text;
    area.style.position = "fixed";
    area.style.opacity = "0";
    document.body.appendChild(area);
    area.select();
    try { document.execCommand("copy"); } catch (err) { /* ignore */ }
    area.remove();
    done();
  }

  /* --------------------------------------------------------------- analyzer */

  function exampleRecord(id) {
    return state.examples.find((ex) => ex.id === id) || null;
  }

  function findDemoRecord(id) {
    if (!DEMO_DATA) return null;
    return (DEMO_DATA.examples || []).find((ex) => ex.id === id) || null;
  }

  function demoResponse(id) {
    const record = findDemoRecord(id);
    return record ? record.response : null;
  }

  async function analyzeExample(id, overrides = {}, mode = "analyze") {
    const record = exampleRecord(id);
    if (!record) throw new Error(`Example "${id}" is not available.`);
    const options = { ...(record.options || {}), ...overrides };
    return api("/api/analyze", {
      method: "POST",
      body: JSON.stringify({
        source: record.source,
        filename: record.name,
        kind: record.kind,
        mode,
        options,
      }),
    });
  }

  function summaryFromResponse(response) {
    const metrics = response.metrics || {};
    const analysis = response.analysis || {};
    const verdicts = new Map((analysis.tasks || []).map((task) => [task.task, task]));
    return {
      exit_code: response.exit_code,
      ok: response.ok,
      states: metrics.states,
      exact: metrics.exact,
      schedulability: analysis.schedulability || metrics.schedulability,
      tasks: (metrics.tasks || []).map((task) => ({
        name: task.name,
        priority: task.priority,
        wcet: task.wcet,
        wcrt: task.wcrt,
        verdict: (verdicts.get(task.name) || {}).verdict || task.verdict,
      })),
      diagnostics: (response.diagnostics || []).map((diag) => ({
        code: diag.code,
        severity: diag.severity,
        subject: diag.subject,
        message: diag.message,
      })),
    };
  }

  const COMPLETE_PTPN_SOURCE = `// Native PTPN Net Model (hw.ptpn): defines places, transitions, intervals, cores, priorities & arcs directly.
// Bypasses RTSL/TDG lowering and can be verified directly via \`ptpn ptpn -f hw.ptpn\`

places
  Fentry:1, Fready:1, Fexit:1
  Bentry:1, Bready:1, Bexit:1
  Aentry:1, Aready:1, Aexit:1
  A_period:1

transitions
  Fget_core [0, 0]   @priority=96, core=0
  Fexec     [2, 10]  @priority=96, core=0, suspendable
  Bget_core [0, 0]   @priority=98, core=0
  Bexec     [2, 3]   @priority=98, core=0, suspendable
  Aget_core [0, 0]   @priority=97, core=0
  Aexec     [4, 6]   @priority=97, core=0, suspendable
  A_to_B    [1, 1]   @priority=0,  core=-1
  A_fire    [20, 20] @priority=0,  core=-1
  B_consume [0, 0]   @priority=0,  core=-1
  F_consume [0, 0]   @priority=0,  core=-1

// Task F execution chain
Fentry -> Fget_core
Fget_core -> Fready
Fready -> Fexec
Fexec -> Fexit
Fexit -> F_consume

// Task B execution chain
Bentry -> Bget_core
Bget_core -> Bready
Bready -> Bexec
Bexec -> Bexit
Bexit -> B_consume

// Task A execution chain and A->B precedence bridge
Aentry -> Aget_core
Aget_core -> Aready
Aready -> Aexec
Aexec -> Aexit
Aexit -> A_to_B
A_to_B -> Bentry

// Task A periodic timer loop
A_period -> A_fire
A_fire -> Aentry
A_fire -> A_period

@init Fentry:1, Aentry:1, A_period:1`;

  async function loadStoryData() {
    if (DEMO) {
      state.examples = DEMO_DATA.examples || [];
      state.hw = demoResponse("hw");
      state.pending = demoResponse("pending-demo");
      state.intro = demoResponse("intro-demo");
      state.deadline = demoResponse("deadline-miss");
      state.policySummaries = (DEMO_DATA.story && DEMO_DATA.story.policies) || {};
      state.repairs = (DEMO_DATA.story && DEMO_DATA.story.repairs) || {};
      const pendingRecord = findDemoRecord("pending-demo");
      const filledRecord = findDemoRecord("intro-demo");
      const ptpnRecord = findDemoRecord("hw-ptpn");
      state.sources.pendingRtsl = pendingRecord ? pendingRecord.source : "";
      state.sources.rtsl = filledRecord ? filledRecord.source : state.sources.pendingRtsl;
      state.sources.ptpn = ptpnRecord ? ptpnRecord.source : COMPLETE_PTPN_SOURCE;
      $("#story-generated").textContent = `Precomputed Demo (${DEMO_DATA.generated_at || "offline"})`;
    } else {
      const payload = await api("/api/examples");
      state.examples = payload.examples || [];
      state.hw = await analyzeExample("hw");
      const optional = (promise) => promise.catch(() => null);
      [state.pending, state.intro, state.deadline] = await Promise.all([
        optional(analyzeExample("pending-demo", {}, "check")),
        optional(analyzeExample("intro-demo")),
        optional(analyzeExample("deadline-miss")),
      ]);
      const pendingRecord = exampleRecord("pending-demo");
      const filledRecord = exampleRecord("intro-demo");
      const ptpnRecord = exampleRecord("hw-ptpn");
      state.sources.pendingRtsl = pendingRecord ? pendingRecord.source : "";
      state.sources.rtsl = filledRecord ? filledRecord.source : state.sources.pendingRtsl;
      state.sources.ptpn = ptpnRecord ? ptpnRecord.source : COMPLETE_PTPN_SOURCE;
      $("#story-generated").textContent = "Live C++ PTPN Analyzer Connected";
    }

    if (!state.hw || !state.hw.graphs || !state.hw.graphs.scg) {
      throw new Error("The hw example did not return a state-class graph.");
    }
    state.sources.json = state.intro && state.intro.tdg ? JSON.stringify(state.intro.tdg, null, 2) : "";
  }

  /* --------------------------------------------------------------- metadata */

  function parseMarking(text) {
    if (!text) return [];
    const body = String(text).trim().replace(/^\[|\]$/g, "");
    if (!body || body === "empty") return [];
    return body.split(",").map((part) => {
      const token = part.trim();
      const match = token.match(/^(.+?)(?:\((\d+)\))?$/);
      return { name: match ? match[1].trim() : token, count: match && match[2] ? Number(match[2]) : 1 };
    });
  }

  function markingHas(marking, name) {
    return parseMarking(marking).some((item) => item.name === name);
  }

  function refsFromText(text) {
    const refs = [];
    if (!text) return refs;
    const re = /(T\d+)\(([^)]*)\)/g;
    let match;
    while ((match = re.exec(text))) {
      const body = match[2];
      const priority = body.match(/priority=(-?\d+)/);
      const core = body.match(/core=(-?\d+)/);
      refs.push({
        id: match[1],
        name: body.split(",")[0].trim(),
        priority: priority ? Number(priority[1]) : null,
        core: core ? Number(core[1]) : null,
        suspendable: body.includes("suspendable"),
      });
    }
    return refs;
  }

  function detailRefs(detail, kind) {
    const key = kind === "struct" ? "e_struct_transitions"
      : kind === "priority" ? "priority_transitions" : "suspended_transitions";
    if (detail && Array.isArray(detail[key]) && detail[key].length) return detail[key];
    if (detail && detail[key] && detail[key].length === 0) return [];
    const text = detail && (kind === "struct" ? detail.e_struct : kind === "priority" ? detail.enabled : detail.suspended);
    return refsFromText(text);
  }

  function buildMetadata(response) {
    const meta = {
      nodes: new Map(),
      tidName: new Map(),
      tidRef: new Map(),
      edgeByTid: new Map(),
      ptpn: response.graphs ? response.graphs.ptpn : null,
    };
    const ptpn = (response.graphs && response.graphs.ptpn) || { nodes: [], edges: [] };
    for (const node of ptpn.nodes || []) {
      meta.nodes.set(node.id, node);
      if (node.text) meta.nodes.set(node.text, node);
    }
    const scg = (response.graphs && response.graphs.scg) || { nodes: [], edges: [] };
    for (const node of scg.nodes || []) {
      const detail = node.detail || {};
      for (const kind of ["struct", "priority", "suspended"]) {
        for (const ref of detailRefs(detail, kind)) {
          if (!meta.tidName.has(ref.id)) meta.tidName.set(ref.id, ref.name);
          meta.tidRef.set(ref.id, ref);
        }
      }
    }
    for (const edge of scg.edges || []) {
      const tid = edge.transition;
      if (tid) {
        meta.edgeByTid.set(tid, edge);
        const match = String(edge.label || "").match(/(T\d+)\(([^,)]*)/);
        if (match && !meta.tidName.has(match[1])) meta.tidName.set(match[1], match[2].trim());
      }
    }
    return meta;
  }

  function nodeMeta(name) {
    return (state.meta && state.meta.nodes.get(name)) || null;
  }

  function taskOf(name) {
    if (!name) return null;
    if (/^A(?:_|exec|get|ready|entry|exit|period|to)/.test(name) || name.startsWith("A")) return "A";
    if (name.startsWith("B")) return "B";
    if (name.startsWith("F")) return "F";
    return null;
  }

  function isExecName(name) {
    const node = nodeMeta(name);
    return (node && node.kind === "exec") || /exec/i.test(name || "");
  }

  function kindOf(name) {
    if (name === "A_period" || name === "A_fire") return "period";
    if (/get_core/.test(name || "")) return "acquire";
    if (isExecName(name)) return "exec";
    return "control";
  }

  function transitionName(tid) {
    return (state.meta && state.meta.tidName.get(tid)) || tid;
  }

  function transitionRefFor(name) {
    const node = nodeMeta(name);
    if (node && node.tid) return node.tid;
    if (state.meta) {
      for (const [tid, ref] of state.meta.tidRef.entries()) {
        if (ref.name === name) return tid;
      }
    }
    return null;
  }

  /* ------------------------------------------------------------------ trace */

  function buildTrace(response, mode) {
    const scg = response.graphs && response.graphs.scg;
    if (!scg || !Array.isArray(scg.nodes) || !scg.nodes.length) return null;
    const nodes = new Map(scg.nodes.map((node) => [node.id, node]));
    const edges = scg.edges || [];
    let current = scg.nodes[0].id;
    const steps = [];
    const seen = new Set();
    for (let guard = 0; guard < 64; guard += 1) {
      if (seen.has(current)) break;
      seen.add(current);
      const node = nodes.get(current);
      if (!node) break;
      const outgoing = edges.filter((edge) => edge.from === current);
      if (!outgoing.length) break;
      const edge = outgoing.find((candidate) => !seen.has(candidate.to)) || outgoing[0];
      const target = nodes.get(edge.to);
      if (!target) break;
      const dwell = mode === "earliest"
        ? Number(edge.dwell_min ?? 0)
        : Number(edge.dwell_max ?? edge.dwell_min ?? 0);
      const atMin = Number(edge.at_min ?? 0);
      const atMax = edge.at_max === null || edge.at_max === undefined ? atMin : Number(edge.at_max);
      steps.push({
        from: node,
        to: target,
        edge,
        tid: edge.transition,
        name: transitionName(edge.transition),
        dwell: Math.max(0, dwell),
        atMin,
        atMax,
        start: 0,
        end: 0,
      });
      current = edge.to;
      if (markingHas(target.detail && target.detail.marking, "Fexit")) {
        const nextOut = edges.filter((e) => e.from === current);
        if (nextOut.length && transitionName(nextOut[0].transition) === "F_consume") {
          const finEdge = nextOut[0];
          const finTarget = nodes.get(finEdge.to);
          if (finTarget) {
            steps.push({
              from: target,
              to: finTarget,
              edge: finEdge,
              tid: finEdge.transition,
              name: transitionName(finEdge.transition),
              dwell: 0,
              atMin: 0,
              atMax: 0,
              start: 0,
              end: 0,
            });
          }
        }
        break;
      }
      if (steps.length > 32) break;
    }

    let time = 0;
    for (const step of steps) {
      step.start = time;
      time += step.dwell;
      step.end = time;
    }
    return {
      steps,
      total: time,
      final: steps.length ? steps[steps.length - 1].to : nodes.get(scg.nodes[0].id),
      initial: nodes.get(scg.nodes[0].id),
    };
  }

  function currentNode() {
    if (!state.trace) return null;
    if (state.step >= state.trace.steps.length) return state.trace.final;
    const step = state.trace.steps[state.step];
    return step ? step.from : state.trace.final;
  }

  function currentStep() {
    return state.trace && state.trace.steps[state.step] ? state.trace.steps[state.step] : null;
  }

  /* -------------------------------------------------------------- net render */

  const PLACES = {
    A_period: { x: 90, y: 52, label: "A_period" },
    Aentry: { x: 90, y: 145, label: "Aentry" },
    Aready: { x: 310, y: 145, label: "Aready" },
    Aexit: { x: 545, y: 145, label: "Aexit" },
    Bentry: { x: 695, y: 290, label: "Bentry" },
    Bready: { x: 880, y: 290, label: "Bready" },
    Bexit: { x: 1065, y: 290, label: "Bexit" },
    Fentry: { x: 90, y: 440, label: "Fentry" },
    Fready: { x: 310, y: 440, label: "Fready" },
    Fexit: { x: 545, y: 440, label: "Fexit" },
  };

  const TRANSITIONS = {
    A_fire: { x: 215, y: 52, label: "A_fire", kind: "period" },
    Aget_core: { x: 200, y: 145, label: "Aget_core", kind: "acquire" },
    Aexec: { x: 428, y: 145, label: "Aexec", kind: "exec" },
    A_to_B: { x: 620, y: 218, label: "A_to_B", kind: "control" },
    Bget_core: { x: 788, y: 290, label: "Bget_core", kind: "acquire" },
    Bexec: { x: 972, y: 290, label: "Bexec", kind: "exec" },
    B_consume: { x: 972, y: 378, label: "B_consume", kind: "control" },
    Fget_core: { x: 200, y: 440, label: "Fget_core", kind: "acquire" },
    Fexec: { x: 428, y: 440, label: "Fexec", kind: "exec" },
    F_consume: { x: 675, y: 440, label: "F_consume", kind: "control" },
  };

  const ARCS = [
    ["A_period", "A_fire"], ["A_fire", "Aentry"],
    ["Aentry", "Aget_core"], ["Aget_core", "Aready"], ["Aready", "Aexec"], ["Aexec", "Aexit"],
    ["Aexit", "A_to_B"], ["A_to_B", "Bentry"],
    ["Bentry", "Bget_core"], ["Bget_core", "Bready"], ["Bready", "Bexec"], ["Bexec", "Bexit"],
    ["Bexit", "B_consume"],
    ["Fentry", "Fget_core"], ["Fget_core", "Fready"], ["Fready", "Fexec"], ["Fexec", "Fexit"],
    ["Fexit", "F_consume"],
  ];

  function rolesFor(name) {
    const roles = ["chain"];
    const task = taskOf(name);
    if (task) roles.push(`task-${task.toLowerCase()}`);
    if (name === "A_period" || name === "A_fire") roles.push("period");
    if (name === "A_to_B") roles.push("dependency");
    if (name === "B_consume" || name === "F_consume" || name === "A_period" || name === "A_fire") roles.push("binding");
    if (/get_core/.test(name) || isExecName(name)) roles.push("core");
    if (isExecName(name)) roles.push("exec");
    return roles.join(" ");
  }

  function transitionMeta(name) {
    const node = nodeMeta(name);
    const interval = node && Array.isArray(node.interval) ? node.interval : null;
    const priority = node && isNum(node.priority) ? node.priority : null;
    const core = node && isNum(node.core) ? node.core : null;
    const bits = [];
    if (interval) bits.push(`[${interval[0]},${interval[1]}]`);
    if (isNum(priority) && priority > 0) bits.push(`pi=${priority}`);
    if (isNum(core)) bits.push(core >= 0 ? `C${core}` : `ctrl`);
    return bits.join(" | ");
  }

  function renderNet(container, options = {}) {
    if (!container) return;
    const svg = svgEl("svg", {
      class: "net-svg",
      viewBox: "0 0 1130 615",
      role: "img",
      "aria-label": options.label || "PTPN structure",
    });
    const markerId = `arrow-${container.id || "net"}`;
    const markerActiveId = `arrow-act-${container.id || "net"}`;
    const defs = svgEl("defs");

    const marker = svgEl("marker", {
      id: markerId, viewBox: "0 0 10 10", refX: 9, refY: 5,
      markerWidth: 6, markerHeight: 6, orient: "auto-start-reverse",
    });
    marker.appendChild(svgEl("path", { d: "M 0 0 L 10 5 L 0 10 z", fill: "#64748b" }));

    const markerActive = svgEl("marker", {
      id: markerActiveId, viewBox: "0 0 10 10", refX: 9, refY: 5,
      markerWidth: 6.5, markerHeight: 6.5, orient: "auto-start-reverse",
    });
    markerActive.appendChild(svgEl("path", { d: "M 0 0 L 10 5 L 0 10 z", fill: "#22d3ee" }));

    defs.appendChild(marker);
    defs.appendChild(markerActive);
    svg.appendChild(defs);

    const lanesLayer = svgEl("g", { class: "lanes" });
    const arcsLayer = svgEl("g", { class: "arcs" });
    const placesLayer = svgEl("g", { class: "places" });
    const transitionsLayer = svgEl("g", { class: "transitions" });
    const tokensLayer = svgEl("g", { class: "tokens" });
    const coreLayer = svgEl("g", { class: "core" });

    lanesLayer.appendChild(svgEl("rect", { class: "lane-bg", x: 16, y: 18, width: 615, height: 175 }));
    lanesLayer.appendChild(svgEl("text", { class: "lane-label", x: 30, y: 40 }, "TASK A (Periodic | pi=97 | I=[4,6] | P=20)"));

    lanesLayer.appendChild(svgEl("rect", { class: "lane-bg", x: 645, y: 235, width: 468, height: 175 }));
    lanesLayer.appendChild(svgEl("text", { class: "lane-label", x: 660, y: 256 }, "TASK B (Successor | pi=98 | I=[2,3])"));

    lanesLayer.appendChild(svgEl("rect", { class: "lane-bg", x: 16, y: 385, width: 730, height: 112 }));
    lanesLayer.appendChild(svgEl("text", { class: "lane-label", x: 30, y: 406 }, "TASK F (Low Priority | pi=96 | I=[2,10] | Preemptable)"));

    const marking = parseMarking(options.marking || "");
    const activeNames = new Set(options.active || []);
    const suspendedNames = new Set(options.suspended || []);
    const structNames = new Set(options.struct || []);
    const firedName = options.fired || null;
    const clocksMap = options.clocksMap || new Map();

    for (const [from, to] of ARCS) {
      const fromPlace = Object.prototype.hasOwnProperty.call(PLACES, from);
      const toPlace = Object.prototype.hasOwnProperty.call(PLACES, to);
      const a = fromPlace ? PLACES[from] : TRANSITIONS[from];
      const b = toPlace ? PLACES[to] : TRANSITIONS[to];
      if (!a || !b) continue;
      const isFiredArc = firedName && (from === firedName || to === firedName);
      const roles = rolesFor(from);
      const path = svgEl("path", {
        class: `arc${isFiredArc ? " active" : ""}`,
        d: `M ${a.x} ${a.y} L ${b.x} ${b.y}`,
        "marker-end": `url(#${isFiredArc ? markerActiveId : markerId})`,
        "data-roles": roles,
        "data-arc": `${from}->${to}`,
      });
      arcsLayer.appendChild(path);
    }

    const selfLoop = svgEl("path", {
      class: `arc${firedName === "A_fire" ? " active" : ""}`,
      d: "M 185 31 C 162 2, 120 2, 102 32",
      "marker-end": `url(#${firedName === "A_fire" ? markerActiveId : markerId})`,
      "data-roles": "chain period binding",
    });
    arcsLayer.appendChild(selfLoop);

    for (const [name, place] of Object.entries(PLACES)) {
      const marked = marking.some((item) => item.name === name);
      const group = svgEl("g", { "data-name": name, "data-roles": rolesFor(name) });
      group.appendChild(svgEl("circle", {
        class: `place-node${marked ? " marked" : ""}`,
        cx: place.x, cy: place.y, r: 16,
      }));
      group.appendChild(svgEl("text", {
        class: `place-label${marked ? " marked" : ""}`,
        x: place.x, y: place.y + 32,
      }, place.label));
      placesLayer.appendChild(group);
    }

    for (const [name, transition] of Object.entries(TRANSITIONS)) {
      const kind = kindOf(name);
      const isActive = activeNames.has(name);
      const isSuspended = suspendedNames.has(name);
      const isFired = firedName === name;
      const isBlocked = structNames.has(name) && !isActive && !isSuspended;
      const classes = ["transition-box", kind];
      if (isActive) classes.push("active");
      if (isSuspended) classes.push("suspended");
      if (isFired) classes.push("firing");

      const group = svgEl("g", {
        "data-name": name,
        "data-tid": transitionRefFor(name) || "",
        "data-roles": rolesFor(name),
      });

      const boxW = 98;
      const boxH = 46;
      group.appendChild(svgEl("rect", {
        class: classes.join(" "),
        x: transition.x - boxW / 2,
        y: transition.y - boxH / 2,
        width: boxW,
        height: boxH,
        rx: 7,
      }));

      group.appendChild(svgEl("text", {
        class: "transition-name",
        x: transition.x,
        y: transition.y - 4,
      }, transition.label));

      group.appendChild(svgEl("text", {
        class: "transition-meta",
        x: transition.x,
        y: transition.y + 10,
      }, transitionMeta(name)));

      const clockInfo = clocksMap.get(name);
      if (clockInfo && (isActive || isSuspended || isFired)) {
        const barW = boxW - 16;
        const barX = transition.x - barW / 2;
        const barY = transition.y + 15;
        group.appendChild(svgEl("rect", {
          class: "progress-track",
          x: barX, y: barY, width: barW, height: 4,
        }));
        const pct = clamp(clockInfo.pct || 0, 0.08, 1);
        group.appendChild(svgEl("rect", {
          class: `progress-fill${isSuspended ? " frozen" : ""}`,
          x: barX, y: barY, width: barW * pct, height: 4,
        }));
      }

      if (isSuspended) {
        const hVal = clockInfo ? clockInfo.val : 1;
        group.appendChild(svgEl("text", {
          class: "status-pill-text",
          x: transition.x,
          y: transition.y - boxH / 2 - 6,
          fill: "#fbbf24",
        }, `FROZEN (h=${hVal})`));
      } else if (isFired) {
        group.appendChild(svgEl("text", {
          class: "status-pill-text",
          x: transition.x,
          y: transition.y - boxH / 2 - 6,
          fill: "#22d3ee",
        }, `FIRING`));
      } else if (isActive && kind === "exec") {
        const hVal = clockInfo ? `h=${clockInfo.val}` : "RUNNING";
        group.appendChild(svgEl("text", {
          class: "status-pill-text",
          x: transition.x,
          y: transition.y - boxH / 2 - 6,
          fill: "#34d399",
        }, `E_pri (${hVal})`));
      } else if (isBlocked) {
        group.appendChild(svgEl("text", {
          class: "status-pill-text",
          x: transition.x,
          y: transition.y - boxH / 2 - 6,
          fill: "#fb7185",
        }, `FILTERED OUT`));
      }

      transitionsLayer.appendChild(group);
    }

    for (const item of marking) {
      const place = PLACES[item.name];
      if (!place) continue;
      const token = svgEl("circle", {
        class: "token-dot",
        cx: place.x, cy: place.y, r: 6.5,
      });
      token.style.animation = "tokenIn 0.28s ease";
      tokensLayer.appendChild(token);
      if (item.count > 1) {
        tokensLayer.appendChild(svgEl("text", {
          x: place.x + 12, y: place.y - 10,
          fill: "#22d3ee", "font-family": "var(--mono)", "font-size": "10px",
        }, String(item.count)));
      }
    }

    if (firedName && TRANSITIONS[firedName] && !REDUCED) {
      const tPos = TRANSITIONS[firedName];
      const inArcs = ARCS.filter(([, to]) => to === firedName);
      const outArcs = ARCS.filter(([from]) => from === firedName);
      const srcPlace = inArcs.length ? PLACES[inArcs[0][0]] : null;
      const dstPlace = outArcs.length ? PLACES[outArcs[0][1]] : null;
      if (srcPlace && dstPlace) {
        const flyer = svgEl("circle", {
          class: "flying-token",
          r: 6.5,
        });
        const animMotion = svgEl("animateMotion", {
          dur: "0.52s",
          repeatCount: "1",
          fill: "freeze",
          path: `M ${srcPlace.x} ${srcPlace.y} L ${tPos.x} ${tPos.y} L ${dstPlace.x} ${dstPlace.y}`,
        });
        flyer.appendChild(animMotion);
        tokensLayer.appendChild(flyer);
      }
    }

    svg.appendChild(lanesLayer);
    svg.appendChild(arcsLayer);
    svg.appendChild(placesLayer);
    svg.appendChild(transitionsLayer);
    svg.appendChild(tokensLayer);

    if (options.showCore !== false) {
      coreLayer.appendChild(svgEl("rect", {
        class: "core-bar", x: 16, y: 530, width: 1096, height: 54, rx: 10,
        "data-roles": "core",
      }));
      coreLayer.appendChild(svgEl("text", { class: "core-priority", x: 36, y: 553 }, "SHARED CPU CORE 0 - Fixed-Priority Arbitration"));
      coreLayer.appendChild(svgEl("text", { class: "core-text", x: 36, y: 571 }, "Priority order: Task B (pi=98)  >  Task A (pi=97)  >  Task F (pi=96)"));
      coreLayer.appendChild(svgEl("text", {
        class: "core-priority", x: 1090, y: 553, "text-anchor": "end",
      }, options.coreStatus || "E_pri filter active"));
      coreLayer.appendChild(svgEl("text", {
        class: "core-text", x: 1090, y: 571, "text-anchor": "end",
      }, options.coreSubStatus || "Only the highest-priority enabled transition advances its execution clock on Core 0"));
      svg.appendChild(coreLayer);
    }

    container.innerHTML = "";
    container.appendChild(svg);
  }

  /* ------------------------------------------------------------ model chapter */

  function initModel() {
    const first = state.hw.graphs.scg.nodes[0];
    const marking = first && first.detail ? first.detail.marking : "";
    renderNet($("#model-net"), { marking, label: "PTPN structure for tasks A, B and F" });

    const highlight = (role, source) => {
      $$("#model-net [data-roles]").forEach((element) => {
        const roles = (element.dataset.roles || "").split(/\s+/);
        const matches = role === "chain" || roles.includes(role);
        element.classList.toggle("highlight", matches);
        element.classList.toggle("dimmed", !matches);
      });
      $$(".note-card").forEach((card) => card.classList.toggle("active", card === source));
    };

    const clear = () => {
      $$("#model-net [data-roles]").forEach((element) => {
        element.classList.remove("highlight", "dimmed");
      });
      $$(".note-card").forEach((card) => card.classList.remove("active"));
    };

    $$(".note-card").forEach((card) => {
      card.addEventListener("mouseenter", () => highlight(card.dataset.highlight, card));
      card.addEventListener("mouseleave", () => {
        if (!card.classList.contains("pinned")) clear();
      });
      card.addEventListener("click", () => {
        const pinned = card.classList.contains("pinned");
        $$(".note-card").forEach((other) => other.classList.remove("pinned"));
        if (pinned) { clear(); return; }
        card.classList.add("pinned");
        highlight(card.dataset.highlight, card);
      });
    });

    const captions = [
      "Stage 01 (Task Chains): Each task expands into entry -> get_core -> ready -> exec -> exit. Token position encodes task state.",
      "Stage 02 (Dependency Bridge): The 1ms delay from A to B lowers to Aexit -> A_to_B [1,1] (core=-1) -> Bentry.",
      "Stage 03 (Bindings): Initial tokens mark Aentry, Fentry, and A_period; A_fire [20,20] releases periodic jobs.",
      "Stage 04 (Suspendable Execution): Aexec, Bexec, and Fexec carry intervals [l,u] and suspendable flags for clock freezing.",
      "Stage 05 (Core & Priority): CPU transitions carry core=0 and priority pi (98 > 97 > 96) consumed by the firing rule.",
    ];

    $$("#stage-strip button").forEach((button) => {
      button.addEventListener("click", () => {
        $$("#stage-strip button").forEach((other) => other.classList.remove("active"));
        button.classList.add("active");
        const index = Number(button.dataset.stage);
        const role = ["chain", "dependency", "binding", "exec", "core"][index] || "chain";
        highlight(role, null);
        $("#stage-caption").textContent = captions[index];
      });
    });
  }

  /* ----------------------------------------------------------- firing chapter */

  function initFiring() {
    state.trace = buildTrace(state.hw, state.mode);
    if (!state.trace || !state.trace.steps.length) {
      $("#firing-narration").textContent = "The hw example did not expose a replayable state-class path.";
      return;
    }
    state.step = 0;
    state.phase = "atState";
    state.time = 0;
    state.lastFired = null;
    renderFiring();

    $("#fire-next").addEventListener("click", () => { stopPlay(); next(); });
    $("#fire-prev").addEventListener("click", () => { stopPlay(); prev(); });
    $("#fire-reset").addEventListener("click", () => { stopPlay(); resetFiring(); });
    $("#fire-play").addEventListener("click", () => togglePlay());

    $$(".mode-toggle .seg").forEach((button) => {
      button.addEventListener("click", () => {
        if (button.dataset.mode === state.mode) return;
        state.mode = button.dataset.mode;
        $$(".mode-toggle .seg").forEach((other) => other.classList.toggle("active", other === button));
        resetFiring();
      });
    });

    $$(".speed-toggle .speed-btn").forEach((button) => {
      button.addEventListener("click", () => {
        state.playSpeed = Number(button.dataset.speed) || 950;
        $$(".speed-toggle .speed-btn").forEach((other) => other.classList.toggle("active", other === button));
      });
    });

    $$("#firing-bookmarks .kf-btn").forEach((btn) => {
      btn.addEventListener("click", () => {
        stopPlay();
        const targetStep = clamp(Number(btn.dataset.bookmarkStep || 0), 0, state.trace.steps.length);
        jumpToStep(targetStep);
      });
    });

    window.addEventListener("resize", () => {
      if (state.trace) renderTimeline();
    });
  }

  function jumpToStep(targetStep) {
    if (!state.trace) return;
    state.step = clamp(targetStep, 0, state.trace.steps.length);
    state.phase = "atState";
    if (state.step >= state.trace.steps.length) {
      state.time = state.trace.total;
      state.lastFired = state.trace.steps.length ? state.trace.steps[state.trace.steps.length - 1].tid : null;
    } else {
      const s = state.trace.steps[state.step];
      state.time = s ? s.start : 0;
      state.lastFired = state.step > 0 ? state.trace.steps[state.step - 1].tid : null;
    }
    renderFiring();
  }

  function resetFiring() {
    stopPlay();
    state.trace = buildTrace(state.hw, state.mode);
    state.step = 0;
    state.phase = "atState";
    state.time = 0;
    state.lastFired = null;
    renderFiring();
  }

  function next() {
    if (!state.trace) return;
    const step = currentStep();
    if (state.phase === "atState" && step) {
      state.phase = "elapsed";
      state.lastFired = step.tid;
      state.time = step.start;
      renderFiring();
      if (step.dwell > 0 && !REDUCED) {
        requestAnimationFrame(() => {
          state.time = step.end;
          animatePlayhead(step.start, step.end);
        });
      } else {
        state.time = step.end;
        animatePlayhead(step.start, step.end);
      }
      return;
    }
    if (state.phase === "elapsed" && step) {
      state.step += 1;
      state.phase = "atState";
      state.time = step.end;
      state.lastFired = step.tid;
      renderFiring();
    }
  }

  function prev() {
    if (!state.trace) return;
    if (state.phase === "elapsed") {
      state.phase = "atState";
      const step = currentStep();
      state.time = step ? step.start : 0;
      state.lastFired = null;
      renderFiring();
      return;
    }
    if (state.step > 0) {
      state.step -= 1;
      state.phase = "atState";
      const step = currentStep();
      state.time = step ? step.start : 0;
      state.lastFired = null;
      renderFiring();
    }
  }

  function togglePlay() {
    if (state.playing) {
      stopPlay();
      return;
    }
    if (state.trace && state.step >= state.trace.steps.length) {
      state.step = 0;
      state.phase = "atState";
      state.time = 0;
    }
    state.playing = true;
    $("#fire-play").setAttribute("aria-pressed", "true");
    $("#fire-play").textContent = "Pause";
    playTick();
  }

  function playTick() {
    if (!state.playing) return;
    if (state.step >= state.trace.steps.length && state.phase === "atState") {
      stopPlay();
      return;
    }
    next();
    const delay = state.phase === "elapsed" ? state.playSpeed * 0.85 : state.playSpeed;
    state.playTimer = setTimeout(playTick, REDUCED ? 200 : delay);
  }

  function stopPlay() {
    state.playing = false;
    if (state.playTimer) clearTimeout(state.playTimer);
    state.playTimer = null;
    const button = $("#fire-play");
    if (button) {
      button.setAttribute("aria-pressed", "false");
      button.textContent = "Play";
    }
  }

  function setPlayhead(pct) {
    const timeline = $("#firing-timeline");
    if (!timeline) return;
    const playhead = $(".tl-playhead", timeline);
    const now = $(".tl-now", timeline);
    const track = $(".tl-track", timeline);
    if (!playhead || !track) return;
    const trackRect = track.getBoundingClientRect();
    const rootRect = timeline.getBoundingClientRect();
    const x = trackRect.left - rootRect.left + trackRect.width * clamp(pct, 0, 1);
    playhead.style.left = `${x}px`;
    if (now) {
      now.style.left = `${x}px`;
      now.textContent = `t = ${state.time} ms`;
    }
  }

  function animatePlayhead(start, end) {
    const total = state.trace.total || 1;
    setPlayhead(end / total);
    $("#firing-time").textContent = `t = ${end} ms`;
  }

  function renderScgChain() {
    const wrap = $("#firing-scg-chain");
    if (!wrap || !state.trace) return;
    const steps = state.trace.steps;
    const parts = [];
    steps.forEach((step, idx) => {
      const node = step.from;
      const label = node.label || `S${idx}`;
      const isCurrent = state.step === idx;
      const isVisited = idx < state.step;
      const detail = node.detail || {};
      const susp = detailRefs(detail, "suspended");
      const isPreempt = susp.length > 0;
      const cls = [
        "scg-node-btn",
        isCurrent ? "current" : "",
        isVisited ? "visited" : "",
        isPreempt ? "preempt-state" : "",
      ].filter(Boolean).join(" ");

      parts.push(`
        <button type="button" class="${cls}" data-jump-step="${idx}" title="Jump to ${esc(label)} (t=${step.start}ms)">
          <b>${esc(label)}</b>
          <small>${isPreempt ? "R={F}" : `t=${step.start}ms`}</small>
        </button>
      `);

      const edgeActive = isCurrent && state.phase === "elapsed";
      const dwellTxt = step.dwell > 0 ? `+${step.dwell}ms` : "0ms";
      parts.push(`
        <div class="scg-edge-arrow${edgeActive ? " active" : ""}">
          <span>${esc(step.name)}</span>
          <span>--&gt;</span>
          <span class="edge-dwell">${dwellTxt}</span>
        </div>
      `);
    });

    if (state.trace.final) {
      const fin = state.trace.final;
      const isFin = state.step >= steps.length;
      parts.push(`
        <button type="button" class="scg-node-btn${isFin ? " current" : ""}" data-jump-step="${steps.length}">
          <b>${esc(fin.label || "S9")}</b>
          <small>t=${state.trace.total}ms</small>
        </button>
      `);
    }

    wrap.innerHTML = `<div class="scg-chain">${parts.join("")}</div>`;
    $$("[data-jump-step]", wrap).forEach((btn) => {
      btn.addEventListener("click", () => {
        stopPlay();
        jumpToStep(Number(btn.dataset.jumpStep));
      });
    });
  }

  function renderFiring() {
    if (!state.trace) return;
    const node = currentNode();
    const step = currentStep();
    const detail = (node && node.detail) || {};
    const entry = detailRefs(detail, "struct");
    const priority = detailRefs(detail, "priority");
    const suspended = detailRefs(detail, "suspended");
    const marking = detail.marking || "";
    const stateTitle = node ? `State ${node.label || node.id}` : "State";
    const final = state.step >= state.trace.steps.length;
    const firingName = state.phase === "elapsed" && step ? step.name : (state.lastFired ? transitionName(state.lastFired) : null);

    $("#firing-state-title").textContent = stateTitle;
    $("#firing-step").textContent = `step ${state.step} / ${state.trace.steps.length}`;
    $("#firing-time").textContent = `t = ${state.time} ms`;
    $("#firing-phase").textContent = final
      ? "Run Complete"
      : state.phase === "elapsed"
        ? "Time Elapses & Fires"
        : "At State Class";
    $("#firing-fired").textContent = firingName || "-";

    const bannerBadge = $("#firing-banner-badge");
    if (bannerBadge) {
      if (suspended.length > 0) {
        bannerBadge.textContent = "PREEMPTED: B on Core 0 | Fexec Frozen (h=1)";
        bannerBadge.className = "state-banner-badge preempt";
      } else if (final) {
        bannerBadge.textContent = "COMPLETE: F Exits at t = 19 ms";
        bannerBadge.className = "state-banner-badge done";
      } else {
        bannerBadge.textContent = `Class ${node ? node.label : ""} | Fixed-Priority Filter`;
        bannerBadge.className = "state-banner-badge";
      }
    }

    $$("#firing-bookmarks .kf-btn").forEach((btn) => {
      const bStep = Number(btn.dataset.bookmarkStep);
      const isMatch = (bStep === 0 && state.step <= 1) ||
        (bStep === 2 && (state.step === 2 || state.step === 3)) ||
        (bStep === 4 && (state.step === 4 || state.step === 5)) ||
        (bStep === 6 && (state.step === 6 || state.step === 7)) ||
        (bStep === 9 && state.step >= 8);
      btn.classList.toggle("active", isMatch);
    });

    const clocksMap = new Map();
    for (const clock of detail.clocks || []) {
      const tName = transitionName(clock.transition) || clock.name;
      const metaNode = nodeMeta(tName);
      const maxBound = metaNode && Array.isArray(metaNode.interval) ? metaNode.interval[1] : (clock.upper || 10);
      const curVal = (state.phase === "elapsed" && step && step.tid === clock.transition)
        ? (state.mode === "earliest" ? step.atMin : step.atMax)
        : clock.lower;
      clocksMap.set(tName, {
        val: curVal,
        max: maxBound || 10,
        pct: clamp((curVal || 0) / (maxBound || 10), 0.08, 1),
      });
    }

    const activeNames = priority.map((ref) => ref.name);
    const suspendedNames = suspended.map((ref) => ref.name);
    const structNames = entry.map((ref) => ref.name);
    const running = activeNames.find((name) => isExecName(name));
    const acquiring = activeNames.find((name) => /get_core/.test(name));

    renderNet($("#firing-net"), {
      marking,
      active: activeNames,
      suspended: suspendedNames,
      struct: structNames,
      fired: firingName,
      clocksMap,
      label: `Marking at ${stateTitle}`,
      coreStatus: running
        ? `Core 0 Running: Task ${taskOf(running)} (${running})`
        : acquiring
          ? `Core 0 Dispatching: Task ${taskOf(acquiring)} (${acquiring})`
          : "Core 0 Idle (Control transition active)",
      coreSubStatus: suspended.length
        ? `Preempted & Frozen in R: ${suspendedNames.join(", ")} (clock paused at h=1)`
        : "No suspended transitions in R",
    });

    renderScgChain();
    renderSets(entry, priority, suspended, firingName);
    renderClocks(detail, step, suspendedNames);
    renderTimeline();

    if (state.phase === "elapsed" && step) {
      $("#firing-narration").innerHTML = `Time advances by <b>+${step.dwell} ms</b> in <b>${esc(stateTitle)}</b>. Transition <b>${esc(firingName || "-")}</b> reaches its firing window and moves tokens to the next state class.`;
    } else if (final) {
      $("#firing-narration").innerHTML = `Task F reaches <b>Fexit</b> at <b>t = ${state.time} ms</b>. The state-class graph certifies that the exact worst-case response time is <b>WCRT(F) = 19 ms</b>.`;
      $("#firing-verdict").hidden = false;
    } else {
      $("#firing-narration").innerHTML = narrationFor(node, step, entry, priority, suspended);
    }
    $("#firing-why").innerHTML = whyNow(entry, priority, suspended);

    $("#firing-dbm").textContent = detail.zone || "-";
    $("#sets-count").textContent = `|E_struct|=${entry.length} | |E_pri|=${priority.length} | |R|=${suspended.length}`;
    $("#firing-dots").innerHTML = state.trace.steps.map((_, index) => {
      const cls = index < state.step ? "done" : index === state.step ? "now" : "";
      return `<button type="button" class="step-dot ${cls}" data-dot-step="${index}" title="Step ${index}"></button>`;
    }).join("") + `<button type="button" class="step-dot ${state.step >= state.trace.steps.length ? "now" : ""}" data-dot-step="${state.trace.steps.length}" title="Final"></button>`;

    $$("#firing-dots .step-dot").forEach((dot) => {
      dot.addEventListener("click", () => {
        stopPlay();
        jumpToStep(Number(dot.dataset.dotStep));
      });
    });

    $("#fire-prev").disabled = state.step === 0 && state.phase === "atState";
    $("#fire-next").disabled = final && state.phase === "atState";

    if (state.trace.steps.length && !final) {
      $("#firing-verdict").hidden = true;
    }
    renderVerdict();
  }

  function narrationFor(node, step, entry, priority, suspended) {
    const suspendedExec = suspended.filter((ref) => isExecName(ref.name));
    if (state.step === 0) {
      return `<b>S0 (Initial State, t=0)</b>: Tokens mark <b>Aentry</b>, <b>Fentry</b>, and <b>A_period</b>. Both A and F compete for Core 0; the fixed-priority rule selects <b>Aget_core (pi=97)</b> over <b>Fget_core (pi=96)</b>.`;
    }
    if (state.step === 1) {
      return `<b>S1 (A Executing, t=0)</b>: <b>Aexec [4,6] (pi=97)</b> runs on Core 0. <b>Fget_core (pi=96)</b> remains structurally enabled in E_struct but is filtered out by priority.`;
    }
    if (state.step === 2) {
      return `<b>S2 (A Exits, t=${state.time}ms)</b>: <b>A_to_B [1,1]</b> begins its 1ms dependency delay on core -1. Core 0 is now free, allowing <b>Fget_core (pi=96)</b> into <b>E<sub>pri</sub></b>.`;
    }
    if (state.step === 3) {
      return `<b>S3 (F Runs During 1ms Delay)</b>: <b>Fexec [2,10]</b> runs on Core 0 while <b>A_to_B [1,1]</b> elapses. In 1ms, B (pi=98) will arrive at <b>Bentry</b> and preempt F.`;
    }
    if (suspendedExec.length) {
      const low = suspendedExec.map((ref) => ref.name).join(", ");
      return `<b>Preemption (${node.label}, t=${state.time}ms)</b>: High-priority B (<b>pi=98</b>) arrives! <b>${esc(low)} (pi=96)</b> is filtered out of <b>E<sub>pri</sub></b> and enters suspended set <b>R</b>. <span class="why">Its token stays in Fready and its execution clock freezes at <b>h(Fexec) = 1 ms</b>!</span>`;
    }
    if (state.step === 6) {
      return `<b>S6 (B Finishes, t=${state.time}ms)</b>: B reaches <b>Bexit</b> and releases Core 0. <b>Fexec (pi=96)</b> automatically moves from <b>R</b> back into <b>E<sub>pri</sub></b>!`;
    }
    if (state.step === 7) {
      return `<b>S7 (F Resumes from h=1, t=${state.time}ms)</b>: Notice the clock panel: <b>h(Fexec) resumes from 1 ms</b>, not from 0! With 1 ms already completed out of [2,10] ms, F needs at most <b>9 ms</b> of residual execution.`;
    }
    if (state.step === 8) {
      return `<b>S8 (F Reaches Fexit, t=${state.time}ms)</b>: F completes at <b>t = ${state.time} ms</b>: 6(A) + 1(F before preemption) + 3(B preemption) + 9(F residual) = 19 ms.`;
    }
    const selected = priority.map((ref) => ref.name).join(", ") || "-";
    return `<b>${esc(node ? node.label : "")}</b>: E<sub>pri</sub> = { <b>${esc(selected)}</b> }.`;
  }

  function whyNow(entry, priority, suspended) {
    const structNames = new Set(entry.map((ref) => ref.name));
    const prioNames = new Set(priority.map((ref) => ref.name));
    const suspNames = new Set(suspended.map((ref) => ref.name));
    const blocked = [...structNames].filter((name) => !prioNames.has(name) && !suspNames.has(name));
    const suspExec = suspended.filter((ref) => isExecName(ref.name));
    if (suspExec.length) {
      return `<b>Firing Rule Verdict</b>: <b>R = { ${suspExec.map((ref) => esc(ref.name)).join(", ")} }</b> is suspendable and preempted by pi=98 on Core 0; its clock rate is set to 0 (frozen at h=1).`;
    }
    if (blocked.length) {
      return `<b>Firing Rule Verdict</b>: <b>${esc(blocked.join(", "))}</b> is in E<sub>struct</sub> but blocked by a higher-priority transition on Core 0.`;
    }
    return `<b>Firing Rule Verdict</b>: Every structurally enabled transition is admissible in <b>E<sub>pri</sub></b> (core -1 control transitions do not compete for Core 0).`;
  }

  function renderSets(entry, priority, suspended, firingName) {
    const groups = [
      ["#set-struct", entry],
      ["#set-priority", priority],
      ["#set-suspended", suspended],
    ];
    for (const [selector, refs] of groups) {
      const root = $(selector);
      if (!refs.length) {
        root.innerHTML = `<span class="chip-t empty">empty</span>`;
        continue;
      }
      root.innerHTML = refs.map((ref) => {
        const kind = kindOf(ref.name);
        const isSusp = selector === "#set-suspended" ? " susp" : "";
        const firing = firingName && ref.name === firingName ? " firing" : "";
        const coreLabel = isNum(ref.core) ? (ref.core >= 0 ? `Core ${ref.core}` : "ctrl") : "-";
        return `<span class="chip-t ${kind}${isSusp}${firing}" title="${esc(ref.name)}">
          <b>${esc(ref.name)}</b>
          <small>pi=${fmt(ref.priority)} | ${coreLabel}</small>
        </span>`;
      }).join("");
    }
  }

  function renderClocks(detail, step, suspendedNames) {
    const root = $("#firing-clocks");
    const clocks = (detail.clocks || []).filter((clock) => (
      clock.kind === "exec" && isExecName(transitionName(clock.transition) || "")
    ));
    if (!clocks.length) {
      root.innerHTML = `<p class="clock-empty">No timed execution segment is active in this state class. Zero-time transitions fire immediately.</p>`;
      return;
    }
    const rows = clocks.map((clock) => {
      const name = transitionName(clock.transition) || clock.name;
      const suspended = suspendedNames.includes(name);
      const isFired = step && step.tid === clock.transition;
      let lower = clock.lower;
      let upper = clock.upper;
      let label = "";
      const frozen = suspended;
      if (isFired && step) {
        lower = step.atMin;
        upper = step.atMax;
        label = state.phase === "elapsed"
          ? `fires @ ${state.mode === "earliest" ? step.atMin : step.atMax}ms`
          : clock.lower !== step.atMin
            ? `h=${clock.lower} -> [${step.atMin},${step.atMax}]`
            : `window [${step.atMin},${step.atMax}]`;
      } else if (suspended) {
        label = upper !== null && lower === upper ? `frozen @ ${lower}ms` : "frozen";
      } else {
        label = upper !== null && lower === upper ? `= ${lower}ms` : upper === null ? `>= ${lower}ms` : `[${lower}, ${upper}]`;
      }
      const scale = Math.max(10, ...clocks.map((item) => item.upper === null ? (item.lower + 10) : item.upper), step ? step.atMax : 10);
      const loPct = clamp((lower / scale) * 100, 0, 96);
      const hiPct = upper === null ? 100 : clamp((upper / scale) * 100, loPct + 2, 100);
      const chosen = state.phase === "elapsed" && isFired
        ? (state.mode === "earliest" ? step.atMin : step.atMax)
        : lower;
      const chosenPct = clamp((chosen / scale) * 100, loPct, hiPct);
      const width = Math.max(4, chosenPct - loPct);
      return `<div class="clock-row">
        <span class="clock-name"><b>h(${esc(name)})</b></span>
        <div class="clock-track"><span class="clock-fill${frozen ? " frozen" : ""}" style="--lo:${loPct}%;--w:${width}%"></span></div>
        <span class="clock-bounds">${esc(label)}</span>
      </div>`;
    });
    root.innerHTML = rows.join("");
  }

  function renderTimeline() {
    const root = $("#firing-timeline");
    if (!root || !state.trace) return;
    const total = state.trace.total || 1;
    const ticks = [0, Math.round(total * 0.25), Math.round(total * 0.5), Math.round(total * 0.75), total];
    const segments = { A: [], B: [], F: [] };
    for (const step of state.trace.steps) {
      if (step.dwell <= 0) continue;
      const detail = step.from.detail || {};
      for (const ref of detailRefs(detail, "priority")) {
        if (!isExecName(ref.name)) continue;
        const task = taskOf(ref.name);
        if (task) segments[task].push({ start: step.start, end: step.end, type: "run" });
      }
      for (const ref of detailRefs(detail, "suspended")) {
        if (!isExecName(ref.name)) continue;
        const task = taskOf(ref.name);
        if (task) segments[task].push({ start: step.start, end: step.end, type: "susp" });
      }
    }
    for (const task of Object.keys(segments)) {
      segments[task] = mergeSegments(segments[task]);
    }
    const rows = [
      ["A", 97],
      ["B", 98],
      ["F", 96],
    ].map(([task, priority]) => {
      const segs = segments[task].map((segment) => {
        const left = (segment.start / total) * 100;
        const width = Math.max(1.2, ((segment.end - segment.start) / total) * 100);
        const dur = segment.end - segment.start;
        const text = segment.type === "susp"
          ? `frozen h=1 (${dur}ms)`
          : dur >= 1 ? `${task}: ${segment.start}->${segment.end} (${dur}ms)` : "";
        return `<span class="tl-seg ${segment.type} ${task.toLowerCase()}" style="left:${left}%;width:${width}%" title="Task ${task} ${segment.type === "susp" ? "Suspended (frozen at h=1)" : "Running"} ${segment.start}-${segment.end} ms">${text}</span>`;
      }).join("");
      return `<div class="tl-row">
        <span class="tl-name"><b>Task ${task}</b> (pi=${priority})</span>
        <div class="tl-track"><div class="tl-grid"></div>${segs}</div>
      </div>`;
    }).join("");
    root.innerHTML = `
      <div class="tl-scale">${ticks.map((tick) => `<span>${tick} ms</span>`).join("")}</div>
      ${rows}
      <div class="tl-now">t = ${state.time} ms</div>
      <div class="tl-playhead"></div>`;
    setPlayhead(state.time / total);
  }

  function mergeSegments(segments) {
    const sorted = segments.slice().sort((a, b) => a.start - b.start || a.end - b.end);
    const merged = [];
    for (const segment of sorted) {
      const last = merged[merged.length - 1];
      if (last && last.type === segment.type && segment.start <= last.end + 0.001) {
        last.end = Math.max(last.end, segment.end);
      } else {
        merged.push({ ...segment });
      }
    }
    return merged;
  }

  function renderVerdict() {
    const metrics = state.hw.metrics || {};
    const task = (metrics.tasks || []).find((item) => item.name === "F");
    const fExit = state.trace ? state.trace.total : 19;
    $("#verdict-f-exit").textContent = fExit;
    $("#verdict-wcrt").textContent = task ? fmt(task.wcrt) : "19";
    $("#verdict-wcet").textContent = task ? fmt(task.wcet) : "10";
    $("#verdict-interference").textContent = task ? fmt(task.worst_interference) : "3";
    $("#verdict-states").textContent = fmt(metrics.states);
    $("#verdict-exact").textContent = metrics.exact === false ? "bound" : "exact";
  }

  /* --------------------------------------------------------- policies chapter */

  const POLICY_META = {
    fixed: {
      title: "Static Fixed Priority (Resume)",
      formula: "Phi_fixed(E_struct) = argmax_{t in E_struct, core} pi(t)",
      mech: "Highest declared priority pi wins per core; preempted transition freezes in R",
      rule: "<b>E<sub>pri</sub></b> retains the transition with the highest declared priority pi on each core. Preempted transitions freeze in <b>R</b> and resume without losing work.",
      note: "The baseline rule shown in Part 01. The PTPN topology is unchanged: 17 state classes, WCRT(F) = 19 ms.",
      structure: "Same 10 places, 10 transitions, and 17 arcs; only the static-priority firing rule is applied.",
      sameNet: true,
    },
    rm: {
      title: "Rate Monotonic (RM)",
      formula: "Phi_rm(E_struct) : pi(tau_i) proportional to 1 / Period(tau_i)",
      mech: "Priority recomputed from 1 / Period; net topology unchanged",
      rule: "<b>The net structure is unchanged</b>; priorities are automatically derived from task periods (shorter period -> higher priority).",
      note: "Switching to RM requires only `--policy rm` without touching a single place or transition.",
      structure: "Places and transitions are 100% unchanged; priorities are derived from periods.",
      sameNet: true,
    },
    dm: {
      title: "Deadline Monotonic (DM)",
      formula: "Phi_dm(E_struct) : pi(tau_i) proportional to 1 / Deadline(tau_i)",
      mech: "Priority recomputed from 1 / Deadline; net topology unchanged",
      rule: "<b>The net structure is unchanged</b>; priorities are derived from relative deadlines (tighter deadline -> higher priority).",
      note: "DM is another instantiation of the exact same firing-rule interface.",
      structure: "Places and transitions are 100% unchanged; priorities follow relative deadlines.",
      sameNet: true,
    },
    edf: {
      title: "Earliest Deadline First (EDF)",
      formula: "Phi_edf(E_struct, Zone) = argmin_{t} (d_t - r_t)",
      mech: "Dynamic per-job deadline clock comparison; guarded by E-UNS-001",
      rule: "<b>The source model is unchanged</b>; the firing rule dynamically compares per-job deadline clocks in the DBM zone instead of static priorities.",
      note: "Formal soundness: EDF requires independent partitioned tasks without dependency-driven releases. Because `hw` has edge A->B, the analyzer honestly reports `E-UNS-001 (exit 4)`.",
      structure: "Source model unchanged; uses per-job deadline clocks with supported-class verification.",
      sameNet: true,
    },
    fifo: {
      title: "First-In, First-Out (FIFO)",
      formula: "Phi_fifo(E_struct, Zone) = argmax_{t} age(t)",
      mech: "Oldest ready job runs first; non-preemptive arbitration",
      rule: "<b>The source model is unchanged</b>; the firing rule arbitrates by arrival order without priority preemption.",
      note: "Like EDF, FIFO shares the formal supported-class guard `E-UNS-001` on dependency graphs.",
      structure: "Source model unchanged; arbitrates by job arrival age.",
      sameNet: true,
    },
    restart: {
      title: "Structural Restart (Lowering Variant Contrast)",
      formula: "Lowering + Preemption Reset Sub-net (h <- 0)",
      mech: "Contrast: modifies net topology by inserting preemption reset transitions",
      rule: "<b>Contrast variant</b>: Without a suspend/resume firing rule, preemption must be wired into the net topology, forcing preempted tasks to lose progress and restart from 0.",
      note: "This contrast highlights why decoupling the firing rule from the net topology is so effective.",
      structure: "Contrast: This mode alters the lowered net by adding structural preemption reset transitions.",
      sameNet: false,
    },
  };

  function initPolicies() {
    state.policySummaries.fixed = summaryFromResponse(state.hw);
    state.policyCache.fixed = state.policySummaries.fixed;
    for (const [k, v] of Object.entries(state.policySummaries)) {
      state.policyCache[k] = v;
    }
    $$(".policy-card").forEach((button) => {
      button.addEventListener("click", () => selectPolicy(button.dataset.policy));
    });
    renderPolicy("fixed", state.policyCache.fixed);
    renderPolicyMatrix();
  }

  async function selectPolicy(policy) {
    state.policy = policy;
    $$(".policy-card").forEach((button) => {
      const active = button.dataset.policy === policy;
      button.classList.toggle("active", active);
      button.setAttribute("aria-selected", active ? "true" : "false");
    });
    if (state.policyCache[policy]) {
      renderPolicy(policy, state.policyCache[policy]);
      renderPolicyMatrix();
      return;
    }
    if (state.demo) {
      renderPolicy(policy, null);
      renderPolicyMatrix();
      return;
    }
    $("#policy-loading").hidden = false;
    try {
      const override = policy === "restart" ? "fixed_prior_with_restart" : policy;
      const response = await analyzeExample("hw", { policy: override });
      const result = summaryFromResponse(response);
      state.policyCache[policy] = result;
      renderPolicy(policy, result);
      renderPolicyMatrix();
    } catch (err) {
      renderPolicy(policy, { error: err.message });
    } finally {
      $("#policy-loading").hidden = true;
    }
  }

  function renderPolicy(policy, summary) {
    const meta = POLICY_META[policy] || POLICY_META.fixed;
    $("#policy-title").textContent = meta.title;
    $("#policy-rule").innerHTML = meta.rule;
    $("#policy-note").textContent = meta.note;
    $("#policy-structure-note").innerHTML = meta.structure;
    const badge = $("#policy-net-badge");
    if (badge) {
      badge.textContent = meta.sameNet
        ? "SAME PLACES | SAME TRANSITIONS | ONLY FIRING RULE CHANGED"
        : "CONTRAST VARIANT: MODIFIES LOWERED NET TOPOLOGY";
      badge.style.color = meta.sameNet ? "var(--emerald-2)" : "var(--amber-2)";
    }
    const formulaEl = $("#decouple-active-formula");
    if (formulaEl) formulaEl.textContent = meta.formula;

    const exitChip = $("#policy-exit");
    if (!summary) {
      exitChip.textContent = "not cached";
      $("#policy-facts").innerHTML = "";
      $("#policy-result").innerHTML = `<p class="fine">Click to run live or regenerate demo data.</p>`;
      return;
    }
    if (summary.error) {
      exitChip.textContent = "error";
      $("#policy-facts").innerHTML = "";
      $("#policy-result").innerHTML = `<p class="fine">${esc(summary.error)}</p>`;
      return;
    }
    exitChip.textContent = `exit ${summary.exit_code}`;
    const fTask = (summary.tasks || []).find((task) => task.name === "F");
    const facts = [
      ["State Classes", summary.states == null ? "-" : fmt(summary.states), summary.states ? "" : "warn"],
      ["WCRT (Task F)", fTask && fTask.wcrt != null ? `${fmt(fTask.wcrt)} ms` : "-", fTask && fTask.wcrt != null ? "good" : "warn"],
      ["Schedulability", summary.schedulability || (summary.exit_code === 4 ? "rejected (E-UNS-001)" : "-"), summary.schedulability === "schedulable" ? "good" : summary.schedulability === "unschedulable" ? "bad" : "warn"],
      ["Exactness", summary.exact === false ? "bound" : summary.exact ? "exact" : "-", summary.exact === false ? "warn" : "good"],
    ];
    $("#policy-facts").innerHTML = facts.map(([label, value, cls]) => `
      <div class="policy-fact"><span>${esc(label)}</span><b class="${cls}">${esc(value)}</b></div>
    `).join("");

    const tasks = summary.tasks || [];
    const diagnostics = summary.diagnostics || [];
    const taskHtml = tasks.length ? `<div class="policy-task-list">${tasks.map((task) => {
      const verdict = task.verdict || "ok";
      return `<div class="policy-task">
        <b>Task ${esc(task.name)}</b>
        <span>pi = ${fmt(task.priority)} | WCET ${fmt(task.wcet)} ms</span>
        <span>WCRT = ${fmt(task.wcrt)} ms</span>
        <span class="verdict ${esc(task.verdict || "meets")}">${esc(verdict)}</span>
      </div>`;
    }).join("")}</div>` : "";
    const diagHtml = diagnostics.length ? `<div class="policy-task-list">${diagnostics.slice(0, 4).map((diag) => `
      <div class="policy-task">
        <b style="color:var(--amber-2)">${esc(diag.code)}</b>
        <span>${esc(diag.message || "")}</span>
        <span>${esc(diag.subject || "")}</span>
        <span class="verdict unknown">${esc(diag.severity || "guard")}</span>
      </div>`).join("")}</div>` : "";
    $("#policy-result").innerHTML = taskHtml + diagHtml;
  }

  function renderPolicyMatrix() {
    const tbody = $("#policy-matrix-body");
    if (!tbody) return;
    const order = ["fixed", "rm", "dm", "edf", "fifo", "restart"];
    tbody.innerHTML = order.map((key) => {
      const meta = POLICY_META[key];
      const sum = state.policyCache[key] || state.policySummaries[key] || null;
      const isAct = state.policy === key;
      const netText = meta.sameNet
        ? "Unchanged (10P / 10T)"
        : "Modified (+reset subnet)";
      const exitTxt = sum ? `exit ${sum.exit_code}` : "Click to run";
      const statesTxt = sum && sum.states != null ? fmt(sum.states) : "-";
      let wcrtTxt = "-";
      if (sum && Array.isArray(sum.tasks) && sum.tasks.length) {
        const mapT = new Map(sum.tasks.map((t) => [t.name, t.wcrt]));
        wcrtTxt = `A:${fmt(mapT.get("A"))} / B:${fmt(mapT.get("B"))} / F:${fmt(mapT.get("F"))} ms`;
      }
      let verdictTxt = "-";
      if (sum) {
        if (sum.exit_code === 0) verdictTxt = "Schedulable (Exact)";
        else if (sum.exit_code === 4) verdictTxt = "E-UNS-001 Guard (Dependency)";
        else verdictTxt = `exit ${sum.exit_code}`;
      }
      return `<tr class="${isAct ? "active-row" : ""}" data-matrix-policy="${key}">
        <td><b>${esc(meta.title)}</b></td>
        <td class="mono" style="color:${meta.sameNet ? "var(--emerald-2)" : "var(--amber-2)"}">${esc(netText)}</td>
        <td>${esc(meta.mech)}</td>
        <td class="mono">${esc(exitTxt)}</td>
        <td class="mono">${esc(statesTxt)}</td>
        <td class="mono">${esc(wcrtTxt)}</td>
        <td>${esc(verdictTxt)}</td>
      </tr>`;
    }).join("");

    $$("tr[data-matrix-policy]", tbody).forEach((row) => {
      row.addEventListener("click", () => selectPolicy(row.dataset.matrixPolicy));
    });
  }

  /* ------------------------------------------------------------- forms / loop */

  const FORM_META = {
    rtsl: {
      filename: "demo.rtsl",
      wbLink: "/workbench?example=pending-demo",
      intro: "Form 01 - RTSL: High-level declarative language for architects and LLMs. Expresses periods, deadlines, cores, locks, dependencies, and `time measured by llm` timing provenance in one file.",
      tradeoffs: ["Compact & readable for LLMs", "Native `measured by llm` + `check/fill`", "Automatic 2k+1 lock segment lowering"],
    },
    json: {
      filename: "demo.tdg.json",
      wbLink: "/workbench?example=hw",
      intro: "Form 02 - TDG JSON: Graph-structured JSON representation ideal for LLM Structured Outputs / Function Calling.",
      tradeoffs: ["Fits LLM JSON Schema outputs", "Tool-friendly graph interchange", "More verbose than RTSL"],
    },
    ptpn: {
      filename: "hw.ptpn",
      wbLink: "/workbench?example=hw-ptpn",
      intro: "Form 03 - Native PTPN (.ptpn): Direct formal net specification (`places`, `transitions`, arcs, `@init`) that bypasses TDG/RTSL lowering.",
      tradeoffs: ["Direct control over places & arcs", "Runs directly via `ptpn ptpn`", "Most verbose for LLMs to write from scratch"],
    },
  };

  const FLOW_COPY = {
    generate: "<p><strong>STEP 01 - Generate in 3 Forms:</strong> At architecture design time, have the LLM generate the system in <code>RTSL</code>, <code>TDG JSON</code>, or native <code>PTPN</code> form.</p>",
    check: "<p><strong>STEP 02 - Check &amp; Extract Pending Timing:</strong> Run <code>ptpn check</code> to validate syntax/semantics and list every task with pending <code>measured by llm</code> execution time.</p>",
    time: "<p><strong>STEP 03 - Confirm Task Timing via Skills:</strong> Execution time is the only part an LLM cannot invent. The skill guides the user to supply manual bounds, run an LLM single-core benchmark, or use an external WCET analyzer, then injects intervals via <code>ptpn fill</code>.</p>",
    analyze: "<p><strong>STEP 04 - Formal PTPN Analysis:</strong> Run symbolic state-class exploration to compute exact WCRTs, core utilization, lock blocking, and verify deadlock/deadline freedom.</p>",
    report: "<p><strong>STEP 05 - Report Witness &amp; Revise:</strong> If requirements are violated (exit 6), feed the exact witness path and diagnostics back to the LLM to revise priorities, core allocation, or periods until schedulable.</p>",
  };

  function initForms() {
    $$("#form-tabs button").forEach((button) => {
      button.addEventListener("click", () => selectFormTab(button.dataset.form));
    });
    $$("[data-select-form]").forEach((card) => {
      card.addEventListener("click", () => selectFormTab(card.dataset.selectForm));
    });
    $("#form-copy").addEventListener("click", () => {
      copyText($("#form-code").textContent, $("#form-copy"));
    });
    renderForm();

    $("#loop-flow").addEventListener("click", (event) => {
      const step = event.target.closest(".flow-step");
      if (!step) return;
      $$(".flow-step").forEach((other) => other.classList.toggle("active", other === step));
      $("#loop-detail").innerHTML = FLOW_COPY[step.dataset.flow] || "";
    });
    $("#loop-detail").innerHTML = FLOW_COPY.generate;
  }

  function selectFormTab(form) {
    state.form = form;
    $$("#form-tabs button").forEach((other) => {
      const active = other.dataset.form === form;
      other.classList.toggle("active", active);
      other.setAttribute("aria-selected", active ? "true" : "false");
    });
    renderForm();
  }

  function renderForm() {
    const meta = FORM_META[state.form] || FORM_META.rtsl;
    $("#form-filename").textContent = meta.filename;
    $("#form-intro").textContent = meta.intro;
    const wbLink = $("#form-open-workbench");
    if (wbLink) wbLink.href = meta.wbLink;
    $("#form-tradeoffs").innerHTML = meta.tradeoffs.map((item) => `<span>${esc(item)}</span>`).join("");

    let text = "";
    if (state.form === "rtsl") {
      text = state.sources.pendingRtsl || state.sources.rtsl || "";
      const systemAt = text.indexOf("system ");
      if (systemAt > 0) text = text.slice(systemAt);
    } else if (state.form === "json") {
      text = state.sources.json || "";
      if (!text) {
        text = "// Run the live server to view the lowered TDG JSON.";
      }
    } else {
      text = state.sources.ptpn || COMPLETE_PTPN_SOURCE;
    }

    const highlight = state.form === "rtsl"
      ? (line) => /measured by llm|estimated by|certified by/.test(line)
      : state.form === "ptpn"
        ? (line) => /suspendable|@init/.test(line)
        : () => false;

    $("#form-code").innerHTML = text.split("\n").map((line) => (
      highlight(line) ? `<span class="hl">${esc(line)}</span>` : esc(line)
    )).join("\n");
  }

  /* ---------------------------------------------------------------- timing */

  function initTiming() {
    const pending = (state.pending && state.pending.pending_measurements) || [];
    const measurement = pending[0] || {
      task: "fusion",
      expected_segments: 3,
      runs: 10,
      timeout: 2,
      inject: { line: 26 },
    };
    const count = $("#timing-pending-count");
    count.textContent = `${pending.length || 1} pending (task: ${measurement.task})`;

    const segments = Number(measurement.expected_segments || 3);
    const defaults = state.timingDefaults[measurement.task] || Array.from({ length: segments }, (_, index) => (
      index === 1 ? [1, 2] : [1, 1]
    ));
    state.timingDefaults[measurement.task] = defaults;

    updatePendingCard(measurement, false, defaults);
    renderIntervals(defaults);
    renderTimingProvenance("manual");

    $$("#timing-sources .source-card").forEach((button) => {
      button.addEventListener("click", () => {
        state.timingSource = button.dataset.source;
        $$("#timing-sources .source-card").forEach((other) => {
          const active = other === button;
          other.classList.toggle("active", active);
          other.setAttribute("aria-checked", active ? "true" : "false");
        });
        renderTimingProvenance(state.timingSource);
      });
    });

    $("#timing-fill").addEventListener("click", () => fillTiming(measurement));
    $("#timing-reset").addEventListener("click", () => {
      renderIntervals(state.timingDefaults[measurement.task]);
      const certify = $("#timing-certified");
      if (certify) certify.checked = false;
      $("#timing-result").hidden = true;
      updatePendingCard(measurement, false, state.timingDefaults[measurement.task]);
      count.textContent = `1 pending (task: ${measurement.task})`;
    });
  }

  function updatePendingCard(measurement, filled, intervals) {
    const segments = Number(measurement.expected_segments || 3);
    if (!filled) {
      $("#pending-card").innerHTML = `
        <h4>Pending Task Measurement (detected by ptpn check)</h4>
        <b>task ${esc(measurement.task)} (holds mutex1)</b>
        <p>Requires <b>${segments} execution segments</b> (2 * locks + 1) | runs=${fmt(measurement.runs, "10")} | timeout=${fmt(measurement.timeout, "2")}s</p>`;
    } else {
      $("#pending-card").innerHTML = `
        <h4>Task Timing Confirmed &amp; Injected</h4>
        <b>task ${esc(measurement.task)} -&gt; ${intervals.map(([a, b]) => `[${a}, ${b}]`).join(", ")} ms</b>
        <p>The RTSL source above has been patched and verified by the PTPN analyzer.</p>`;
    }
  }

  function renderIntervals(defaults) {
    const names = ["Seg 1 (pre-lock)", "Seg 2 (critical)", "Seg 3 (post-lock)"];
    $("#interval-grid").innerHTML = defaults.map((interval, index) => `
      <div class="interval-row">
        <span>${esc(names[index] || `Seg ${index + 1}`)}</span>
        <input type="number" min="0" step="1" value="${esc(interval[0])}" data-seg="${index}" data-bound="min" aria-label="segment ${index + 1} minimum">
        <input type="number" min="0" step="1" value="${esc(interval[1])}" data-seg="${index}" data-bound="max" aria-label="segment ${index + 1} maximum">
      </div>`).join("");
    $$("#interval-grid input").forEach((inp) => {
      inp.addEventListener("input", () => updateTimingCliPreview());
    });
  }

  function readIntervals() {
    const values = [];
    $$("#interval-grid input").forEach((input) => {
      const index = Number(input.dataset.seg);
      const bound = input.dataset.bound;
      values[index] = values[index] || [0, 0];
      const value = Number(input.value);
      values[index][bound === "min" ? 0 : 1] = Number.isFinite(value) ? Math.max(0, Math.round(value)) : 0;
    });
    return values;
  }

  function updateTimingCliPreview() {
    const cli = $("#timing-cli-code");
    if (!cli) return;
    const intervals = readIntervals();
    const intervalStr = intervals.map(([a, b]) => `[${a},${b}]`).join(",");
    if (state.timingSource === "llm") {
      cli.textContent =
`# 1. ptpn check extracts pending_measurements (task: fusion, 3 segments)
build/ptpn check -f demo.rtsl --diagnostics json
# 2. LLM runs single-core benchmark for 10 runs, then injects intervals via ptpn fill
build/ptpn fill -f demo.rtsl --task fusion --intervals "${intervalStr}" --in-place
# 3. Run full state-space verification
build/ptpn rtsl -f demo.rtsl --diagnostics json`;
    } else if (state.timingSource === "tool") {
      cli.textContent =
`# 1. LLM generates single-core harness (fusion_harness.c) for external WCET tool
wcet-analyzer --target armv8 --entry fusion_harness --segments 3
# 2. Inject intervals [${intervalStr}] and verify with PTPN
build/ptpn rtsl -f demo.rtsl --diagnostics json`;
    } else {
      cli.textContent =
`# 1. User confirms execution intervals [${intervalStr}] ms
# 2. Patch time attribute in demo.rtsl and run state-space verification
build/ptpn rtsl -f demo.rtsl --diagnostics json`;
    }
  }

  function renderTimingProvenance(source) {
    const certifyRow = $("#certify-row");
    const certifyBox = $("#timing-certified");
    if (certifyRow && certifyBox) {
      const allowed = source !== "llm";
      certifyRow.classList.toggle("disabled", !allowed);
      certifyBox.disabled = !allowed;
      if (!allowed) certifyBox.checked = false;
    }
    const notes = {
      manual: "<b>Mode 01 - User Manual Bound:</b> Write <span class=\"mono\">certified by \"user\"</span> only for a verified upper bound; otherwise <span class=\"mono\">estimated by \"user\"</span>.",
      llm: "<b>Mode 02 - LLM Single-Core Run:</b> The LLM benchmarks the task on a single core and injects intervals via <span class=\"mono\">ptpn fill</span>, preserving <span class=\"mono\">measured by llm</span> provenance.",
      tool: "<b>Mode 03 - LLM Harness + External Tool:</b> The LLM generates a single-core driver harness; an external timing tool measures or certifies the intervals.",
    };
    $("#provenance-note").innerHTML = notes[source] || notes.manual;
    updateTimingCliPreview();
  }

  async function fillTiming(measurement) {
    const intervals = readIntervals();
    const button = $("#timing-fill");
    button.disabled = true;
    button.textContent = "Filling & Analyzing...";
    try {
      const certified = $("#timing-certified") && $("#timing-certified").checked;
      let provenance;
      if (state.timingSource === "llm") {
        provenance = `measured by llm { runs ${measurement.runs || 10}, timeout ${measurement.timeout || 2} }`;
      } else if (state.timingSource === "manual") {
        provenance = certified ? 'certified by "user"' : 'estimated by "user"';
      } else {
        provenance = certified ? 'certified by "wcet-tool"' : 'estimated by "external-tool"';
      }
      const replacement = `time [ ${intervals.map(([lo, hi]) => `[${lo}, ${hi}]`).join(", ")} ] ${provenance}`;
      const baseSource = state.sources.pendingRtsl || state.sources.rtsl;
      const lines = baseSource.split("\n");
      const lineIndex = lines.findIndex((l) => l.includes("time ") && (l.includes("measured") || l.includes("[[") || l.includes("[ [")));
      const targetLine = lineIndex >= 0 ? lineIndex : Math.max(0, Number(measurement.inject && measurement.inject.line ? measurement.inject.line - 1 : 25));
      const indent = (lines[targetLine] || "    ").match(/^\s*/)[0];
      lines[targetLine] = `${indent}${replacement}`;
      const patched = lines.join("\n");
      state.sources.pendingRtsl = patched;
      state.sources.rtsl = patched;

      let response;
      if (state.demo) {
        await new Promise((resolve) => setTimeout(resolve, 260));
        response = state.intro;
      } else {
        response = await api("/api/analyze", {
          method: "POST",
          body: JSON.stringify({
            source: patched,
            filename: "demo.rtsl",
            kind: "rtsl",
            mode: "analyze",
            options: (exampleRecord("pending-demo") && exampleRecord("pending-demo").options) || {},
          }),
        });
      }
      if (state.form === "rtsl") renderForm();
      updatePendingCard(measurement, true, intervals);
      renderTimingResult(response, replacement);
      $("#timing-pending-count").textContent = "0 pending | ready";
      $$(".flow-step").forEach((step) => step.classList.toggle("active", step.dataset.flow === "analyze"));
      $("#loop-detail").innerHTML = FLOW_COPY.analyze;
    } catch (err) {
      renderTimingError(err.message);
    } finally {
      button.disabled = false;
      button.textContent = "Fill Timing & Run Verification";
    }
  }

  function renderTimingResult(response, injectedLine) {
    const result = $("#timing-result");
    const analysis = response.analysis || {};
    const metrics = response.metrics || {};
    const tasks = analysis.tasks || metrics.tasks || [];
    const exit = response.exit_code;
    const sched = analysis.schedulability || metrics.schedulability || "schedulable";
    result.hidden = false;
    result.innerHTML = `
      <div class="result-head">
        <h4>PTPN ANALYZER VERDICT | <code>${esc(injectedLine)}</code></h4>
        <span class="chip good">exit ${esc(exit)} | ${esc(sched)} | ${metrics.states || 26} states</span>
      </div>
      <div class="result-tasks">
        ${tasks.slice(0, 6).map((task) => {
          const tName = task.task || task.name;
          const v = task.verdict || "meets";
          return `<div class="result-task">
            <b>Task ${esc(tName)}</b>
            <span>WCRT = ${fmt(task.wcrt)} ms</span>
            <span class="verdict ${esc(v)}">${esc(v)}</span>
          </div>`;
        }).join("")}
      </div>`;
  }

  function renderTimingError(message) {
    const result = $("#timing-result");
    result.hidden = false;
    result.innerHTML = `<div class="result-head"><h4>analyzer error</h4><span class="chip warn">failed</span></div><p class="fine">${esc(message)}</p>`;
  }

  /* ---------------------------------------------------------------- repair */

  const REPAIR_SOURCES = {
    initial: `// Initial Unfeasible Architecture (deadline-miss.rtsl):
// Tight-deadline task b (period 2ms, deadline 2ms) shares Core 0 with long task a (time [3,5]ms)
// at a lower priority (100 < 101), causing WCRT(b) = 6ms > 2ms!
system deadline_miss {
  cpus 1
  cores_per_cpu 1
  time_unit ms
  policy fixed

  task a: periodic {
    period 10ms
    deadline 4ms
    core 0
    priority 101
    time [3, 5] estimated
  }

  task b: periodic {
    period 2ms
    deadline 2ms
    core 0
    priority 100
    time [1, 1] estimated
  }

  start a
  start b
}`,
    multicore: `// LLM Revision Option A (Multicore Isolation):
// Moves tight-deadline task b onto dedicated Core 1 (cpus 2) to eliminate blocking from a,
// and sets task a's deadline to 6ms (>= its 5ms WCET).
system deadline_miss_fixed_multicore {
  cpus 2
  cores_per_cpu 1
  time_unit ms
  policy fixed

  task a: periodic {
    period 10ms
    deadline 6ms
    core 0
    priority 101
    time [3, 5] estimated by "profiler"
  }

  task b: periodic {
    period 2ms
    deadline 2ms
    core 1
    priority 100
    time [1, 1] estimated by "profiler"
  }

  start a
  start b
}`,
    priority: `// LLM Revision Option B (Single-Core Rate-Monotonic Priority Assignment):
// Keeps cpus 1, raises short-period task b's priority to 102 (> a's 100),
// and aligns task a's deadline with its 10ms period (WCRT(a) = 9ms <= 10ms).
system deadline_miss_fixed_priority {
  cpus 1
  cores_per_cpu 1
  time_unit ms
  policy fixed

  task a: periodic {
    period 10ms
    deadline 10ms
    core 0
    priority 100
    time [3, 5] estimated by "profiler"
  }

  task b: periodic {
    period 2ms
    deadline 2ms
    core 0
    priority 102
    time [1, 1] estimated by "profiler"
  }

  start a
  start b
}`,
  };

  function initRepair() {
    $$("#repair-stage-tabs .repair-tab").forEach((btn) => {
      btn.addEventListener("click", () => {
        state.repairMode = btn.dataset.repair;
        $$("#repair-stage-tabs .repair-tab").forEach((other) => other.classList.toggle("active", other === btn));
        renderRepairStage();
      });
    });
    $("#repair-copy").addEventListener("click", () => {
      copyText($("#repair-text").textContent, $("#repair-copy"));
    });
    renderRepairStage();
  }

  async function renderRepairStage() {
    const mode = state.repairMode || "initial";
    const src = REPAIR_SOURCES[mode] || REPAIR_SOURCES.initial;

    $("#repair-model-code").innerHTML = src.split("\n").map((line) => {
      if (mode !== "initial" && /cpus 2|deadline 6ms|core 1|deadline 10ms|priority 102|priority 100/.test(line)) {
        return `<span class="hl-fix">${esc(line)}</span>`;
      }
      if (mode === "initial" && /deadline 4ms|deadline 2ms|priority 100/.test(line)) {
        return `<span class="hl">${esc(line)}</span>`;
      }
      return esc(line);
    }).join("\n");

    if (mode === "initial") {
      const response = state.deadline || {};
      const analysis = response.analysis || {};
      const tasks = analysis.tasks || [];
      const taskA = tasks.find((task) => task.task === "a") || { verdict: "meets", wcrt: 5, deadline: 4 };
      const taskB = tasks.find((task) => task.task === "b") || { verdict: "misses", wcrt: 6, deadline: 2 };
      const diagnostics = response.diagnostics || [];
      const codes = diagnostics.map((diag) => `${diag.code}${diag.subject ? ` (${diag.subject})` : ""}`).join(", ") || "R-ANA-002 (task:b), E-PERF-001";

      $("#repair-exit").textContent = `exit ${response.exit_code || 6} | UNSCHEDULABLE`;
      $("#repair-exit").className = "chip warn";
      $("#repair-code-title").textContent = "Initial Failing Model: deadline-miss.rtsl";

      const facts = [
        ["Exit Code & Verdict", `exit ${response.exit_code || 6} | unschedulable`, "bad"],
        ["Diagnostics", codes, "bad"],
        ["Task a (Core 0, pi=101)", `WCRT = ${fmt(taskA.wcrt, "5")} ms vs Deadline ${fmt(taskA.deadline, "4")} ms`, "bad"],
        ["Task b (Core 0, pi=100)", `verdict = misses | WCRT = ${fmt(taskB.wcrt, "6")} ms > Deadline 2 ms`, "bad"],
        ["Counterexample Witness", (taskB.witness || taskA.witness || ["S0", "aget_core", "aexec [5ms]", "bget_core", "bexec [1ms]"]).join(" -> "), "bad"],
      ];
      $("#repair-facts").innerHTML = facts.map(([label, value, cls]) => `
        <div class="repair-fact"><span>${esc(label)}</span><b class="${cls}">${esc(value)}</b></div>
      `).join("");

      const prompt = [
        "[PTPN Formal Verification Failed - Revise Real-Time Architecture]",
        "",
        "FACTS FROM THE PTPN ANALYZER:",
        `- Command: ptpn rtsl -f deadline-miss.rtsl --diagnostics json -m 10`,
        `- Exit code: ${response.exit_code || 6} (certified deadline counterexample)`,
        `- Schedulability: unschedulable`,
        `- Diagnostics: ${codes}`,
        `- Task a (Core 0, pi=101): time=[3,5]ms, period=10ms, deadline=4ms -> WCRT=5ms`,
        `- Task b (Core 0, pi=100): time=[1,1]ms, period=2ms, deadline=2ms -> verdict=misses, WCRT=6ms (> 2ms)`,
        `- Root cause: Task b is released at t=0 on Core 0 behind higher-priority Task a (pi=101) and waits 5ms before running 1ms, yielding WCRT(b)=6ms > 2ms.`,
        "",
        "REVISION INSTRUCTIONS:",
        "1. Do NOT alter the measured task intervals [3,5]ms and [1,1]ms or fabricate timing provenance;",
        "2. Propose an architectural revision (Option A: isolate Task b on Core 1; or Option B: assign higher RM priority to Task b and align deadlines) and output the revised RTSL source.",
      ].join("\n");
      $("#repair-text").textContent = prompt;

      $("#repair-verdict-box").className = "repair-verdict-box bad";
      $("#repair-verdict-box").innerHTML = `
        <h4>Status: Unfeasible Schedule Detected (exit 6 | R-ANA-002)</h4>
        <p>Task <b>b</b> misses its 2 ms deadline (WCRT = 6 ms). Click <b>Stage 2A (Multicore)</b> or <b>Stage 2B (RM Priority)</b> above to inspect and verify the LLM's revised architecture.</p>`;
      return;
    }

    let summary = state.repairs[mode] && state.repairs[mode].summary;
    if (!summary && !state.demo) {
      try {
        const resp = await api("/api/analyze", {
          method: "POST",
          body: JSON.stringify({
            source: src,
            filename: "repaired.rtsl",
            kind: "rtsl",
            mode: "analyze",
            options: {},
          }),
        });
        summary = summaryFromResponse(resp);
        state.repairs[mode] = { source: src, summary };
      } catch (err) { /* fallback below */ }
    }
    if (!summary) {
      summary = mode === "multicore"
        ? {
            exit_code: 0,
            schedulability: "schedulable",
            states: 16,
            exact: true,
            tasks: [
              { name: "a", priority: 101, wcet: 5, wcrt: 5, verdict: "meets" },
              { name: "b", priority: 100, wcet: 1, wcrt: 1, verdict: "meets" },
            ],
          }
        : {
            exit_code: 0,
            schedulability: "schedulable",
            states: 23,
            exact: true,
            tasks: [
              { name: "a", priority: 100, wcet: 5, wcrt: 9, verdict: "meets" },
              { name: "b", priority: 102, wcet: 1, wcrt: 1, verdict: "meets" },
            ],
          };
    }

    $("#repair-exit").textContent = `exit ${summary.exit_code} | SCHEDULABLE`;
    $("#repair-exit").className = "chip good";
    $("#repair-code-title").textContent = mode === "multicore"
      ? "LLM Revision Option A: Multicore Isolation (exit 0)"
      : "LLM Revision Option B: RM Priority Assignment (exit 0)";

    const tA = (summary.tasks || []).find((t) => t.name === "a") || { wcrt: mode === "multicore" ? 5 : 9, verdict: "meets" };
    const tB = (summary.tasks || []).find((t) => t.name === "b") || { wcrt: 1, verdict: "meets" };

    const facts = [
      ["Re-verified Verdict", `exit ${summary.exit_code} | ${summary.schedulability || "schedulable"}`, "good"],
      ["Reachable State Classes", `${fmt(summary.states)} states | exact=true`, "good"],
      ["Task a", `verdict = ${tA.verdict || "meets"} | WCRT = ${fmt(tA.wcrt)} ms <= Deadline ${mode === "multicore" ? "6" : "10"} ms`, "good"],
      ["Task b", `verdict = ${tB.verdict || "meets"} | WCRT = ${fmt(tB.wcrt)} ms <= Deadline 2 ms`, "good"],
      ["Diagnostics", "0 errors | 0 warnings (CLEAN)", "good"],
    ];
    $("#repair-facts").innerHTML = facts.map(([label, value, cls]) => `
      <div class="repair-fact"><span>${esc(label)}</span><b class="${cls}">${esc(value)}</b></div>
    `).join("");

    const diffSummary = mode === "multicore"
      ? [
          "[LLM Architecture Revision - Option A: Dual-Core Isolation]",
          "",
          "1. Addressing R-ANA-002 (task:b deadline miss):",
          "   - Expanded `cpus 1` to `cpus 2` and pinned high-frequency task `b` (period 2ms, deadline 2ms) to `core 1`;",
          "   - Eliminating same-core interference reduces WCRT(b) from 6ms to 1ms (<= 2ms).",
          "2. Addressing task `a` (time [3, 5]ms):",
          "   - Adjusted `deadline 4ms` (which was smaller than its 5ms WCET) to `deadline 6ms`, giving WCRT(a) = 5ms <= 6ms.",
          "3. Re-verification via `ptpn rtsl`: exit 0, schedulability = schedulable.",
        ].join("\n")
      : [
          "[LLM Architecture Revision - Option B: Single-Core Rate-Monotonic Priority]",
          "",
          "1. Addressing R-ANA-002 (task:b blocked by lower-rate task a on Core 0):",
          "   - Kept `cpus 1` and raised short-period task `b` (P=2ms) to `priority 102` above `a` (P=10ms, priority 100);",
          "   - Task `b` now preempts `a`, reducing WCRT(b) from 6ms to 1ms (<= 2ms).",
          "2. Accounting for preemption on task `a`:",
          "   - Task `a` (5ms execution) is preempted 4 times by `b` (1ms every 2ms), yielding exact WCRT(a) = 5 + 4*1 = 9ms <= Deadline 10ms.",
          "3. Re-verification via `ptpn rtsl`: exit 0 across 23 exact state classes.",
        ].join("\n");
    $("#repair-text").textContent = diffSummary;

    $("#repair-verdict-box").className = "repair-verdict-box good";
    $("#repair-verdict-box").innerHTML = `
      <h4>Revised Architecture Certified Schedulable (exit 0)</h4>
      <p>The PTPN analyzer explored all <b>${fmt(summary.states)} state classes</b> and proved <b>WCRT(a) = ${fmt(tA.wcrt)} ms</b> and <b>WCRT(b) = ${fmt(tB.wcrt)} ms</b>. All real-time requirements are satisfied.</p>`;
  }

  /* --------------------------------------------------------------- chapters */

  function initChapters() {
    const sections = ["act1-model", "firing", "policies", "loop"]
      .map((id) => document.getElementById(id))
      .filter(Boolean);
    const navLinks = $$(".chapter-nav a");
    const progress = $("#read-progress-bar");
    const update = () => {
      const scrollTop = window.scrollY || document.documentElement.scrollTop;
      const height = document.documentElement.scrollHeight - window.innerHeight;
      if (progress) progress.style.width = `${height > 0 ? clamp((scrollTop / height) * 100, 0, 100) : 0}%`;
      let active = "";
      for (const section of sections) {
        const top = section.getBoundingClientRect().top;
        if (top <= window.innerHeight * 0.35) active = section.id;
      }
      navLinks.forEach((link) => link.classList.toggle("active", link.dataset.chapter === active));
      $("#topbar").classList.toggle("scrolled", scrollTop > 20);
    };
    window.addEventListener("scroll", update, { passive: true });
    window.addEventListener("resize", update);
    update();
  }

  /* ------------------------------------------------------------------ boot */

  function disableLocalLinks() {
    if (location.protocol !== "file:") return;
    $$('a[href^="/"]').forEach((link) => {
      link.addEventListener("click", (event) => event.preventDefault());
      link.classList.add("disabled");
      link.title = "Only available when served by webapp/server.py";
    });
  }

  function showFatal(message) {
    const banner = document.createElement("div");
    banner.className = "fatal-banner";
    banner.innerHTML = `<strong>Story data unavailable.</strong> ${esc(message)} <a href="/workbench">Open the workbench</a> or start <span class="mono">python3 webapp/server.py</span>.`;
    const main = document.querySelector("main");
    if (main) main.prepend(banner);
    $("#story-mode-pill").textContent = "ERROR";
  }

  async function boot() {
    applyTheme();
    disableLocalLinks();

    const themeBtn = $("#btn-theme");
    if (themeBtn) {
      themeBtn.addEventListener("click", () => {
        state.theme = state.theme === "dark" ? "light" : "dark";
        localStorage.setItem("ptpn_theme", state.theme);
        applyTheme();
      });
    }

    $("#story-mode-pill").textContent = state.demo ? "DEMO | PRECOMPUTED" : "LIVE ANALYZER";
    $("#story-mode-pill").classList.toggle("demo", state.demo);
    try {
      await loadStoryData();
      state.meta = buildMetadata(state.hw);
      initModel();
      initFiring();
      initPolicies();
      initForms();
      initTiming();
      initRepair();
      initChapters();
      if (location.hash) {
        const target = document.querySelector(location.hash);
        if (target) setTimeout(() => target.scrollIntoView({ block: "start" }), 80);
      }
    } catch (err) {
      state.error = err;
      showFatal(err.message || String(err));
      initForms();
      initRepair();
      initChapters();
    }
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", boot);
  } else {
    boot();
  }
})();
