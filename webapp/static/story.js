/* PTPN Chronograph — the guided story.
 *
 * The story is deliberately driven by the analyzer's own output: the SCG
 * nodes carry the marking, E_struct / E_pri / suspended sets and the DBM
 * clock bounds; the edges carry the firing window and the dwell interval.
 * No scheduling result is recomputed in the browser.
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
    examples: [],
    hw: null,
    pending: null,
    intro: null,
    deadline: null,
    sources: { rtsl: "", pendingRtsl: "", json: "", ptpn: "" },
    policySummaries: {},
    policyCache: {},
    policy: "fixed",
    trace: null,
    meta: null,
    step: 0,
    phase: "atState",
    time: 0,
    mode: "worst",
    lastFired: null,
    playing: false,
    playTimer: null,
    timeline: null,
    form: "rtsl",
    timingSource: "manual",
    timingDefaults: {},
    error: null,
  };

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

  function fmt(v, fallback = "—") {
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
      button.textContent = "copied";
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
      schedulability: analysis.schedulability,
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

  async function loadStoryData() {
    if (DEMO) {
      state.examples = DEMO_DATA.examples || [];
      state.hw = demoResponse("hw");
      state.pending = demoResponse("pending-demo");
      state.intro = demoResponse("intro-demo");
      state.deadline = demoResponse("deadline-miss");
      state.policySummaries = (DEMO_DATA.story && DEMO_DATA.story.policies) || {};
      const pendingRecord = findDemoRecord("pending-demo");
      const filledRecord = findDemoRecord("intro-demo");
      state.sources.pendingRtsl = pendingRecord ? pendingRecord.source : "";
      state.sources.rtsl = filledRecord ? filledRecord.source : state.sources.pendingRtsl;
      $("#story-generated").textContent = `precomputed ${DEMO_DATA.generated_at || "demo"}`;
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
      state.sources.pendingRtsl = pendingRecord ? pendingRecord.source : "";
      state.sources.rtsl = filledRecord ? filledRecord.source : state.sources.pendingRtsl;
      $("#story-generated").textContent = "live analyzer";
    }

    if (!state.hw || !state.hw.graphs || !state.hw.graphs.scg) {
      throw new Error("The hw example did not return a state-class graph.");
    }
    state.sources.json = state.intro && state.intro.tdg ? JSON.stringify(state.intro.tdg, null, 2) : "";
    state.sources.ptpn = PTPN_SOURCE;
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
    return (state.meta && (state.meta.nodes.get(name) || state.meta.nodes.get(name))) || null;
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
      if (markingHas(target.detail && target.detail.marking, "Fexit")) break;
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
    A_period: { x: 80, y: 45, label: "A_period" },
    Aentry: { x: 80, y: 130, label: "Aentry" },
    Aready: { x: 300, y: 130, label: "Aready" },
    Aexit: { x: 540, y: 130, label: "Aexit" },
    Bentry: { x: 700, y: 280, label: "Bentry" },
    Bready: { x: 880, y: 280, label: "Bready" },
    Bexit: { x: 1080, y: 280, label: "Bexit" },
    Fentry: { x: 80, y: 440, label: "Fentry" },
    Fready: { x: 290, y: 440, label: "Fready" },
    Fexit: { x: 540, y: 440, label: "Fexit" },
  };

  const TRANSITIONS = {
    A_fire: { x: 200, y: 45, label: "A_fire", kind: "period" },
    Aget_core: { x: 190, y: 130, label: "Aget_core", kind: "acquire" },
    Aexec: { x: 420, y: 130, label: "Aexec", kind: "exec" },
    A_to_B: { x: 620, y: 205, label: "A_to_B", kind: "control" },
    Bget_core: { x: 790, y: 280, label: "Bget_core", kind: "acquire" },
    Bexec: { x: 980, y: 280, label: "Bexec", kind: "exec" },
    B_consume: { x: 960, y: 370, label: "B_consume", kind: "control" },
    Fget_core: { x: 180, y: 440, label: "Fget_core", kind: "acquire" },
    Fexec: { x: 410, y: 440, label: "Fexec", kind: "exec" },
    F_consume: { x: 660, y: 440, label: "F_consume", kind: "control" },
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

  function nodePoint(kind, name) {
    return kind === "place" ? PLACES[name] : TRANSITIONS[name];
  }

  function rolesFor(name, kind) {
    const roles = [];
    if (kind === "place") roles.push("chain");
    else roles.push("chain");
    const task = taskOf(name);
    if (task) roles.push(`task-${task.toLowerCase()}`);
    if (name === "A_period" || name === "A_fire") roles.push("period");
    if (name === "A_to_B") roles.push("dependency");
    if (name === "B_consume" || name === "F_consume") roles.push("binding");
    if (/get_core/.test(name)) roles.push("core");
    if (isExecName(name)) roles.push("exec");
    return roles.join(" ");
  }

  function transitionMeta(name) {
    const node = nodeMeta(name);
    const interval = node && Array.isArray(node.interval) ? node.interval : null;
    const priority = node && isNum(node.priority) ? node.priority : (TRANSITIONS[name] ? null : null);
    const core = node && isNum(node.core) ? node.core : null;
    const bits = [];
    if (interval) bits.push(`[${interval[0]},${interval[1]}]`);
    if (isNum(priority)) bits.push(`p${priority}`);
    if (isNum(core)) bits.push(`c${core}`);
    return bits.join(" · ");
  }

  function renderNet(container, options = {}) {
    if (!container) return;
    const svg = svgEl("svg", {
      class: "net-svg",
      viewBox: "0 0 1120 620",
      role: "img",
      "aria-label": options.label || "PTPN structure",
    });
    const markerId = `arrow-${container.id || "net"}`;
    const defs = svgEl("defs");
    const marker = svgEl("marker", {
      id: markerId, viewBox: "0 0 10 10", refX: 9, refY: 5,
      markerWidth: 6, markerHeight: 6, orient: "auto-start-reverse",
    });
    marker.appendChild(svgEl("path", { d: "M 0 0 L 10 5 L 0 10 z", fill: "#3b464d" }));
    defs.appendChild(marker);
    svg.appendChild(defs);

    const arcsLayer = svgEl("g", { class: "arcs" });
    const placesLayer = svgEl("g", { class: "places" });
    const transitionsLayer = svgEl("g", { class: "transitions" });
    const tokensLayer = svgEl("g", { class: "tokens" });
    const labelsLayer = svgEl("g", { class: "labels" });
    const coreLayer = svgEl("g", { class: "core" });

    const marking = parseMarking(options.marking || "");
    const activeNames = new Set(options.active || []);
    const suspendedNames = new Set(options.suspended || []);
    const firedName = options.fired || null;

    for (const [from, to] of ARCS) {
      const fromPlace = Object.prototype.hasOwnProperty.call(PLACES, from);
      const toPlace = Object.prototype.hasOwnProperty.call(PLACES, to);
      const a = fromPlace ? PLACES[from] : TRANSITIONS[from];
      const b = toPlace ? PLACES[to] : TRANSITIONS[to];
      if (!a || !b) continue;
      const roles = rolesFor(from, fromPlace ? "place" : "transition");
      const path = svgEl("path", {
        class: "arc",
        d: `M ${a.x} ${a.y} L ${b.x} ${b.y}`,
        "marker-end": `url(#${markerId})`,
        "data-roles": roles,
      });
      arcsLayer.appendChild(path);
    }

    // The periodic release transition puts a token back into its period place.
    const selfLoop = svgEl("path", {
      class: "arc",
      d: "M 168 24 C 150 -6, 112 -6, 94 24",
      "marker-end": `url(#${markerId})`,
      "data-roles": "chain period",
    });
    arcsLayer.appendChild(selfLoop);

    for (const [name, place] of Object.entries(PLACES)) {
      const marked = marking.some((item) => item.name === name);
      const group = svgEl("g", { "data-name": name, "data-roles": rolesFor(name, "place") });
      group.appendChild(svgEl("circle", {
        class: `place-node${marked ? " marked" : ""}`,
        cx: place.x, cy: place.y, r: 15,
      }));
      group.appendChild(svgEl("text", {
        class: `place-label${marked ? " marked" : ""}`,
        x: place.x, y: place.y + 30,
      }, place.label));
      placesLayer.appendChild(group);
    }

    for (const [name, transition] of Object.entries(TRANSITIONS)) {
      const kind = kindOf(name);
      const isActive = activeNames.has(name);
      const isSuspended = suspendedNames.has(name);
      const isFired = firedName === name;
      const classes = ["transition-box", kind];
      if (isActive) classes.push("active");
      if (isSuspended) classes.push("suspended");
      if (isFired) classes.push("firing");
      const group = svgEl("g", {
        "data-name": name,
        "data-tid": transitionRefFor(name) || "",
        "data-roles": rolesFor(name, "transition"),
      });
      group.appendChild(svgEl("rect", {
        class: classes.join(" "),
        x: transition.x - 45, y: transition.y - 21, width: 90, height: 42, rx: 7,
      }));
      group.appendChild(svgEl("text", {
        class: "transition-name", x: transition.x, y: transition.y - 2,
      }, transition.label));
      group.appendChild(svgEl("text", {
        class: "transition-meta", x: transition.x, y: transition.y + 11,
      }, transitionMeta(name)));
      group.appendChild(svgEl("title", {}, `${name}${transitionMeta(name) ? " · " + transitionMeta(name) : ""}`));
      transitionsLayer.appendChild(group);
    }

    for (const item of marking) {
      const place = PLACES[item.name];
      if (!place) continue;
      const token = svgEl("circle", {
        class: "token-dot",
        cx: place.x, cy: place.y, r: 5.5,
      });
      token.style.animation = "tokenIn .3s ease";
      tokensLayer.appendChild(token);
      if (item.count > 1) {
        tokensLayer.appendChild(svgEl("text", {
          x: place.x + 12, y: place.y - 10,
          fill: "#e3ad72", "font-family": "var(--mono)", "font-size": "9px",
        }, String(item.count)));
      }
    }

    labelsLayer.appendChild(svgEl("text", { class: "lane-label", x: 20, y: 112 }, "A"));
    labelsLayer.appendChild(svgEl("text", { class: "lane-label", x: 20, y: 262 }, "B"));
    labelsLayer.appendChild(svgEl("text", { class: "lane-label", x: 20, y: 422 }, "F"));
    svg.appendChild(arcsLayer);
    svg.appendChild(placesLayer);
    svg.appendChild(transitionsLayer);
    svg.appendChild(tokensLayer);
    svg.appendChild(labelsLayer);

    if (options.showCore !== false) {
      coreLayer.appendChild(svgEl("rect", {
        class: "core-bar", x: 80, y: 545, width: 1000, height: 44, rx: 8,
        "data-roles": "core",
      }));
      coreLayer.appendChild(svgEl("text", { class: "core-text", x: 96, y: 563 }, "CORE 0"));
      coreLayer.appendChild(svgEl("text", { class: "core-text", x: 96, y: 578 }, "one task at a time"));
      coreLayer.appendChild(svgEl("text", { class: "core-priority", x: 1060, y: 563, "text-anchor": "end" }, "B 98  >  A 97  >  F 96"));
      coreLayer.appendChild(svgEl("text", { class: "core-text", x: 1060, y: 578, "text-anchor": "end" }, options.coreStatus || "priority filter active"));
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

    let selected = null;
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
      "Each task vertex becomes an alternating chain: entry → get_core → ready → exec → exit.",
      "Dependencies become bridge transitions. Aexit → A_to_B[1,1] → Bentry carries the delay.",
      "Start, periodic and end bindings add the initial token, the release transition and the sink consumer.",
      "Under the resume policy, preemption is not a sub-net: execution transitions are suspendable and the firing rule freezes them.",
      "Core and priority are transition attributes. A lock would add a resource place; this example has none.",
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
    window.addEventListener("resize", () => {
      if (state.trace) renderTimeline();
    });
    document.addEventListener("keydown", (event) => {
      const tag = event.target && event.target.tagName;
      if (tag === "INPUT" || tag === "TEXTAREA" || event.target.isContentEditable) return;
      if (event.key === "ArrowRight" || event.key === " ") {
        if (document.activeElement && document.activeElement.closest("#firing")) {
          event.preventDefault();
          stopPlay();
          next();
        }
      } else if (event.key === "ArrowLeft") {
        if (document.activeElement && document.activeElement.closest("#firing")) {
          event.preventDefault();
          stopPlay();
          prev();
        }
      }
    });
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
    const delay = state.phase === "elapsed" ? 900 : 1100;
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
      now.textContent = `t=${state.time}`;
    }
  }

  function animatePlayhead(start, end) {
    const total = state.trace.total || 1;
    setPlayhead(end / total);
    $("#firing-time").textContent = `t = ${end}`;
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
    $("#firing-time").textContent = `t = ${state.time}`;
    $("#firing-phase").textContent = final ? "Complete" : state.phase === "elapsed" ? "Time elapsed" : "At state";
    $("#firing-fired").textContent = firingName || "—";

    const activeNames = priority.map((ref) => ref.name);
    const suspendedNames = suspended.map((ref) => ref.name);
    const running = activeNames.find((name) => isExecName(name));
    const acquiring = activeNames.find((name) => /get_core/.test(name));
    renderNet($("#firing-net"), {
      marking,
      active: activeNames,
      suspended: suspendedNames,
      fired: firingName,
      label: `Marking at ${stateTitle}`,
      coreStatus: running
        ? `running: ${taskOf(running) || "—"}`
        : acquiring
          ? `dispatching: ${taskOf(acquiring) || "—"}`
          : "core idle",
    });

    renderSets(entry, priority, suspended, firingName);
    renderClocks(detail, step, suspendedNames);
    renderTimeline();

    if (state.phase === "elapsed" && step) {
      $("#firing-narration").innerHTML = `Time passes by <b>${step.dwell}</b> in ${stateTitle}. ${firingName ? `<b>${esc(firingName)}</b> reaches its firing window` : "the next transition is ready"}. The marking has not changed yet.`;
    } else if (final) {
      $("#firing-narration").innerHTML = `F reaches <b>Fexit</b> at <b>t = ${state.time}</b>. The representative run is complete; the state-class graph still represents every timing allowed by the intervals.`;
      $("#firing-verdict").hidden = false;
    } else {
      $("#firing-narration").innerHTML = narrationFor(node, step, entry, priority, suspended);
    }
    $("#firing-why").innerHTML = whyNow(entry, priority, suspended);

    $("#firing-dbm").textContent = detail.zone || "—";
    $("#sets-count").textContent = `${entry.length} / ${priority.length} / ${suspended.length}`;
    $("#firing-dots").innerHTML = state.trace.steps.map((_, index) => {
      const cls = index < state.step ? "done" : index === state.step ? "now" : "";
      return `<span class="step-dot ${cls}"></span>`;
    }).join("") + `<span class="step-dot ${state.step >= state.trace.steps.length ? "now" : ""}"></span>`;

    $("#fire-prev").disabled = state.step === 0 && state.phase === "atState";
    $("#fire-next").disabled = final && state.phase === "atState";

    if (state.trace.steps.length && !final) {
      $("#firing-verdict").hidden = true;
    }
    renderVerdict();
  }

  function narrationFor(node, step, entry, priority, suspended) {
    const name = node ? `S${node.label || node.id}` : "this state";
    const suspendedExec = suspended.filter((ref) => isExecName(ref.name));
    if (state.step === 0) {
      return `<b>S0</b> is the initial class: a token in <b>Aentry</b>, <b>Fentry</b> and <b>A_period</b>. A is periodic and ready; F is aperiodic and also ready. The core filter selects <b>Aget_core</b> (priority 97) over <b>Fget_core</b> (96).`;
    }
    if (suspendedExec.length) {
      const low = suspendedExec.map((ref) => ref.name).join(", ");
      return `<b>${esc(low)}</b> is enabled but not selected. It is suspendable, so it enters <b>R</b> and its execution clock freezes. <span class="why">The core belongs to the highest-priority enabled transition on core 0.</span>`;
    }
    if (state.step === 1) {
      return `<b>Aexec</b> is the only execution transition on core 0. Time can advance inside the interval <b>[4, 6]</b>; the next firing is legal only when the clock reaches the lower bound.`;
    }
    if (state.step === 2) {
      return `A has exited. <b>A_to_B</b> is a zero-priority control transition carrying the 1-unit dependency delay. F is no longer filtered: <b>Fget_core</b> can acquire the core.`;
    }
    if (state.step === 3) {
      return `F is running while the dependency delay elapses. When <b>Bentry</b> receives a token, B (priority 98) will outrank F (96).`;
    }
    if (state.step === 6) {
      return `B has exited. <b>B_consume</b> removes the terminal token, and <b>Fexec</b> becomes admissible again. F resumes from <b>h = 1</b>, not from zero.`;
    }
    if (state.step === 7) {
      return `F runs its residual work. Its execution clock enters at <b>1</b> and may fire anywhere in <b>[2, 10]</b>; this run takes the worst case, 10.`;
    }
    if (state.step === 8) {
      return `F reaches <b>Fexit</b>. The response time is <b>19</b>: 6 units of A, 1 unit of dependency delay, 3 units of B, and 9 units of residual F.`;
    }
    const selected = priority.map((ref) => ref.name).join(", ") || "—";
    return `<b>${esc(name)}</b>: the selected active set is <b>${esc(selected)}</b>. Fire the transition whose clock has reached its window, or let more time pass first.`;
  }

  function whyNow(entry, priority, suspended) {
    const structNames = new Set(entry.map((ref) => ref.name));
    const prioNames = new Set(priority.map((ref) => ref.name));
    const suspNames = new Set(suspended.map((ref) => ref.name));
    const blocked = [...structNames].filter((name) => !prioNames.has(name) && !suspNames.has(name));
    const suspExec = suspended.filter((ref) => isExecName(ref.name));
    if (suspExec.length) {
      return `<b>R = { ${suspExec.map((ref) => esc(ref.name)).join(", ")} }</b>: suspendable, enabled, but filtered out by a higher-priority transition on the same core. Its clock is frozen.`;
    }
    if (blocked.length) {
      return `<b>${esc(blocked.join(", "))}</b> is in E<sub>struct</sub> but not selected, and it is not suspendable. It is <b>blocked</b>, not suspended: the control step must wait for the core.`;
    }
    if (prioNames.size < structNames.size) {
      return `The per-core filter removed ${structNames.size - prioNames.size} lower-priority transition(s). The selected set is <b>E<sub>pri</sub></b>.`;
    }
    return `Every structurally enabled transition is admissible here. Control transitions on core −1 do not compete for the CPU.`;
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
        root.innerHTML = `<span class="chip-t empty">—</span>`;
        continue;
      }
      root.innerHTML = refs.map((ref) => {
        const kind = kindOf(ref.name);
        const firing = firingName && ref.name === firingName ? " firing" : "";
        return `<span class="chip-t ${kind}${firing}" title="${esc(ref.name)}">${esc(ref.name)}<small>p${fmt(ref.priority)} · c${fmt(ref.core)}</small></span>`;
      }).join("");
    }
  }

  function renderClocks(detail, step, suspendedNames) {
    const root = $("#firing-clocks");
    const clocks = (detail.clocks || []).filter((clock) => (
      clock.kind === "exec" && isExecName(transitionName(clock.transition) || "")
    ));
    if (!clocks.length) {
      root.innerHTML = `<p class="clock-empty">No execution segment is active in this class. Acquire and control transitions are zero-time; their clocks are visible in the raw DBM below.</p>`;
      return;
    }
    const rows = clocks.map((clock) => {
      const name = transitionName(clock.transition) || clock.name;
      const suspended = suspendedNames.includes(name);
      const isFired = step && step.tid === clock.transition;
      let lower = clock.lower;
      let upper = clock.upper;
      let label = "";
      let frozen = suspended;
      if (isFired && step) {
        lower = step.atMin;
        upper = step.atMax;
        label = state.phase === "elapsed"
          ? `fires at ${state.mode === "earliest" ? step.atMin : step.atMax}`
          : clock.lower !== step.atMin
            ? `h=${clock.lower} → [${step.atMin}, ${step.atMax}]`
            : `window [${step.atMin}, ${step.atMax}]`;
      } else if (suspended) {
        label = upper !== null && lower === upper ? `frozen at ${lower}` : "frozen";
      } else {
        label = upper !== null && lower === upper ? `= ${lower}` : upper === null ? `≥ ${lower}` : `[${lower}, ${upper}]`;
      }
      const scale = Math.max(10, ...clocks.map((item) => item.upper === null ? (item.lower + 10) : item.upper), step ? step.atMax : 10);
      const loPct = clamp((lower / scale) * 100, 0, 99);
      const hiPct = upper === null ? 100 : clamp((upper / scale) * 100, loPct + 1, 100);
      const chosen = state.phase === "elapsed" && isFired
        ? (state.mode === "earliest" ? step.atMin : step.atMax)
        : lower;
      const chosenPct = clamp((chosen / scale) * 100, loPct, hiPct);
      const width = Math.max(1.5, chosenPct - loPct);
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
    const ticks = [0, total * 0.25, total * 0.5, total * 0.75, total];
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
        const width = Math.max(0.8, ((segment.end - segment.start) / total) * 100);
        return `<span class="tl-seg ${segment.type} ${task.toLowerCase()}" style="left:${left}%;width:${width}%" title="${task} ${segment.type} ${segment.start}–${segment.end}">${segment.end - segment.start >= 1 ? `${segment.end - segment.start}` : ""}</span>`;
      }).join("");
      return `<div class="tl-row">
        <span class="tl-name"><b>${task}</b> · ${priority}</span>
        <div class="tl-track"><div class="tl-grid"></div>${segs}</div>
      </div>`;
    }).join("");
    root.innerHTML = `
      <div class="tl-scale">${ticks.map((tick) => `<span>${Math.round(tick)}</span>`).join("")}</div>
      ${rows}
      <div class="tl-now">t=${state.time}</div>
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
    $("#verdict-exact").textContent = metrics.exact === false ? "approximate" : "exact";
    const note = $("#timeline-note");
    if (note) {
      note.textContent = state.mode === "worst"
        ? "This run takes the latest admissible firing in every interval. The analyzer's WCRT is the worst case over the whole state-class graph."
        : "This run takes the earliest admissible firing in every interval. The analyzer's WCRT is still 19: other admissible timings are worse.";
    }
  }

  /* --------------------------------------------------------- policies chapter */

  const POLICY_META = {
    fixed: {
      title: "Static priority",
      rule: "E<sub>pri</sub> keeps the highest declared priority on each core. Equal-priority ties are broken by the smallest transition index, so the result is deterministic.",
      note: "This is the rule used in the walkthrough. The net structure is unchanged; only the winning transition changes.",
      structure: "The lowering is unchanged; A, B and F keep their declared priorities.",
    },
    rm: {
      title: "Rate monotonic",
      rule: "Priority is recomputed from the task period: a shorter period gets a higher priority. The task graph and the net structure stay the same.",
      note: "RM changes priorities, not the places/transitions. The analyzer reports the recomputed result.",
      structure: "Same places and transitions; priorities are recomputed from periods.",
    },
    dm: {
      title: "Deadline monotonic",
      rule: "Priority is recomputed from the relative deadline: a shorter deadline gets a higher priority. The task graph and the net structure stay the same.",
      note: "DM is another static-priority instantiation of the same firing interface.",
      structure: "Same places and transitions; priorities are recomputed from deadlines.",
    },
    restart: {
      title: "Structural restart",
      rule: "The same priorities, but the lowering adds a structural preemption sub-net. A preempted task returns to its entry place and restarts from zero.",
      note: "This is a lowering variant, not a pure firing-rule swap: the net gains preemption transitions. It is not the same net.",
      structure: "The source model is the same, but the lowered net is larger and different.",
    },
    edf: {
      title: "Earliest deadline first",
      rule: "Each job carries a release and deadline clock; the earliest absolute deadline wins. The rule is dynamic, so the winner can change at every job arrival.",
      note: "This analyzer supports EDF only for static partitions with one in-flight job per task, no locks, no fork/join and no dependency-driven releases.",
      structure: "The source model is the same; the rule uses per-job clocks instead of static priorities.",
    },
    fifo: {
      title: "First in, first out",
      rule: "The oldest ready job runs first. Equal priority does not preempt a running transition; arrival order is the arbitration rule.",
      note: "FIFO shares the same supported-class guard as EDF in this analyzer.",
      structure: "The source model is the same; the rule uses arrival order instead of priority.",
    },
  };

  function initPolicies() {
    state.policySummaries.fixed = summaryFromResponse(state.hw);
    state.policyCache.fixed = state.policySummaries.fixed;
    $$(".policy-card").forEach((button) => {
      button.addEventListener("click", () => selectPolicy(button.dataset.policy));
    });
    renderPolicy("fixed", state.policyCache.fixed);
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
      return;
    }
    const summary = state.policySummaries[policy];
    if (summary) {
      state.policyCache[policy] = summary;
      renderPolicy(policy, summary);
      return;
    }
    if (state.demo) {
      renderPolicy(policy, null);
      return;
    }
    $("#policy-loading").hidden = false;
    try {
      const override = policy === "restart" ? "fixed_prior_with_restart" : policy;
      const response = await analyzeExample("hw", { policy: override });
      const result = summaryFromResponse(response);
      state.policyCache[policy] = result;
      renderPolicy(policy, result);
    } catch (err) {
      renderPolicy(policy, { error: err.message });
    } finally {
      $("#policy-loading").hidden = true;
    }
  }

  function renderPolicy(policy, summary) {
    const meta = POLICY_META[policy];
    $("#policy-title").textContent = meta.title;
    $("#policy-rule").innerHTML = meta.rule;
    $("#policy-note").textContent = meta.note;
    $("#policy-structure-note").innerHTML = meta.structure;
    const exitChip = $("#policy-exit");
    if (!summary) {
      exitChip.textContent = "not in demo data";
      $("#policy-facts").innerHTML = "";
      $("#policy-result").innerHTML = `<p class="fine">This policy variant is generated by <span class="mono">webapp/make_demo.py</span> for the precomputed demo. Run the live server to execute it now.</p>`;
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
      ["states", summary.states === null || summary.states === undefined ? "—" : fmt(summary.states), summary.states ? "" : "warn"],
      ["WCRT(F)", fTask && fTask.wcrt !== null && fTask.wcrt !== undefined ? fmt(fTask.wcrt) : "—", fTask && fTask.wcrt !== null ? "good" : "warn"],
      ["schedulability", summary.schedulability || "—", summary.schedulability === "schedulable" ? "good" : summary.schedulability === "unschedulable" ? "bad" : "warn"],
      ["exact", summary.exact === false ? "no" : summary.exact ? "yes" : "—", summary.exact === false ? "warn" : ""],
    ];
    $("#policy-facts").innerHTML = facts.map(([label, value, cls]) => `<div class="policy-fact"><span>${esc(label)}</span><b class="${cls}">${esc(value)}</b></div>`).join("");
    const tasks = summary.tasks || [];
    const diagnostics = summary.diagnostics || [];
    const taskHtml = tasks.length ? `<div class="policy-task-list">${tasks.map((task) => {
      const verdict = task.verdict || "n/a";
      return `<div class="policy-task"><b>${esc(task.name)}</b><span>p${fmt(task.priority)}</span><span>WCRT ${fmt(task.wcrt)}</span><span class="verdict ${esc(task.verdict || "na")}">${esc(verdict)}</span></div>`;
    }).join("")}</div>` : "";
    const diagHtml = diagnostics.length ? `<div class="policy-task-list">${diagnostics.slice(0, 5).map((diag) => `<div class="policy-task"><b>${esc(diag.code)}</b><span>${esc(diag.severity || "")}</span><span>${esc(diag.subject || "")}</span><span></span></div>`).join("")}</div>` : "";
    $("#policy-result").innerHTML = taskHtml + diagHtml;
  }

  /* ------------------------------------------------------------- forms / loop */

  const PTPN_SOURCE = `// Direct PTPN source (excerpt): the same style of net, without the TDG front-end.
places
    Aentry:1, Aready:1, Aexit:1
    Fentry:1, Fready:1, Fexit:1
    A_period:1

transitions
    Aget_core [0, 0]  @priority=97, core=0
    Aexec     [4, 6]  @priority=97, core=0, suspendable
    A_fire    [20,20] @priority=0,  core=-1
    A_to_B    [1, 1]  @priority=0,  core=-1
    Fget_core [0, 0]  @priority=96, core=0
    Fexec     [2, 10] @priority=96, core=0, suspendable

// arcs, initial marking and capacities are omitted for brevity
@init Aentry:1, Fentry:1, A_period:1`;

  const FORM_META = {
    rtsl: {
      filename: "model.rtsl",
      intro: "RTSL is the human- and LLM-facing source language. It carries units, task kinds, cores, locks, dependencies and timing provenance in one readable file. The snippet is the resolved system; the timing panel below shows its pending version.",
      tradeoffs: ["units + provenance", "check / fill / allocate", "compact for LLMs"],
    },
    json: {
      filename: "model.tdg.json",
      intro: "The TDG JSON form is the graph-shaped interchange. It is familiar to tooling and easy to generate node-by-node, but it says less about timing provenance. The snippet is the analyzer's export of the same system.",
      tradeoffs: ["graph-native", "tool-friendly", "verbose nodes/edges"],
    },
    ptpn: {
      filename: "model.ptpn",
      intro: "PTPN source is the direct net form. It gives the most structural control and bypasses lowering, but it is the least forgiving to generate. This snippet is an excerpt of the A/B/F net, not a complete file.",
      tradeoffs: ["explicit places/arcs", "no lowering", "most verbose"],
    },
  };

  function initForms() {
    $$("#form-tabs button").forEach((button) => {
      button.addEventListener("click", () => {
        state.form = button.dataset.form;
        $$("#form-tabs button").forEach((other) => {
          const active = other === button;
          other.classList.toggle("active", active);
          other.setAttribute("aria-selected", active ? "true" : "false");
        });
        renderForm();
      });
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
  }

  function renderForm() {
    const meta = FORM_META[state.form];
    $("#form-filename").textContent = meta.filename;
    $("#form-intro").textContent = meta.intro;
    $("#form-tradeoffs").innerHTML = meta.tradeoffs.map((item) => `<span>${esc(item)}</span>`).join("");
    let text = state.sources[state.form] || "";
    if (state.form === "rtsl") {
      const systemAt = text.indexOf("system ");
      if (systemAt > 0) text = text.slice(systemAt);
    }
    if (state.form === "json") {
      if (!text) {
        text = "// The analyzer did not return a TDG export for this example.\n// Use the live workbench to export one.";
      } else {
        const lines = text.split("\n");
        if (lines.length > 80) text = `${lines.slice(0, 80).join("\n")}\n… (${lines.length - 80} more lines)`;
      }
    }
    const highlight = state.form === "rtsl" ? (line) => /measured by llm/.test(line) : () => false;
    $("#form-code").innerHTML = text.split("\n").map((line) => (
      highlight(line) ? `<span class="hl">${esc(line)}</span>` : esc(line)
    )).join("\n");
  }

  const FLOW_COPY = {
    generate: "<p><strong>Generate.</strong> Start from the requirements: task kinds, periods, deadlines, priorities, cores, locks and dependencies. The model is a claim; it is not yet a result.</p>",
    check: "<p><strong>Check.</strong> <span class=\"mono\">ptpn check</span> parses and validates without lowering. It returns every pending <span class=\"mono\">measured by llm</span> task in <span class=\"mono\">result.pending_measurements</span>.</p>",
    time: "<p><strong>Time.</strong> The only missing information is execution time. Confirm the source for every pending task; keep the provenance honest. Estimated or measured intervals are not certified WCET bounds.</p>",
    analyze: "<p><strong>Analyze.</strong> <span class=\"mono\">ptpn rtsl</span> or <span class=\"mono\">ptpn allocate</span> returns a verdict, WCRT, witness, exit code and an exact/truncated flag. A counterexample is a fact about the model, not a formatting error.</p>",
    report: "<p><strong>Report and revise.</strong> Feed the facts back: command, exit code, diagnostics, schedulability, exactness, and the witness. Ask for a proposed design diff. Do not silently rewrite timing provenance or hide truncation.</p>",
  };

  /* ---------------------------------------------------------------- timing */

  function initTiming() {
    const pending = (state.pending && state.pending.pending_measurements) || [];
    const measurement = pending[0] || null;
    const count = $("#timing-pending-count");
    count.textContent = pending.length ? `${pending.length} pending` : "none pending";
    if (!measurement) {
      $("#pending-card").innerHTML = `<h4>No pending measurement</h4><p>The selected example already has timing.</p>`;
      $("#interval-grid").innerHTML = "";
      $("#timing-fill").disabled = true;
      return;
    }
    const segments = Number(measurement.expected_segments || 1);
    const defaults = state.timingDefaults[measurement.task] || Array.from({ length: segments }, (_, index) => (
      index === 1 ? [1, 2] : [1, 1]
    ));
    state.timingDefaults[measurement.task] = defaults;
    $("#pending-card").innerHTML = `<h4>Pending measurement</h4><b>${esc(measurement.task)}</b><p>${segments} execution segment${segments === 1 ? "" : "s"} expected · runs ${fmt(measurement.runs, "—")} · timeout ${fmt(measurement.timeout, "—")} ms</p>`;
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
      count.textContent = `${pending.length} pending`;
    });
  }

  function renderIntervals(defaults) {
    $("#interval-grid").innerHTML = defaults.map((interval, index) => `
      <div class="interval-row">
        <span>seg ${index + 1}</span>
        <input type="number" min="0" step="1" value="${esc(interval[0])}" data-seg="${index}" data-bound="min" aria-label="segment ${index + 1} minimum">
        <input type="number" min="0" step="1" value="${esc(interval[1])}" data-seg="${index}" data-bound="max" aria-label="segment ${index + 1} maximum">
      </div>`).join("");
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
      manual: "<b>Certified or estimated?</b> If the user supplies a real bound, write <span class=\"mono\">certified by \"tool\"</span>. Otherwise <span class=\"mono\">estimated by \"source\"</span> is honest. Only the certified form is a WCET guarantee.",
      llm: "<b>Measured by llm.</b> The LLM runs the task on a single core and records the intervals. This is an observation, not a certified bound; keep the <span class=\"mono\">measured by llm</span> provenance.",
      tool: "<b>Generated harness.</b> The LLM writes a single-core driver; an external timing tool produces the intervals. Use <span class=\"mono\">estimated by \"tool\"</span> for an informal figure, or <span class=\"mono\">certified by \"tool\"</span> only when the tool actually certifies a bound.",
    };
    $("#provenance-note").innerHTML = notes[source] || notes.manual;
  }

  async function fillTiming(measurement) {
    const intervals = readIntervals();
    const button = $("#timing-fill");
    button.disabled = true;
    button.textContent = "Analyzing…";
    try {
      const certified = $("#timing-certified") && $("#timing-certified").checked;
      let provenance;
      if (state.timingSource === "llm") {
        provenance = `measured by llm { runs ${measurement.runs || 10}, timeout ${measurement.timeout || 2} }`;
      } else if (state.timingSource === "manual") {
        provenance = certified ? 'certified by "user"' : 'estimated by "user"';
      } else {
        provenance = certified ? 'certified by "external-tool"' : 'estimated by "external-tool"';
      }
      const replacement = `time [ ${intervals.map(([lo, hi]) => `[${lo}, ${hi}]`).join(", ")} ] ${provenance}`;
      const baseSource = state.sources.pendingRtsl || state.sources.rtsl;
      const lines = baseSource.split("\n");
      const lineIndex = Math.max(0, Number(measurement.inject && measurement.inject.line ? measurement.inject.line - 1 : 0));
      const indent = (lines[lineIndex] || "").match(/^\s*/)[0];
      lines[lineIndex] = `${indent}${replacement}`;
      const patched = lines.join("\n");
      state.sources.pendingRtsl = patched;
      state.sources.rtsl = patched;

      let response;
      if (state.demo) {
        await new Promise((resolve) => setTimeout(resolve, 350));
        response = state.intro;
      } else {
        response = await api("/api/analyze", {
          method: "POST",
          body: JSON.stringify({
            source: patched,
            filename: "model.rtsl",
            kind: "rtsl",
            mode: "analyze",
            options: exampleRecord("pending-demo").options,
          }),
        });
      }
      renderForm();
      $("#pending-card").innerHTML = `<h4>Measurement filled</h4><b>${esc(measurement.task)}</b><p>The pending task now has ${intervals.length} execution interval${intervals.length === 1 ? "" : "s"}. The provenance stays explicit.</p>`;
      renderTimingResult(response);
      $("#timing-pending-count").textContent = (response.pending_measurements || []).length
        ? `${response.pending_measurements.length} pending` : "none pending";
      $$(".flow-step").forEach((step) => step.classList.toggle("active", step.dataset.flow === "analyze"));
      $("#loop-detail").innerHTML = FLOW_COPY.analyze;
    } catch (err) {
      renderTimingError(err.message);
    } finally {
      button.disabled = false;
      button.textContent = "Fill timing & analyze";
    }
  }

  function renderTimingResult(response) {
    const result = $("#timing-result");
    const analysis = response.analysis || {};
    const tasks = analysis.tasks || [];
    const exit = response.exit_code;
    const sched = analysis.schedulability || "unknown";
    const cls = sched === "schedulable" ? "good" : sched === "unschedulable" ? "bad" : "warn";
    result.hidden = false;
    result.innerHTML = `
      <div class="result-head">
        <h4>analyzer verdict</h4>
        <span class="chip ${cls === "bad" ? "warn" : ""}">exit ${esc(exit)} · ${esc(sched)} · ${analysis.exact === false ? "bound" : "exact"}</span>
      </div>
      <div class="result-tasks">
        ${tasks.slice(0, 6).map((task) => `<div class="result-task"><b>${esc(task.task)}</b><span>WCRT ${fmt(task.wcrt)}</span><span class="verdict ${esc(task.verdict || "unknown")}">${esc(task.verdict || "—")}</span></div>`).join("") || `<p class="fine">No per-task verdict was returned.</p>`}
      </div>
      <p class="fine" style="margin-top:.55rem">${state.demo ? "Precomputed result for the reference intervals; the source above reflects your chosen values and provenance. Start the live server to re-analyze edited values." : "The source above was patched and re-analyzed by the local binary."}</p>`;
  }

  function renderTimingError(message) {
    const result = $("#timing-result");
    result.hidden = false;
    result.innerHTML = `<div class="result-head"><h4>analyzer error</h4><span class="chip warn">failed</span></div><p class="fine">${esc(message)}</p>`;
  }

  /* ---------------------------------------------------------------- repair */

  function initRepair() {
    const response = state.deadline;
    if (!response) {
      $("#repair-facts").innerHTML = `<p class="fine">The deadline-miss example is not present in this demo data.</p>`;
      $("#repair-text").textContent = "";
      return;
    }
    const analysis = response.analysis || {};
    const tasks = analysis.tasks || [];
    const taskA = tasks.find((task) => task.task === "a") || {};
    const taskB = tasks.find((task) => task.task === "b") || {};
    const diagnostics = response.diagnostics || [];
    const codes = diagnostics.map((diag) => `${diag.code}${diag.subject ? ` (${diag.subject})` : ""}`).join(", ") || "none";
    $("#repair-exit").textContent = `exit ${response.exit_code}`;
    const facts = [
      ["schedulability", analysis.schedulability || "—", analysis.schedulability === "unschedulable" ? "bad" : ""],
      ["exact / truncated", `${analysis.exact === false ? "bound" : "exact"} / ${analysis.truncated ? "yes" : "no"}`, analysis.truncated ? "bad" : ""],
      ["diagnostics", codes, "bad"],
      ["task a", `${taskA.verdict || "—"} · WCRT ${fmt(taskA.wcrt)} · deadline ${fmt(taskA.deadline)}`, taskA.verdict === "misses" ? "bad" : ""],
      ["task b", `${taskB.verdict || "—"} · WCRT ${fmt(taskB.wcrt)} · deadline ${fmt(taskB.deadline)}`, taskB.verdict === "misses" ? "bad" : ""],
      ["witness", (taskA.witness || []).join(" → ") || "—", ""],
    ];
    $("#repair-facts").innerHTML = facts.map(([label, value, cls]) => `<div class="repair-fact"><span>${esc(label)}</span><b class="${cls}">${esc(value)}</b></div>`).join("");
    const prompt = [
      "Revise the RTSL model so that the analyzer's verdict can become schedulable.",
      "",
      "FACTS FROM THE ANALYZER (do not ignore or rephrase as a vibe):",
      `- command: ptpn rtsl -f model.rtsl --diagnostics json -m 10`,
      `- exit code: ${response.exit_code}`,
      `- schedulability: ${analysis.schedulability || "unknown"}`,
      `- exact: ${analysis.exact === false ? "false (bound)" : "true"}; truncated: ${analysis.truncated ? "true" : "false"}`,
      `- diagnostics: ${codes}`,
      `- task a: verdict=${taskA.verdict || "unknown"}, wcrt=${taskA.wcrt ?? "null"}, wcrt_lb=${taskA.wcrt_lb ?? "null"}, deadline=${taskA.deadline ?? "null"}, witness=${(taskA.witness || []).join(" -> ") || "none"}`,
      `- task b: verdict=${taskB.verdict || "unknown"}, wcrt=${taskB.wcrt ?? "null"}, deadline=${taskB.deadline ?? "null"}`,
      "",
      "Propose a minimal design change (priority, core assignment, period, deadline or dependency)",
      "and show the exact source diff. State the expected new verdict and which fact it addresses.",
      "Do not silently rewrite timing provenance or hide the truncation. If a requirement must",
      "change, ask the user before changing it.",
    ].join("\n");
    $("#repair-text").textContent = prompt;
    $("#repair-copy").addEventListener("click", () => copyText(prompt, $("#repair-copy")));
  }

  /* --------------------------------------------------------------- chapters */

  function initChapters() {
    const sections = ["problem", "model", "firing", "policies", "loop"]
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
    disableLocalLinks();
    $("#story-mode-pill").textContent = state.demo ? "PRECOMPUTED" : "LIVE";
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
      // Still wire the static chapters so the page remains navigable.
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
