/* PTPN Chronograph - browser workbench.
   Runs live against webapp/server.py, or against precomputed responses in the
   published demo (window.PTPN_DEMO_DATA). No external libraries. */
(() => {
  "use strict";

  const SVGNS = "http://www.w3.org/2000/svg";
  const DEMO = !!window.PTPN_DEMO_DATA;
  const DEMO_DATA = window.PTPN_DEMO_DATA || null;
  const REDUCED = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

  const TABS = ["overview", "model", "tdg", "ptpn", "scg", "metrics"];
  const STAGE_TAB = { source: "model", tdg: "tdg", ptpn: "ptpn", scg: "scg" };
  const CORE_COLORS = ["#3987e5", "#d95926", "#199e70"];
  const CORE_NEUTRAL = "#8a857c";

  const state = {
    demo: DEMO,
    lang: localStorage.getItem("ptpn_lang") || "zh",
    theme: localStorage.getItem("ptpn_theme") || "dark",
    status: null,
    examples: [],
    current: null,
    source: "",
    originalSource: "",
    filename: "model.rtsl",
    kind: "rtsl",
    options: defaultOptions(),
    response: null,
    lastResponse: null,
    diagnostics: [],
    pending: [],
    logs: [],
    busy: false,
    dirty: false,
    activeTab: "overview",
    selected: { tdg: null, ptpn: null, scg: null },
    graphs: { tdg: null, ptpn: null, scg: null },
    views: {},
    filters: { ptpnLabels: false, scgDeadlocks: false },
    pipelineTimers: [],
    toastTimer: null,
    modalReturn: null,
    tip: null,
  };

  /* ------------------------------------------------------------------ utils */

  function defaultOptions() {
    return {
      max_states: 10000,
      canonicalization: "equality",
      reduction: "none",
      policy: "",
      horizon: null,
      extrapolation: false,
      keep_suspension_clocks: false,
    };
  }

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

  function fmtInt(v, fallback = "-") {
    return isNum(v) ? Math.round(Number(v)).toLocaleString("en-US") : fallback;
  }

  function fmtNum(v, digits = 1, fallback = "-") {
    if (!isNum(v)) return fallback;
    const n = Number(v);
    return Math.abs(n - Math.round(n)) < 1e-9 ? String(Math.round(n)) : n.toFixed(digits);
  }

  function fmtPct(v, digits = 1) {
    return isNum(v) ? `${(Number(v) * 100).toFixed(digits)}%` : "-";
  }

  function fmtMs(v) {
    if (!isNum(v)) return "-";
    const n = Number(v);
    return n < 1 ? "<1 ms" : `${Math.round(n)} ms`;
  }

  function fmtDuration(ms) {
    if (!isNum(ms)) return "-";
    const n = Number(ms);
    if (n < 1000) return `${Math.round(n)} ms`;
    return `${(n / 1000).toFixed(2)} s`;
  }

  function sumSegments(segs, index) {
    if (!Array.isArray(segs)) return 0;
    return segs.reduce((acc, s) => acc + (Array.isArray(s) && isNum(s[index]) ? Number(s[index]) : 0), 0);
  }

  function deadlockCount(metrics) {
    const d = metrics && metrics.deadlock_states;
    if (Array.isArray(d)) return d.length;
    return isNum(d) ? Number(d) : 0;
  }

  // Tri-state schedulability: prefers the `schedulability` string, falls back
  // to the legacy boolean (`false` with no proven counterexample is unknown).
  function schedulabilityOf(metrics) {
    if (!metrics) return "unknown";
    if (metrics.schedulability === "schedulable" || metrics.schedulability === "unschedulable" ||
        metrics.schedulability === "unknown") {
      return metrics.schedulability;
    }
    if (metrics.schedulable === true) return "schedulable";
    if (metrics.schedulable === false) {
      return deadlockCount(metrics) > 0 || (metrics.tasks || []).some((t) => t && t.deadline_missed)
        ? "unschedulable"
        : "unknown";
    }
    return "unknown";
  }

  function truncate(text, max) {
    const s = String(text ?? "");
    return s.length > max ? `${s.slice(0, Math.max(0, max - 1))}...` : s;
  }

  function wrapName(text, max = 15, maxLines = 2) {
    const s = String(text ?? "");
    if (s.length <= max) return [s];
    const parts = s.split(/_/);
    const lines = [];
    let cur = "";
    for (const part of parts) {
      const next = cur ? `${cur}_${part}` : part;
      if (next.length > max && cur) {
        lines.push(cur);
        cur = part;
      } else {
        cur = next;
      }
      if (lines.length === maxLines - 1 && cur) {
        lines.push(cur.length > max + 3 ? `${cur.slice(0, max)}...` : cur);
        cur = "";
        break;
      }
    }
    if (cur && lines.length < maxLines) lines.push(cur);
    if (lines.length > maxLines) lines.length = maxLines;
    return lines.length ? lines : [truncate(s, max)];
  }

  function countLines(text) {
    return String(text ?? "").split("\n").length;
  }

  function severityClass(sev) {
    return sev === "error" ? "error" : sev === "warning" ? "warning" : "note";
  }

  function setText(el, text) {
    if (el) el.textContent = text;
  }

  function svgEl(tag, attrs = {}, text) {
    const el = document.createElementNS(SVGNS, tag);
    for (const [k, v] of Object.entries(attrs)) {
      if (v !== null && v !== undefined) el.setAttribute(k, String(v));
    }
    if (text !== undefined) el.textContent = String(text);
    return el;
  }

  function polyline(points) {
    return points.map(([x, y]) => `${x.toFixed(2)},${y.toFixed(2)}`).join(" ");
  }

  /* ------------------------------------------------------------------- api */

  async function api(path, opts = {}) {
    const res = await fetch(path, {
      headers: { "Content-Type": "application/json" },
      ...opts,
    });
    const text = await res.text();
    let data;
    try {
      data = JSON.parse(text);
    } catch {
      throw new Error(`HTTP ${res.status}: ${truncate(text.replace(/\s+/g, " "), 180)}`);
    }
    if (!res.ok && !data.diagnostics) {
      throw new Error(data.error || `HTTP ${res.status}`);
    }
    return data;
  }

  function normalizeExample(ex) {
    const kind = ex.kind === "tdg" ? "tdg" : ex.kind === "ptpn" ? "ptpn" : "rtsl";
    return {
      ...ex,
      kind,
      options: { ...defaultOptions(), ...(ex.options || {}) },
      demo: !!ex.demo,
      response: ex.response || null,
      source: ex.source || "",
      name: ex.name || (kind === "tdg" ? "model.json" : kind === "ptpn" ? "model.ptpn" : "model.rtsl"),
    };
  }

  function groupFor(ex) {
    const p = String(ex.path || "");
    if (p.includes("/diagnostics/")) return "Diagnostics";
    if (p.includes("/introduction/")) return "Introduction";
    if (p.includes("/motivating-examples/")) return "Core Preemption (A/B/F)";
    if (p.includes("/symmetry/")) return "Reductions";
    if (p.includes("/l-bench/") || p.includes("/p-bench/") || p.includes("/s-bench/") || p.includes("/t-bench/")) return "Benchmarks";
    return "Examples";
  }

  /* ------------------------------------------------------------------- boot */

  async function boot() {
    bindStatic();
    startClock();
    const requestedTab = initialTab();
    renderOptions(state.options, false);
    renderAllEmpty();
    setRunbar({
      label: state.demo ? "Loading demo..." : "Connecting to analyzer...",
      dot: "busy",
      exit: null,
      duration: null,
      meta: state.demo ? "precomputed" : "build/ptpn",
    });

    if (DEMO) {
      state.status = (DEMO_DATA && DEMO_DATA.status) || { ok: true, demo: true, binary_exists: false, version: "precomputed" };
      state.examples = ((DEMO_DATA && DEMO_DATA.examples) || []).map(normalizeExample);
      renderMode();
      renderExamples();
      const exampleId = new URLSearchParams(location.search).get("example");
      const preferred = state.examples.find((e) => e.id === exampleId) || state.examples.find((e) => e.id === "intro-demo") || state.examples[0];
      if (preferred) selectExample(preferred.id, { initial: true });
      else renderEmptyAll("Demo data is empty.");
      if (requestedTab) setActiveTab(requestedTab);
      return;
    }

    try {
      const [status, examples] = await Promise.all([
        api("/api/status"),
        api("/api/examples"),
      ]);
      state.status = status;
      state.examples = (examples.examples || []).map(normalizeExample);
      renderMode();
      renderExamples();
      const exampleId = new URLSearchParams(location.search).get("example");
      const preferred = state.examples.find((e) => e.id === exampleId) || state.examples.find((e) => e.id === "intro-demo") || state.examples[0];
      if (preferred) selectExample(preferred.id, { initial: true, autoRun: true });
      else renderEmptyAll("No built-in models are available.");
    } catch (err) {
      state.status = { ok: false, error: String(err && err.message ? err.message : err), binary_exists: false };
      renderMode();
      renderExamples();
      renderEmptyAll("Cannot connect to the local analysis service.");
      showToast("Cannot connect to the local analysis service. Start python3 webapp/server.py first.", "err");
    }
    if (requestedTab) setActiveTab(requestedTab);
  }

  function initialTab() {
    const token = String(location.hash || "").replace(/^#/, "");
    return TABS.includes(token) ? token : null;
  }

  function renderMode() {
    const mode = $("#pill-mode");
    const binary = $("#pill-binary");
    const note = $("#options-note");
    const offline = !state.demo && (!state.status || state.status.ok === false || state.status.binary_exists === false);

    if (state.demo) {
      mode.textContent = "DEMO | PRECOMPUTED";
      mode.className = "pill accent";
      binary.textContent = "no backend";
      note.textContent = "precomputed";
    } else if (offline) {
      mode.textContent = "OFFLINE";
      mode.className = "pill";
      mode.style.color = "var(--critical)";
      binary.textContent = "binary missing";
      note.textContent = "offline";
    } else {
      mode.textContent = "LOCAL | LIVE";
      mode.className = "pill live";
      mode.style.color = "";
      binary.textContent = state.status.version ? truncate(state.status.version, 28) : "build/ptpn";
      note.textContent = "live";
    }

    if (state.demo) {
      $("#options").classList.add("locked");
      $$("#options input, #options select").forEach((el) => { el.disabled = true; });
    } else {
      $("#options").classList.remove("locked");
      $$("#options input, #options select").forEach((el) => { el.disabled = false; });
    }

    const analyze = $("#btn-analyze");
    const check = $("#btn-check");
    if (offline) {
      analyze.disabled = true;
      check.disabled = true;
    } else {
      analyze.disabled = false;
      check.disabled = false;
    }
  }

  function startClock() {
    const el = $("#pill-clock");
    const tick = () => {
      const d = new Date();
      setText(el, d.toLocaleTimeString("en-GB", { hour12: false }));
    };
    tick();
    setInterval(tick, 1000);
  }

  /* --------------------------------------------------------------- examples */

  function renderExamples() {
    const root = $("#example-list");
    root.innerHTML = "";
    if (!state.examples.length) {
      root.innerHTML = `<div class="note">No models yet.</div>`;
      return;
    }
    const groups = new Map();
    for (const ex of state.examples) {
      const g = groupFor(ex);
      if (!groups.has(g)) groups.set(g, []);
      groups.get(g).push(ex);
    }
    for (const [group, items] of groups) {
      const label = document.createElement("div");
      label.className = "ex-group";
      label.textContent = group;
      root.append(label);
      for (const ex of items) {
        const wrap = document.createElement("div");
        wrap.className = "ex-wrap";
        wrap.setAttribute("role", "listitem");
        const btn = document.createElement("button");
        btn.type = "button";
        btn.className = "ex";
        btn.dataset.id = ex.id;
        btn.setAttribute("aria-current", state.current && state.current.id === ex.id ? "true" : "false");
        const kindBadge = ex.kind === "tdg" ? "TDG JSON" : ex.kind === "ptpn" ? "PTPN" : "RTSL";
        btn.innerHTML = `
          <span class="ex-top"><span class="ex-title">${esc(ex.title || ex.name)}</span><span class="ex-kind">${kindBadge}${state.demo && ex.demo ? " | D" : ""}</span></span>
          <span class="ex-file">${esc(ex.name)}</span>
          <span class="ex-desc">${esc(ex.description || "")}</span>`;
        btn.addEventListener("click", () => selectExample(ex.id));
        wrap.append(btn);
        root.append(wrap);
      }
    }
  }

  function selectExample(id, opts = {}) {
    const ex = state.examples.find((e) => e.id === id);
    if (!ex) return;
    state.current = ex;
    state.source = ex.source;
    state.originalSource = ex.source;
    state.filename = ex.name;
    state.kind = ex.kind;
    state.options = { ...defaultOptions(), ...(ex.options || {}) };
    state.dirty = false;
    state.selected = { tdg: null, ptpn: null, scg: null };
    renderOptions(state.options, state.demo);
    setEditorSource(ex.source, { keepScroll: false });
    renderExamples();
    const activeEl = Array.from($$("#example-list .ex")).find((el) => el.dataset.id === id);
    if (activeEl) activeEl.scrollIntoView({ block: "nearest" });
    if (window.matchMedia && window.matchMedia("(max-width: 880px)").matches) {
      $(".rail").classList.remove("open");
      $("#btn-rail").setAttribute("aria-expanded", "false");
    }
    updateEditorMeta();
    resetRunState();
    if (state.demo && ex.response) {
      if (opts.initial || REDUCED) {
        applyResponse(ex.response, { mode: "analyze", keepTab: true, instant: true });
      } else {
        simulateRun(ex.response);
      }
    } else if (!state.demo && opts.autoRun) {
      runAnalysis("analyze", { keepTab: false });
    } else {
      renderAllEmpty();
    }
  }

  /* ---------------------------------------------------------------- options */

  function renderOptions(options, locked) {
    $("#opt-max-states").value = options.max_states;
    $("#opt-reduction").value = options.reduction || "none";
    $("#opt-canon").value = options.canonicalization || "equality";
    $("#opt-policy").value = options.policy || "";
    $("#opt-horizon").value = options.horizon == null ? "" : options.horizon;
    $("#opt-extrapolation").checked = !!options.extrapolation;
    $("#opt-keep-susp").checked = !!options.keep_suspension_clocks;
    if ($("#toolbar-policy")) $("#toolbar-policy").value = options.policy || "";
    if ($("#toolbar-reduction")) $("#toolbar-reduction").value = options.reduction || "none";
    $("#options").classList.toggle("locked", !!locked);
    $$("#options input, #options select, #toolbar-policy, #toolbar-reduction").forEach((el) => {
      if (el) el.disabled = !!locked;
    });
  }

  function collectOptions() {
    const horizonRaw = $("#opt-horizon").value.trim();
    return {
      max_states: clamp(parseInt($("#opt-max-states").value, 10) || 10000, 1, 200000),
      canonicalization: $("#opt-canon").value,
      reduction: $("#opt-reduction").value,
      policy: $("#opt-policy").value,
      horizon: horizonRaw ? clamp(parseInt(horizonRaw, 10) || 1, 1, 1000000000) : null,
      extrapolation: $("#opt-extrapolation").checked,
      keep_suspension_clocks: $("#opt-keep-susp").checked,
    };
  }

  function onOptionsChanged() {
    state.options = collectOptions();
    if (state.response) {
      // Options no longer match the rendered result; mark it visually.
      setRunMeta("Options changed | run again to update");
    }
  }

  /* ------------------------------------------------------------------- run */

  function resetRunState() {
    state.response = null;
    state.lastResponse = null;
    state.diagnostics = [];
    state.pending = [];
    state.logs = [];
    state.graphs = { tdg: null, ptpn: null, scg: null };
    Object.values(state.views).forEach((view) => view.clear());
    state.selected = { tdg: null, ptpn: null, scg: null };
    setRunbar({ label: "Ready", dot: "", exit: null, duration: null, meta: "" });
    renderSignals();
    renderConsole();
    renderDiagnostics();
    renderPending();
    renderGraphs();
    renderCode();
  }

  async function runAnalysis(mode = "analyze", opts = {}) {
    if (state.busy) return;
    if (state.demo) {
      if (state.dirty) {
        openDemoHint();
        return;
      }
      if (state.current && state.current.response) {
        simulateRun(state.current.response, mode === "check");
      } else {
        showToast("The demo has no precomputed data for this model.", "err");
      }
      return;
    }
    if (!state.status || state.status.binary_exists === false) {
      showToast("ptpn binary not found. Build the project or set PTPN_BIN.", "err");
      return;
    }
    if (!state.source.trim()) {
      showToast("The model is empty. Enter RTSL or TDG JSON before running.", "err");
      return;
    }

    state.busy = true;
    state.diagnostics = [];
    state.pending = [];
    setBusy(true);
    startPipelineAnimation();
    setRunbar({ label: mode === "check" ? "Checking..." : "Exploring state classes...", dot: "busy", exit: null, duration: null, meta: "" });
    const started = performance.now();
    try {
      const body = {
        source: state.source,
        filename: state.filename,
        kind: state.kind,
        mode,
        options: collectOptions(),
      };
      const resp = await api("/api/analyze", { method: "POST", body: JSON.stringify(body) });
      resp.client_duration_ms = Math.round(performance.now() - started);
      stopPipelineAnimation(resp.ok || resp.metrics);
      applyResponse(resp, { mode, keepTab: !!opts.keepTab });
      if (resp.ok) {
        const states = resp.metrics && resp.metrics.states;
        showToast(
          mode === "check"
            ? `Check complete | ${resp.exit_code === 0 ? "passed" : `exit ${resp.exit_code}`}`
            : `Analysis complete | ${states != null ? `${fmtInt(states)} states | ` : ""}${fmtDuration(resp.client_duration_ms || resp.duration_ms)}`,
          resp.exit_code === 0 ? "ok" : "err"
        );
      } else if (!(resp.diagnostics || []).length) {
        showToast(resp.error || "Analysis failed.", "err");
      }
    } catch (err) {
      stopPipelineAnimation(false);
      setRunbar({ label: "Request failed", dot: "err", exit: null, duration: Math.round(performance.now() - started), meta: String(err.message || err) });
      renderConsoleError(String(err.message || err));
      showToast(`Could not complete analysis: ${truncate(err.message || String(err), 160)}`, "err");
    } finally {
      state.busy = false;
      setBusy(false);
    }
  }

  function simulateRun(response, checkOnly = false) {
    if (state.busy) return;
    state.busy = true;
    setBusy(true);
    startPipelineAnimation();
    setRunbar({ label: checkOnly ? "Checking (precomputed)..." : "Replaying precomputed run...", dot: "busy", exit: null, duration: null, meta: "" });
    const delay = REDUCED ? 0 : 520;
    setTimeout(() => {
      stopPipelineAnimation(true);
      applyResponse(response, { mode: checkOnly ? "check" : "analyze", keepTab: checkOnly });
      state.busy = false;
      setBusy(false);
      if (!checkOnly) showToast("Demo: showing precomputed results. The local server can analyze any model.", "ok");
    }, delay);
  }

  function setBusy(busy) {
    const analyze = $("#btn-analyze");
    const check = $("#btn-check");
    analyze.disabled = busy || (!state.demo && state.status && state.status.binary_exists === false);
    check.disabled = analyze.disabled || state.kind !== "rtsl";
    analyze.textContent = busy ? "Running..." : "Analyze";
    if (!busy) {
      analyze.innerHTML = `Analyze <kbd>Cmd+Enter</kbd>`;
      check.innerHTML = `Check <kbd>Cmd+Shift+Enter</kbd>`;
    }
    if (busy) {
      analyze.textContent = "Running...";
      check.textContent = "Check";
    }
  }

  function applyResponse(resp, opts = {}) {
    state.lastResponse = resp;
    state.diagnostics = Array.isArray(resp.diagnostics) ? resp.diagnostics : [];
    state.pending = Array.isArray(resp.pending_measurements) ? resp.pending_measurements : [];
    state.logs = Array.isArray(resp.logs) ? resp.logs : [];
    const hasAnalysis = !!(resp.metrics || (resp.graphs && (resp.graphs.ptpn || resp.graphs.scg)) || resp.tdg);
    if (hasAnalysis) {
      state.response = resp;
    } else if (opts.mode !== "check") {
      // A failed analysis must not leave stale metrics on screen.
      state.response = null;
    }
    state.options = { ...defaultOptions(), ...(resp.effective_options || state.options) };
    renderOptions(state.options, state.demo);
    updatePipeline();
    updateEditorMeta();
    renderSignals();
    renderDiagnostics();
    renderPending();
    renderRunbar(resp);
    renderConsole();
    const errors = state.diagnostics.filter((d) => d.severity === "error");
    if (state.response) {
      renderOverview();
      renderMetrics();
      renderGraphs();
    } else {
      let reason = opts.mode === "check" ? "Check mode does not include analysis results." : "There is no analysis result to display.";
      if (state.pending.length) reason = "Waiting for pending measurements";
      else if (errors.length) reason = "Analysis is blocked by diagnostics";
      renderEmptyAll(reason, { keepDiagnostics: true });
    }
    renderCode();
    if (!state.response && (errors.length || state.pending.length)) {
      setActiveTab("model");
    } else if (opts.mode === "check") {
      if (!opts.keepTab) setActiveTab("model");
    } else if (!opts.keepTab) {
      setActiveTab("overview");
    }
  }

  function renderEmptyAll(reason, opts = {}) {
    state.response = null;
    renderOverview();
    renderMetrics();
    renderGraphs();
    if (!opts.keepDiagnostics) {
      state.diagnostics = [];
      state.pending = [];
    }
    renderDiagnostics();
    renderPending();
    renderSignals();
    renderConsole();
    updatePipeline();
    if (reason) setRunMeta(reason);
  }

  function setRunbar({ label, dot, exit, duration, meta }) {
    const dotEl = $("#run-dot");
    if (dotEl) dotEl.className = `status-dot ${dot || ""}`;
    setText($("#run-label"), label || "Ready");
    setText($("#run-meta"), meta || "");
    const exitEl = $("#chip-exit");
    if (exit != null) {
      exitEl.textContent = `exit ${exit}`;
      exitEl.style.color = exit === 0 ? "var(--good)" : exit === 6 ? "var(--critical)" : "var(--warn)";
    } else {
      exitEl.textContent = "exit -";
      exitEl.style.color = "";
    }
    setText($("#chip-duration"), duration != null ? fmtDuration(duration) : "- ms");
  }

  function renderRunbar(resp) {
    const metrics = resp.metrics;
    const errors = state.diagnostics.filter((d) => d.severity === "error").length;
    const pending = state.pending.length;
    let label = resp.ok ? "Clean run" : "Diagnostics found";
    let dot = resp.ok ? "ok" : "err";
    if (pending && !resp.ok) { label = "Pending measurements"; dot = "warn"; }
    else if (resp.exit_code === 6) { label = "Certified negative result"; dot = "err"; }
    else if (!resp.ok && errors) { label = "Blocked by diagnostics"; dot = "err"; }
    else if (!resp.ok) { label = "Diagnostics found"; dot = "err"; }
    else if (resp.analysis && resp.analysis.truncated) { label = "Run truncated"; dot = "warn"; }
    else if (metrics && schedulabilityOf(metrics) === "unschedulable") { label = "Not schedulable"; dot = "err"; }
    else if (metrics && schedulabilityOf(metrics) === "unknown") { label = "Schedulability unknown"; dot = "warn"; }
    else if (metrics && metrics.exact === false) { label = "Approximate result"; dot = "warn"; }
    else if (pending) { label = "Pending measurements"; dot = "warn"; }
    const parts = [];
    if (resp.analysis) {
      parts.push(`${fmtInt(resp.analysis.states)} states`);
      parts.push(`${fmtInt(resp.analysis.transitions)} edges`);
      if (resp.analysis.deadlock) parts.push(`${fmtInt(resp.analysis.deadlock_states)} deadlock`);
    } else if (metrics) {
      parts.push(`${fmtInt(metrics.states)} states`);
      parts.push(`${fmtInt(metrics.transitions)} edges`);
    }
    if (pending) parts.push(`${pending} pending`);
    if (errors) parts.push(`${errors} errors`);
    if (state.demo) parts.push("precomputed");
    setRunbar({
      label,
      dot,
      exit: resp.exit_code,
      duration: resp.client_duration_ms != null ? resp.client_duration_ms : resp.duration_ms,
      meta: parts.join(" | "),
    });
    const showRepair = !resp.ok || errors > 0 || resp.exit_code === 6 || (metrics && schedulabilityOf(metrics) === "unschedulable");
    ["#btn-llm-repair", "#btn-overview-repair", "#btn-model-repair"].forEach((sel) => {
      const btn = $(sel);
      if (btn) btn.hidden = !showRepair;
    });
  }

  function setRunMeta(text) {
    setText($("#run-meta"), text || "");
  }

  /* -------------------------------------------------------------- pipeline */

  function clearPipelineTimers() {
    for (const t of state.pipelineTimers) clearTimeout(t);
    state.pipelineTimers = [];
  }

  function startPipelineAnimation() {
    clearPipelineTimers();
    $$(".stage").forEach((s) => { s.classList.remove("done"); });
    $$(".stage-link").forEach((l) => { l.classList.remove("running"); });
    const stages = ["source", "tdg", "ptpn", "scg"];
    stages.forEach((name, i) => {
      const t = setTimeout(() => {
        const el = $(`.stage[data-stage="${name}"]`);
        if (el) el.classList.add("running");
        const link = el && el.nextElementSibling;
        if (link && link.classList.contains("stage-link")) link.classList.add("running");
      }, REDUCED ? 0 : i * 210);
      state.pipelineTimers.push(t);
    });
  }

  function stopPipelineAnimation(success) {
    clearPipelineTimers();
    $$(".stage").forEach((s) => s.classList.remove("running"));
    $$(".stage-link").forEach((l) => l.classList.remove("running"));
    if (success) {
      $$(".stage").forEach((s) => s.classList.add("done"));
    }
  }

  function updatePipeline() {
    const resp = state.response;
    const metrics = resp && resp.metrics;
    const tdg = resp && resp.tdg;
    const ptpn = resp && resp.graphs && resp.graphs.ptpn;
    const scg = resp && resp.graphs && resp.graphs.scg;

    const kindLabel = state.kind === "tdg" ? "TDG JSON" : state.kind === "ptpn" ? "Native PTPN" : "RTSL";
    setText($("#stage-source-metric"), `${countLines(state.source)} lines`);
    setText($("#stage-source-sub"), `${state.filename || "model"} | ${kindLabel}`);

    if (tdg && Array.isArray(tdg.nodes)) {
      const tasks = tdg.nodes.filter((n) => (n.type || "task") === "task").length;
      const edges = Array.isArray(tdg.edges) ? tdg.edges.length : 0;
      setText($("#stage-tdg-metric"), `${fmtInt(tasks)} tasks`);
      setText($("#stage-tdg-sub"), `${fmtInt(tdg.nodes.length)} nodes | ${fmtInt(edges)} edges`);
    } else {
      setText($("#stage-tdg-metric"), "-");
      setText($("#stage-tdg-sub"), "dependency graph");
    }

    if (ptpn && Array.isArray(ptpn.nodes)) {
      const PLACE_KINDS = new Set(["place", "resource", "period", "hold", "segment"]);
      const places = ptpn.nodes.filter((n) => PLACE_KINDS.has(n.kind)).length;
      const trans = ptpn.nodes.length - places;
      setText($("#stage-ptpn-metric"), `${fmtInt(places)} places`);
      setText($("#stage-ptpn-sub"), `${fmtInt(trans)} transitions | ${fmtInt(ptpn.edges ? ptpn.edges.length : 0)} arcs`);
    } else {
      setText($("#stage-ptpn-metric"), "-");
      setText($("#stage-ptpn-sub"), "timed Petri net");
    }

    if (metrics) {
      setText($("#stage-scg-metric"), fmtInt(metrics.states));
      setText($("#stage-scg-sub"), `${fmtInt(metrics.transitions)} edges | ${metrics.exact ? "exact" : "approx"}`);
    } else if (scg) {
      setText($("#stage-scg-metric"), fmtInt(scg.node_count));
      setText($("#stage-scg-sub"), `${fmtInt(scg.edge_count)} edges`);
    } else {
      setText($("#stage-scg-metric"), "-");
      setText($("#stage-scg-sub"), "state classes");
    }
  }

  /* --------------------------------------------------------------- overview */

  function renderOverview() {
    const resp = state.response;
    const metrics = resp && resp.metrics;
    renderStats(metrics, resp);
    renderDialPanel(metrics);
    renderWcrtChart(metrics);
    renderCoreChart(metrics);
    renderLockMini(metrics);
    renderOverviewDiagnostics();
  }

  function renderStats(metrics, resp) {
    const grid = $("#stat-grid");
    if (!metrics) {
      grid.innerHTML = `
        ${statTile("Schedulability", "-", "RUN ANALYSIS", "neutral")}
        ${statTile("State classes", "-", "no result", "neutral")}
        ${statTile("Tasks", "-", "no result", "neutral")}
        ${statTile("Max WCRT", "-", "no result", "neutral")}`;
      return;
    }
    const tasks = metrics.tasks || [];
    const maxWcrt = tasks.reduce((m, t) => Math.max(m, isNum(t.wcrt) ? Number(t.wcrt) : 0), 0);
    const deadlocks = deadlockCount(metrics);
    const schedState = schedulabilityOf(metrics);
    const sched = schedState === "schedulable";
    const unknownSched = schedState === "unknown";
    const exact = metrics.exact !== false;
    const status = sched ? (exact ? "ok" : "warn") : unknownSched ? "warn" : "err";
    const schedLabel = sched ? (exact ? "YES" : "YES*") : unknownSched ? "UNKNOWN" : "NO";
    const schedSub = sched
      ? (exact ? "certified exact run" : "approximate / reduced")
      : unknownSched
        ? "not certified by the explored graph"
        : `${deadlocks ? `${deadlocks} deadlock states` : "deadline violation or deadlock"}`;
    const taskCount = tasks.length;
    const periods = tasks.map((t) => Number(t.period) || 0).filter((p) => p > 0);
    const minDeadline = tasks.map((t) => Number(t.deadline) || 0).filter((d) => d > 0).sort((a, b) => a - b)[0];
    grid.innerHTML = `
      ${statTile("Schedulability", schedLabel, schedSub, status)}
      ${statTile("State classes", fmtInt(metrics.states), `${fmtInt(metrics.transitions)} firing edges | ${metrics.bounded ? "bounded" : "unbounded"}`, "accent")}
      ${statTile("Tasks", fmtInt(taskCount), `${periods.length ? `hyperperiod ${fmtInt(metrics.hyperperiod)} ms` : "aperiodic set"}`, "neutral")}
      ${statTile("Max WCRT", `${fmtNum(maxWcrt)}<small> ms</small>`, minDeadline ? `min deadline ${fmtInt(minDeadline)} ms` : "no declared deadlines", maxWcrt > 0 ? "ok" : "neutral")}`;
  }

  function statTile(label, value, sub, cls) {
    return `<article class="stat ${cls}">
      <div class="stat-label">${esc(label)}</div>
      <div class="stat-value">${value}</div>
      <div class="stat-sub">${esc(sub)}</div>
    </article>`;
  }

  function renderDialPanel(metrics) {
    const tasks = (metrics && metrics.tasks) || [];
    const withDeadline = tasks.filter((t) => Number(t.deadline) > 0);
    const maxWcrt = tasks.reduce((m, t) => Math.max(m, Number(t.wcrt) || 0), 0);
    let ratio = 0;
    let label = "WCRT / deadline";
    let mode = "ratio";
    if (withDeadline.length) {
      ratio = withDeadline.reduce((m, t) => Math.max(m, Number(t.wcrt) / Number(t.deadline)), 0);
    } else if (tasks.length && Number(metrics.hyperperiod) > 0) {
      ratio = maxWcrt / Number(metrics.hyperperiod);
      label = "WCRT / hyperperiod";
    } else if (tasks.length) {
      mode = "wcrt";
      label = "NO DEADLINE";
    }
    const stamp = $("#dial-stamp");
    let stampText = "NO DATA";
    let stampClass = "stamp";
    if (metrics && tasks.length) {
      if (mode === "wcrt") {
        stampText = metrics.exact === false ? "BOUND" : "EXACT";
        stampClass = metrics.exact === false ? "stamp warn" : "stamp ok";
      } else if (ratio > 1.0001) {
        stampText = "DEADLINE MISS";
        stampClass = "stamp err";
      } else if (metrics.exact === false || schedulabilityOf(metrics) !== "schedulable") {
        stampText = metrics.exact === false ? "BOUND" : "CHECK";
        stampClass = "stamp warn";
      } else {
        stampText = "CERTIFIED";
        stampClass = "stamp ok";
      }
    } else if (metrics) {
      stampText = "NO TASKS";
      stampClass = "stamp";
    }
    stamp.textContent = stampText;
    stamp.className = stampClass;
    setText($("#dial-label"), label);
    $("#dial").innerHTML = dialSvg(ratio, {
      hasData: !!metrics && tasks.length > 0,
      label,
      needle: mode !== "wcrt",
      zones: mode !== "wcrt",
      readoutMain: mode === "wcrt" ? fmtNum(maxWcrt) : (metrics && tasks.length ? Number(ratio).toFixed(2) : "-"),
      readoutUnit: mode === "wcrt" ? "ms" : "",
      sub: mode === "wcrt" ? "MAX WCRT" : "RATIO",
    });

    const comp = $("#dial-complications");
    if (comp) {
      if (!metrics) {
        comp.innerHTML = "";
      } else {
        const hp = Number(metrics.hyperperiod) > 0 ? `${fmtInt(metrics.hyperperiod)}<small> ms</small>` : "-";
        comp.innerHTML = `
          <dl class="dial-comp"><dt>exact</dt><dd>${metrics.exact ? "yes" : "no"}</dd></dl>
          <dl class="dial-comp"><dt>deadlocks</dt><dd>${fmtInt(deadlockCount(metrics))}</dd></dl>
          <dl class="dial-comp"><dt>states</dt><dd>${fmtInt(metrics.states)}</dd></dl>
          <dl class="dial-comp"><dt>period</dt><dd>${hp}</dd></dl>`;
      }
    }
  }

  function dialSvg(ratio, opts) {
    const W = 280;
    const H = 74;
    const maxRatio = 1.25;
    const hasNeedle = opts.needle !== false;
    const tClamped = clamp(Number(ratio) || 0, 0, maxRatio);
    const barX = 8;
    const barY = 44;
    const barW = W - 16;
    const barH = 10;
    const fillW = hasNeedle ? (tClamped / maxRatio) * barW : (opts.hasData ? barW * 0.65 : 0);
    const limitX = barX + (1.0 / maxRatio) * barW;
    const warnX = barX + (0.75 / maxRatio) * barW;
    const main = opts.readoutMain != null
      ? opts.readoutMain
      : (opts.hasData ? (Number(ratio) > 99 ? "99+" : Number(ratio).toFixed(2)) : "-");
    const unit = opts.readoutUnit ? ` ${opts.readoutUnit}` : "";
    const barColor = !opts.hasData
      ? "var(--line-2)"
      : !hasNeedle
        ? "var(--viz-1)"
        : ratio > 1.0001
          ? "var(--critical)"
          : ratio > 0.75
            ? "var(--warn)"
            : "var(--good)";

    const aria = opts.hasData
      ? `${opts.label || "response ratio"}: ${hasNeedle ? tClamped.toFixed(2) : `${main}${unit}`}`
      : "no data";

    return `<svg viewBox="0 0 ${W} ${H}" role="img" aria-label="${esc(aria)}">
      <text x="${barX}" y="24" fill="var(--ink)" font-size="22" font-weight="600" font-family="var(--font-mono)">${esc(main)}<tspan font-size="11" fill="var(--muted)">${esc(unit)}</tspan></text>
      <text x="${W - 8}" y="22" text-anchor="end" fill="var(--faint)" font-size="9.5" font-family="var(--font-mono)">${esc(opts.sub || "RATIO")}</text>
      <rect x="${barX}" y="${barY}" width="${barW}" height="${barH}" rx="3" fill="var(--viz-track)"/>
      ${fillW > 0 ? `<rect x="${barX}" y="${barY}" width="${fillW.toFixed(1)}" height="${barH}" rx="3" fill="${barColor}"/>` : ""}
      ${hasNeedle ? `
        <line x1="${warnX.toFixed(1)}" y1="${barY - 2}" x2="${warnX.toFixed(1)}" y2="${barY + barH + 2}" stroke="var(--line-3)" stroke-width="1" stroke-dasharray="2 2"/>
        <line x1="${limitX.toFixed(1)}" y1="${barY - 4}" x2="${limitX.toFixed(1)}" y2="${barY + barH + 4}" stroke="var(--critical)" stroke-width="1.5"/>
        <text x="${barX}" y="${H - 4}" fill="var(--faint)" font-size="8.5" font-family="var(--font-mono)">0.0</text>
        <text x="${warnX.toFixed(1)}" y="${H - 4}" text-anchor="middle" fill="var(--faint)" font-size="8.5" font-family="var(--font-mono)">0.75</text>
        <text x="${limitX.toFixed(1)}" y="${H - 4}" text-anchor="middle" fill="var(--critical)" font-size="8.5" font-family="var(--font-mono)">1.00 (deadline)</text>
      ` : `
        <text x="${barX}" y="${H - 4}" fill="var(--faint)" font-size="8.5" font-family="var(--font-mono)">Aperiodic bound (no relative deadline declared)</text>
      `}
    </svg>`;
  }

  function renderWcrtChart(metrics) {
    const root = $("#wcrt-chart");
    const legend = $("#wcrt-legend");
    legend.innerHTML = `
      <span class="legend-item"><i class="legend-swatch viz1"></i>WCRT interval</span>
      <span class="legend-item"><i class="legend-swatch viz2"></i>WCET</span>`;
    if (!metrics || !Array.isArray(metrics.tasks) || !metrics.tasks.length) {
      root.innerHTML = `<div class="empty"><strong>NO TIMING DATA</strong>Run an analysis to show response and execution times per task.</div>`;
      return;
    }
    const tasks = metrics.tasks;
    const W = 640;
    const left = 108;
    const right = 96;
    const top = 25;
    const rowH = 34;
    const H = top + tasks.length * rowH + 10;
    const plotW = W - left - right;
    const maxV = Math.max(1, ...tasks.flatMap((t) => [
      Number(t.wcrt) || 0,
      Number(t.wcrt_lb) || 0,
      Number(t.wcet) || 0,
    ])) * 1.12;
    const x = (v) => left + (clamp(Number(v) || 0, 0, maxV) / maxV) * plotW;
    const parts = [];
    parts.push(`<svg viewBox="0 0 ${W} ${H}" role="img" aria-label="Worst-case response time versus execution time per task">`);
    for (let i = 0; i <= 4; i += 1) {
      const v = (maxV / 4) * i;
      const gx = x(v);
      parts.push(`<line class="grid-line" x1="${gx.toFixed(1)}" y1="${top - 7}" x2="${gx.toFixed(1)}" y2="${H - 6}"/>`);
      parts.push(`<text class="axis-label" x="${gx.toFixed(1)}" y="12" text-anchor="middle">${fmtNum(v, 0)}</text>`);
    }
    tasks.forEach((t, i) => {
      const y = top + i * rowH;
      const wcrt = Number(t.wcrt) || 0;
      const lb = isNum(t.wcrt_lb) ? Number(t.wcrt_lb) : wcrt;
      const wcet = Number(t.wcet) || 0;
      const deadline = Number(t.deadline) || 0;
      const slack = isNum(t.slack) ? Number(t.slack) : null;
      const tip = `${t.name} | C${t.core} | pi${t.priority}\nWCRT ${fmtNum(lb)}-${fmtNum(wcrt)} ms | WCET ${fmtNum(wcet)} ms` +
        `${deadline ? `\ndeadline ${fmtNum(deadline)} ms | slack ${fmtNum(slack)} ms` : "\nno deadline"}` +
        `${t.worst_interference ? `\ninterference ${fmtNum(t.worst_interference)} ms` : ""}` +
        `${t.worst_blocking ? ` | blocking ${fmtNum(t.worst_blocking)} ms` : ""}`;
      parts.push(`<g class="row" data-tip="${esc(tip)}">`);
      parts.push(`<rect class="row-hit" x="0" y="${y - 2}" width="${W}" height="${rowH - 2}"/>`);
      parts.push(`<text class="row-label" x="${left - 10}" y="${y + 12}" text-anchor="end">${esc(truncate(t.name, 15))}</text>`);
      const sub = `C${t.core} | pi${t.priority}` + (deadline && slack != null ? ` | slack ${fmtNum(slack)}` : "");
      parts.push(`<text class="row-sub" x="${left - 10}" y="${y + 23}" text-anchor="end">${esc(sub)}</text>`);
      parts.push(`<rect class="track" x="${left}" y="${y + 6}" width="${plotW}" height="8" rx="1"/>`);
      const x0 = x(lb);
      const x1 = x(wcrt);
      if (x1 - x0 > 0.6) {
        parts.push(`<rect class="bar-interval" x="${x0.toFixed(1)}" y="${y + 6}" width="${(x1 - x0).toFixed(1)}" height="8" rx="1"/>`);
      }
      parts.push(`<line x1="${x(lb).toFixed(1)}" y1="${y + 4}" x2="${x(lb).toFixed(1)}" y2="${y + 16}" stroke="var(--viz-1)" stroke-width="1.3" opacity="0.65"/>`);
      parts.push(`<line x1="${x(wcrt).toFixed(1)}" y1="${y + 2}" x2="${x(wcrt).toFixed(1)}" y2="${y + 18}" stroke="var(--viz-1)" stroke-width="2.2"/>`);
      parts.push(`<line x1="${x(wcet).toFixed(1)}" y1="${y + 1}" x2="${x(wcet).toFixed(1)}" y2="${y + 19}" stroke="var(--viz-2)" stroke-width="1.8" stroke-dasharray="2 2"/>`);
      const labelX = Math.min(x(wcrt) + 6, W - right + 4);
      parts.push(`<text class="value-label" x="${labelX.toFixed(1)}" y="${y + 11}">${fmtNum(wcrt)}<tspan fill="var(--faint)" font-size="8"> ms</tspan></text>`);
      parts.push(`<text class="row-sub" x="${labelX.toFixed(1)}" y="${y + 22}">wcet ${fmtNum(wcet)}</text>`);
      if (deadline && wcrt > deadline) parts.push(`<circle cx="${left + 4}" cy="${y + 10}" r="2.2" fill="var(--critical)"/>`);
      parts.push(`</g>`);
    });
    parts.push(`</svg>`);
    root.innerHTML = parts.join("");
    bindTips(root);
  }

  function renderCoreChart(metrics) {
    const root = $("#core-chart");
    const legend = $("#core-legend");
    legend.innerHTML = `
      <span class="legend-item"><i class="legend-swatch viz1"></i>Utilization range</span>
      <span class="legend-item"><i class="legend-swatch viz2"></i>Graph busy fraction</span>`;
    const cores = (metrics && metrics.cores) || [];
    if (!cores.length) {
      root.innerHTML = `<div class="empty"><strong>NO CORE DATA</strong>Run an analysis to show per-core load.</div>`;
      return;
    }
    const W = 640;
    const left = 74;
    const right = 78;
    const top = 25;
    const rowH = 36;
    const H = top + cores.length * rowH + 10;
    const plotW = W - left - right;
    const maxPct = Math.max(10, Math.min(100, Math.ceil(Math.max(...cores.flatMap((c) => [Number(c.util_max) || 0, Number(c.graph_busy_fraction) || 0])) * 100 / 10) * 10));
    const x = (v) => left + (clamp((Number(v) || 0) * 100, 0, maxPct) / maxPct) * plotW;
    const parts = [];
    parts.push(`<svg viewBox="0 0 ${W} ${H}" role="img" aria-label="Core utilization">`);
    for (let i = 0; i <= 2; i += 1) {
      const v = (maxPct / 2) * i;
      const gx = x(v / 100);
      parts.push(`<line class="grid-line" x1="${gx.toFixed(1)}" y1="${top - 7}" x2="${gx.toFixed(1)}" y2="${H - 6}"/>`);
      parts.push(`<text class="axis-label" x="${gx.toFixed(1)}" y="12" text-anchor="middle">${fmtNum(v, 0)}%</text>`);
    }
    cores.forEach((c, i) => {
      const y = top + i * rowH;
      const min = Number(c.util_min) || 0;
      const max = Number(c.util_max) || 0;
      const busy = Number(c.graph_busy_fraction) || 0;
      const tip = `core ${c.core}\nutilization ${fmtPct(min)}-${fmtPct(max)}\ngraph busy fraction ${fmtPct(busy, 2)}`;
      parts.push(`<g class="row" data-tip="${esc(tip)}">`);
      parts.push(`<rect class="row-hit" x="0" y="${y - 2}" width="${W}" height="${rowH - 2}"/>`);
      parts.push(`<text class="row-label" x="${left - 10}" y="${y + 12}" text-anchor="end">core ${esc(c.core)}</text>`);
      parts.push(`<text class="row-sub" x="${left - 10}" y="${y + 23}" text-anchor="end">busy ${fmtPct(busy, 1)}</text>`);
      parts.push(`<rect class="track" x="${left}" y="${y + 6}" width="${plotW}" height="8" rx="1"/>`);
      const bx0 = x(min);
      const bx1 = x(max);
      if (bx1 - bx0 < 2) {
        parts.push(`<line x1="${bx0.toFixed(1)}" y1="${y + 3}" x2="${bx0.toFixed(1)}" y2="${y + 17}" stroke="var(--viz-1)" stroke-width="2.2"/>`);
      } else {
        parts.push(`<rect class="bar-series" x="${bx0.toFixed(1)}" y="${y + 6}" width="${(bx1 - bx0).toFixed(1)}" height="8" rx="1"/>`);
      }
      parts.push(`<line x1="${x(busy).toFixed(1)}" y1="${y + 2}" x2="${x(busy).toFixed(1)}" y2="${y + 18}" stroke="var(--viz-2)" stroke-width="1.8"/>`);
      parts.push(`<text class="value-label" x="${W - 2}" y="${y + 13}" text-anchor="end">${fmtPct(min, 0)}-${fmtPct(max, 1)}</text>`);
      parts.push(`</g>`);
    });
    parts.push(`</svg>`);
    root.innerHTML = parts.join("");
    bindTips(root);
  }

  function renderLockMini(metrics) {
    const root = $("#lock-table");
    const locks = (metrics && metrics.locks) || [];
    if (!locks.length) {
      root.innerHTML = `<div class="empty"><strong>NO LOCKS</strong>The model declares no mutex locks.</div>`;
      return;
    }
    root.innerHTML = `
      <div class="mini-row head"><span>lock</span><span class="num">worst</span><span class="num">total</span><span class="num">wait</span></div>
      ${locks.map((l) => `<div class="mini-row"><span class="name">${esc(l.name)}</span><span class="num">${fmtNum(l.worst_hold)}</span><span class="num">${fmtNum(l.total_hold)}</span><span class="num">${fmtNum(l.total_wait)}</span></div>`).join("")}`;
  }

  function renderOverviewDiagnostics() {
    const root = $("#overview-diagnostics");
    if (!state.lastResponse) {
      root.innerHTML = `<div class="note">No run yet. Select a model and press Analyze.</div>`;
      return;
    }
    if (!state.diagnostics.length && !state.pending.length) {
      root.innerHTML = `<div class="signal ok"><i></i><span class="signal-text">CLEAN | no diagnostics</span></div>`;
      return;
    }
    const items = [];
    for (const d of state.diagnostics.slice(0, 6)) {
      items.push(diagHtml(d));
    }
    if (!state.diagnostics.length && state.pending.length) {
      items.push(`<div class="signal pending"><i></i><span class="signal-text">${state.pending.length} pending measurement${state.pending.length > 1 ? "s" : ""}</span></div>`);
    }
    if (state.diagnostics.length > 6) {
      items.push(`<div class="note">${state.diagnostics.length - 6} more diagnostics; see the Model tab.</div>`);
    }
    root.innerHTML = items.join("");
    bindDiagClicks(root);
  }

  /* -------------------------------------------------------------- diagnostics */

  function diagHtml(d) {
    const sev = severityClass(d.severity);
    const loc = d.location ? `${d.location.line}:${d.location.column}` : "";
    return `<button type="button" class="diag ${sev}" data-line="${d.location ? d.location.line : ""}" data-col="${d.location ? d.location.column : ""}" data-code="${esc(d.code || "")}">
      <span class="diag-top"><span class="diag-code">${esc(d.code || d.category || "DIAG")}</span><span class="diag-loc">${esc(loc)}</span></span>
      <span class="diag-msg">${esc(d.message || "")}</span>
      ${d.fix ? `<span class="diag-fix">fix: ${esc(d.fix)}</span>` : ""}
      ${d.subject ? `<span class="diag-subject">${esc(d.subject)}</span>` : ""}
    </button>`;
  }

  function renderDiagnostics() {
    const root = $("#diagnostics");
    setText($("#diag-count"), `${state.diagnostics.length}`);
    if (!state.lastResponse) {
      root.innerHTML = `<div class="empty"><strong>NO RUN</strong>Run Check or Analyze to show diagnostics.</div>`;
      return;
    }
    if (!state.diagnostics.length) {
      root.innerHTML = `<div class="signal ok"><i></i><span class="signal-text">CLEAN | no diagnostics</span></div>`;
      return;
    }
    root.innerHTML = state.diagnostics.map(diagHtml).join("");
    bindDiagClicks(root);
  }

  function bindDiagClicks(root) {
    $$(".diag", root).forEach((el) => {
      el.addEventListener("click", () => {
        const line = parseInt(el.dataset.line, 10);
        const col = parseInt(el.dataset.col, 10);
        if (line) {
          setActiveTab("model");
          focusLine(line, col || 1);
        }
      });
    });
  }

  function renderSignals() {
    const root = $("#signal-list");
    const note = $("#signals-note");
    const errors = state.diagnostics.filter((d) => d.severity === "error").length;
    const warnings = state.diagnostics.filter((d) => d.severity === "warning").length;
    const pending = state.pending.length;
    if (!state.lastResponse) {
      root.innerHTML = `<div class="signal"><i></i><span class="signal-text">idle - no run yet</span></div>`;
      note.textContent = "idle";
      return;
    }
    const items = [];
    if (errors) items.push(`<div class="signal error"><i></i><span class="signal-text">errors</span><span class="count">${errors}</span></div>`);
    if (warnings) items.push(`<div class="signal warning"><i></i><span class="signal-text">warnings</span><span class="count">${warnings}</span></div>`);
    if (pending) items.push(`<div class="signal pending"><i></i><span class="signal-text">pending measurements</span><span class="count">${pending}</span></div>`);
    if (!items.length) {
      items.push(`<div class="signal ok"><i></i><span class="signal-text">clean - no diagnostics</span><span class="count">0</span></div>`);
      if (state.response && state.response.metrics) {
        const schedState = schedulabilityOf(state.response.metrics);
        const schedText = schedState === "schedulable" ? "schedulable" : schedState === "unschedulable" ? "not schedulable" : "schedulability unknown";
        items.push(`<div class="signal ${schedState === "schedulable" ? "ok" : schedState === "unschedulable" ? "error" : "warning"}"><i></i><span class="signal-text">${schedText}</span><span class="count">${fmtInt(state.response.metrics.states)}</span></div>`);
      }
    }
    root.innerHTML = items.join("");
    note.textContent = errors ? "attention" : pending ? "pending" : "clean";
  }

  function renderPending() {
    const root = $("#pending-list");
    const banner = $("#pending-banner");
    setText($("#pending-count-badge"), `${state.pending.length}`);
    if (state.pending.length && !state.diagnostics.length) {
      switchTray("pending");
    } else if (state.diagnostics.length) {
      switchTray("diagnostics");
    }
    if (!state.pending.length) {
      root.innerHTML = `<div class="pending-empty">No pending measurements. ${state.lastResponse ? "" : "Run Check to inspect pending_measurements."}</div>`;
      if (banner) {
        banner.hidden = true;
        banner.innerHTML = "";
      }
      return;
    }
    if (banner) {
      const first = state.pending[0];
      const segs = first.expected_segments || 1;
      const loc = first.inject ? `line ${first.inject.line}` : "source";
      const extra = state.pending.length > 1 ? ` | ${state.pending.length - 1} more` : "";
      banner.hidden = false;
      banner.innerHTML = `<div class="pb-text"><span class="pb-eyebrow">Pending measurement</span><strong>${esc(first.task || "task")}</strong> needs ${segs} execution segments${extra} | ${esc(loc)}</div><button class="btn primary" type="button" data-pending-banner>Fill measurements</button>`;
      const btn = banner.querySelector("[data-pending-banner]");
      if (btn) btn.addEventListener("click", () => openPendingModal(first));
    }
    root.innerHTML = state.pending.map((pm, idx) => {
      const segs = pm.expected_segments || 1;
      const loc = pm.inject ? `${pm.inject.line}:${pm.inject.column}` : "-";
      return `<div class="pending-card">
        <div class="pending-title"><strong>${esc(pm.task || "task")}</strong><span class="chip mono">${segs} segment${segs > 1 ? "s" : ""}</span></div>
        <div class="pending-meta">measured by llm | runs ${esc(pm.runs ?? "-")} | timeout ${esc(pm.timeout ?? "-")} | line ${esc(loc)}</div>
        <div class="pending-actions">
          <button class="btn ghost" type="button" data-pending="${idx}">Fill measurements</button>
          <span class="note">or replace directly in the source <code class="code-inline">time measured by llm {...}</code></span>
        </div>
      </div>`;
    }).join("");
    $$("[data-pending]", root).forEach((btn) => {
      btn.addEventListener("click", () => openPendingModal(state.pending[Number(btn.dataset.pending)]));
    });
  }

  /* ------------------------------------------------------------------ editor */

  function setEditorSource(text, opts = {}) {
    const ta = $("#code-input");
    ta.value = text;
    state.source = text;
    if (!opts.keepScroll) {
      ta.scrollTop = 0;
      ta.scrollLeft = 0;
    }
    renderCode();
    updateEditorMeta();
  }

  function updateEditorMeta() {
    setText($("#editor-title"), state.filename || "model.rtsl");
    const kindBadge = state.kind === "tdg" ? "TDG JSON" : state.kind === "ptpn" ? "NATIVE PTPN" : "RTSL";
    setText($("#editor-kind"), kindBadge);
    setText($("#editor-lines"), `${countLines(state.source)} lines${state.dirty ? " | edited" : ""}`);
    const check = $("#btn-check");
    if (check && !state.busy) {
      check.disabled = state.kind !== "rtsl" || (!state.demo && state.status && state.status.binary_exists === false);
    }
  }

  const RTSL_KEYWORDS = new Set([
    "system", "task", "cpus", "cores_per_cpu", "time_unit", "policy", "period", "deadline",
    "core", "time", "locks", "start", "end", "measured", "by", "estimated", "certified",
    "runs", "timeout", "any", "periodic", "aperiodic", "normal", "fixed", "rm", "dm",
    "pip", "pcp", "srp", "edf", "fifo", "llf", "resume", "restart", "ms", "s",
    "places", "transitions", "suspendable", "priority", "init", "capacity",
  ]);

  function highlightLine(line, kind) {
    const re = kind === "tdg"
      ? /("(?:[^"\\]|\\.)*")|(-?\b\d+(?:\.\d+)?(?:[eE][+-]?\d+)?\b)|\b(true|false|null)\b|([{}\[\],:])/g
      : /(\/\/[^\n]*)|("(?:[^"\\]|\\.)*")|(\b\d+(?:\.\d+)?(?:ms|s)?\b)|([A-Za-z_][A-Za-z0-9_]*)|(->|[{}()\[\];,.:=<>-])/g;
    let out = "";
    let last = 0;
    let m;
    while ((m = re.exec(line)) !== null) {
      out += esc(line.slice(last, m.index));
      const tok = m[0];
      let cls = "";
      if (kind === "tdg") {
        if (m[1]) {
          const rest = line.slice(re.lastIndex);
          cls = /^\s*:/.test(rest) ? "tok-key" : "tok-str";
        } else if (m[2]) cls = "tok-num";
        else if (m[3]) cls = "tok-bool";
        else if (m[4]) cls = "tok-punc";
      } else {
        if (m[1]) cls = "tok-com";
        else if (m[2]) cls = "tok-str";
        else if (m[3]) cls = "tok-num";
        else if (m[4]) cls = RTSL_KEYWORDS.has(tok) ? "tok-kw" : "";
        else if (m[5]) cls = "tok-punc";
      }
      out += cls ? `<span class="${cls}">${esc(tok)}</span>` : esc(tok);
      last = re.lastIndex;
    }
    out += esc(line.slice(last));
    return out || " ";
  }

  function renderCode() {
    const ta = $("#code-input");
    const view = $("#code-view");
    const gutter = $("#gutter-inner");
    const text = ta.value;
    const lines = text.split("\n");
    const errLines = new Set();
    const warnLines = new Set();
    const pendingLines = new Set();
    for (const d of state.diagnostics) {
      if (!d.location) continue;
      if (d.severity === "error") errLines.add(d.location.line);
      else if (d.severity === "warning") warnLines.add(d.location.line);
    }
    for (const pm of state.pending) {
      if (pm.inject) pendingLines.add(pm.inject.line);
    }
    view.innerHTML = lines.map((line, idx) => {
      const n = idx + 1;
      const cls = [
        "cline",
        errLines.has(n) ? "err" : "",
        !errLines.has(n) && warnLines.has(n) ? "warn" : "",
        !errLines.has(n) && !warnLines.has(n) && pendingLines.has(n) ? "pending" : "",
      ].filter(Boolean).join(" ");
      return `<span class="${cls}">${highlightLine(line, state.kind)}</span>`;
    }).join("");
    gutter.innerHTML = lines.map((_, idx) => {
      const n = idx + 1;
      const cls = errLines.has(n) ? "err" : warnLines.has(n) ? "warn" : "";
      return `<div class="gutter-line ${cls}">${n}</div>`;
    }).join("");
    syncScroll();
  }

  function syncScroll() {
    const ta = $("#code-input");
    const view = $("#code-view");
    const gutter = $("#gutter-inner");
    if (!ta || !view || !gutter) return;
    view.scrollTop = ta.scrollTop;
    view.scrollLeft = ta.scrollLeft;
    gutter.style.transform = `translateY(${-ta.scrollTop}px)`;
  }

  function focusLine(line, col) {
    const ta = $("#code-input");
    const lines = ta.value.split("\n");
    const target = clamp(line, 1, lines.length);
    let index = 0;
    for (let i = 0; i < target - 1; i += 1) index += lines[i].length + 1;
    index += clamp((col || 1) - 1, 0, lines[target - 1].length);
    ta.focus();
    ta.setSelectionRange(index, index);
    const lineHeight = parseFloat(getComputedStyle(ta).lineHeight) || 20;
    ta.scrollTop = Math.max(0, (target - 4) * lineHeight);
    syncScroll();
  }

  function byteToCharIndex(text, byteOffset) {
    if (!isNum(byteOffset)) return -1;
    const enc = new TextEncoder();
    let bytes = 0;
    let i = 0;
    while (i < text.length) {
      if (bytes >= Number(byteOffset)) return i;
      const cp = text.codePointAt(i);
      const ch = String.fromCodePoint(cp);
      bytes += enc.encode(ch).length;
      i += ch.length;
    }
    return bytes >= Number(byteOffset) ? text.length : -1;
  }

  function openPendingModal(pm) {
    if (!pm) return;
    const segs = Math.max(1, Number(pm.expected_segments) || 1);
    const rows = Array.from({ length: segs }, (_, i) => `
      <div class="seg-row">
        <span>Seg ${i + 1}</span>
        <input type="number" min="0" step="1" id="seg-min-${i}" value="1" placeholder="min ms" inputmode="numeric">
        <input type="number" min="0" step="1" id="seg-max-${i}" value="${i === 1 ? 2 : 1}" placeholder="max ms" inputmode="numeric">
      </div>`).join("");
    const html = `
      <p>Task <code class="code-inline">${esc(pm.task || "task")}</code> declares <code class="code-inline">measured by llm</code>.
      It requires <strong>${segs}</strong> execution segment(s) (2 * locks + 1). Select the timing confirmation method:</p>
      <div class="template-cards" id="modal-timing-sources">
        <button type="button" class="template-card selected" data-tsrc="manual">
          <strong>01. User Manual Bound</strong>
          <small>Supply datasheet or trusted bounds (<code class="code-inline">estimated by "user"</code> or <code class="code-inline">certified by "user"</code>)</small>
        </button>
        <button type="button" class="template-card" data-tsrc="llm">
          <strong>02. LLM Single-Core Benchmark Run</strong>
          <small>Run single-core benchmark code generated by the LLM and preserve <code class="code-inline">measured by llm</code> provenance</small>
        </button>
        <button type="button" class="template-card" data-tsrc="tool">
          <strong>03. LLM Single-Core Harness + External WCET Analyzer</strong>
          <small>Measure LLM-generated harness with an external timing tool (<code class="code-inline">estimated/certified by "external-tool"</code>)</small>
        </button>
      </div>
      <div class="seg-grid">${rows}</div>
      <label class="opt-check" id="modal-certify-row" style="margin-top:8px">
        <input type="checkbox" id="modal-certified">
        <span>Certify these intervals as a verified safe WCET upper bound (writes <code class="code-inline">certified by</code>)</span>
      </label>
      <div class="modal-actions">
        <button class="btn ghost" type="button" id="pending-cancel">Cancel</button>
        <button class="btn primary" type="button" id="pending-apply">Fill Timing and Analyze</button>
      </div>`;
    let chosenSource = "manual";
    openModal({ eyebrow: "Timing Skill Confirmation", title: `Confirm timing for task: ${pm.task || "task"}`, html, onMount: () => {
      $$("#modal-timing-sources .template-card").forEach((card) => {
        card.addEventListener("click", () => {
          chosenSource = card.dataset.tsrc;
          $$("#modal-timing-sources .template-card").forEach((c) => c.classList.toggle("selected", c === card));
          const certInput = $("#modal-certified");
          if (certInput) {
            certInput.disabled = chosenSource === "llm";
            if (chosenSource === "llm") certInput.checked = false;
          }
        });
      });
      $("#pending-cancel").addEventListener("click", closeModal);
      $("#pending-apply").addEventListener("click", () => {
        const values = [];
        for (let i = 0; i < segs; i += 1) {
          const minRaw = $(`#seg-min-${i}`).value.trim();
          const maxRaw = $(`#seg-max-${i}`).value.trim();
          if (!minRaw || !maxRaw) {
            showToast(`Segment ${i + 1} is missing min or max.`, "err");
            return;
          }
          const min = parseInt(minRaw, 10);
          const max = parseInt(maxRaw, 10);
          if (!Number.isFinite(min) || !Number.isFinite(max) || min < 0 || max < min) {
            showToast(`Segment ${i + 1} has an invalid interval (require 0 <= min <= max).`, "err");
            return;
          }
          values.push([min, max]);
        }
        const certified = $("#modal-certified") && $("#modal-certified").checked;
        let prov = 'estimated by "user"';
        if (chosenSource === "llm") {
          prov = `measured by llm { runs ${pm.runs || 10}, timeout ${pm.timeout || 2} }`;
        } else if (chosenSource === "manual") {
          prov = certified ? 'certified by "user"' : 'estimated by "user"';
        } else {
          prov = certified ? 'certified by "wcet-tool"' : 'estimated by "external-tool"';
        }
        const repl = values.length === 1
          ? `time [${values[0][0]}, ${values[0][1]}] ${prov}`
          : `time [ ${values.map(([a, b]) => `[${a}, ${b}]`).join(", ")} ] ${prov}`;
        let start = pm.inject ? byteToCharIndex(state.source, pm.inject.byte_start) : -1;
        let end = pm.inject ? byteToCharIndex(state.source, pm.inject.byte_end) : -1;
        if (start < 0 || end < start) {
          const idx = state.source.indexOf("time measured by llm");
          if (idx < 0) {
            showToast("No replaceable time statement was found; edit the source directly.", "err");
            return;
          }
          const lineEnd = state.source.indexOf("\n", idx);
          start = idx;
          end = lineEnd > idx ? lineEnd : idx + "time measured by llm".length;
        }
        const next = state.source.slice(0, start) + repl + state.source.slice(end);
        closeModal();
        setEditorSource(next);
        state.dirty = state.source !== state.originalSource;
        updateEditorMeta();
        showToast("Execution timing intervals injected. Running analysis...", "ok");
        if (!state.demo) runAnalysis("analyze", { keepTab: false });
        else showToast("Demo source updated. Start the local server to re-analyze.", "ok");
      });
    } });
    setTimeout(() => { const first = $("#seg-min-0"); if (first) first.focus(); }, 30);
  }

  function openLlmRepairModal() {
    const resp = state.lastResponse || state.response || {};
    const analysis = resp.analysis || {};
    const metrics = resp.metrics || {};
    const diags = state.diagnostics || [];
    const tasks = analysis.tasks || metrics.tasks || [];
    const codes = diags.map((d) => `${d.code}${d.subject ? ` (${d.subject})` : ""}: ${d.message || ""}`).join("\n  - ") || "none";
    const taskLines = tasks.map((t) => {
      const name = t.task || t.name;
      const witness = Array.isArray(t.witness) && t.witness.length ? ` | witness: ${t.witness.join(" -> ")}` : "";
      return `  - task ${name}: verdict=${t.verdict || "-"}, WCRT=${t.wcrt ?? "-"}ms, WCET=${t.wcet ?? "-"}ms, deadline=${t.deadline ?? "-"}ms, priority=${t.priority ?? "-"}, core=${t.core ?? "-"}${witness}`;
    }).join("\n") || "  - (no per-task metrics)";

    const promptText = [
      "Revise the real-time architecture model below based on the formal PTPN verification report:",
      "",
      "=== 1. FACTS FROM THE PTPN ANALYZER ===",
      `- File and format: ${state.filename} (${state.kind.toUpperCase()})`,
      `- Exit code: ${resp.exit_code ?? "-"}`,
      `- System schedulability: ${analysis.schedulability || schedulabilityOf(metrics)}`,
      `- Exact / truncated: exact=${metrics.exact ?? analysis.exact ?? "-"}, truncated=${analysis.truncated ?? metrics.truncated ?? false}`,
      `- Diagnostics:\n  - ${codes}`,
      `- Per-task verdicts and counterexample witness paths:\n${taskLines}`,
      "",
      "=== 2. CURRENT MODEL SOURCE ===",
      state.source,
      "",
      "=== 3. REVISION REQUIREMENTS ===",
      "- Do NOT silently alter measured/estimated [BCET, WCET] execution intervals or fabricate certified provenance;",
      "- Propose a concrete architectural fix addressing the witness path (e.g., core allocation, priority adjustment, or period/deadline change with user confirmation) and provide the updated source.",
    ].join("\n");

    const html = `
      <p>The analyzer's exit code, diagnostics, per-task WCRT bounds, and counterexample witness paths have been packaged into a structured prompt for LLM revision:</p>
      <pre class="zone" id="llm-repair-prompt-pre" style="max-height:320px;margin:10px 0;white-space:pre-wrap">${esc(promptText)}</pre>
      <div class="modal-actions">
        <button class="btn ghost" type="button" id="repair-modal-close">Close</button>
        <button class="btn primary" type="button" id="repair-modal-copy">Copy LLM Repair Prompt</button>
      </div>`;
    openModal({
      eyebrow: "LLM Architecture Feedback Loop",
      title: "Formal Verification Repair Prompt",
      html,
      onMount: () => {
        $("#repair-modal-close").addEventListener("click", closeModal);
        $("#repair-modal-copy").addEventListener("click", () => {
          copyText(promptText, "LLM Repair Prompt");
        });
      },
    });
  }

  /* ------------------------------------------------------------------ modal */

  function openModal({ eyebrow = "Action", title = "", html = "", onMount }) {
    state.modalReturn = document.activeElement;
    setText($("#modal-eyebrow"), eyebrow);
    setText($("#modal-title"), title);
    $("#modal-body").innerHTML = html;
    $("#modal").hidden = false;
    document.body.style.overflow = "hidden";
    if (onMount) onMount();
    setTimeout(() => {
      const focusable = $("#modal-body input, #modal-body button, #modal-close");
      if (focusable) focusable.focus();
    }, 20);
  }

  function closeModal() {
    $("#modal").hidden = true;
    document.body.style.overflow = "";
    if (state.modalReturn && state.modalReturn.focus) state.modalReturn.focus();
  }

  function openDemoHint() {
    openModal({
      eyebrow: "Demo mode",
      title: "Run the full analyzer locally",
      html: `
        <p>This is a shareable demo: models and results are precomputed, and the browser cannot start the C++ analyzer.</p>
        <p>Start the local server in the repository to analyze any RTSL / TDG model:</p>
        <p><code class="code-inline">./example/build.py && python3 webapp/server.py</code></p>
        <p>Then open <code class="code-inline">http://127.0.0.1:8765</code>. Editing, Check, Analyze and exports are all available.</p>
        <div class="modal-actions">
          <button class="btn primary" type="button" id="demo-ok">Got it</button>
        </div>`,
      onMount: () => $("#demo-ok").addEventListener("click", closeModal),
    });
  }

  function openHelp() {
    openModal({
      eyebrow: "Reference",
      title: "Workbench guide",
      html: `
        <p>PTPN Chronograph sends RTSL/TDG models to <code class="code-inline">build/ptpn</code> and displays TDG, PTPN, SCG and WCRT metrics. The demo only replays precomputed data.</p>
        <div class="help-keys">
          <div class="help-key"><span>Analyze</span><kbd>Cmd+ / Ctrl + Enter</kbd></div>
          <div class="help-key"><span>Check (parse + validate)</span><kbd>Cmd+ / Ctrl + Shift + Enter</kbd></div>
          <div class="help-key"><span>Switch view</span><kbd>1 - 6</kbd></div>
          <div class="help-key"><span>Pan graph</span><kbd>drag</kbd></div>
          <div class="help-key"><span>Zoom graph</span><kbd>wheel</kbd></div>
        </div>
        <p class="note" style="margin-top:12px">RTSL time is normalized to milliseconds. EDF/FIFO/LLF are limited by their supported classes; extrapolation and symmetry apply only to the transition-indexed layouts of static/PIP/PCP/SRP rules.</p>
        <div class="modal-actions">
          <button class="btn ghost" type="button" id="help-close">Close</button>
        </div>`,
      onMount: () => $("#help-close").addEventListener("click", closeModal),
    });
  }

  /* ------------------------------------------------------------------ charts */

  function bindTips(root) {
    $$("[data-tip]", root).forEach((el) => {
      el.addEventListener("pointerenter", (ev) => showTip(el.dataset.tip, ev.clientX, ev.clientY));
      el.addEventListener("pointermove", (ev) => moveTip(ev.clientX, ev.clientY));
      el.addEventListener("pointerleave", hideTip);
    });
  }

  function showTip(text, x, y) {
    if (!state.tip) {
      state.tip = document.createElement("div");
      state.tip.className = "tip";
      document.body.appendChild(state.tip);
    }
    state.tip.textContent = text || "";
    state.tip.hidden = !text;
    moveTip(x, y);
  }

  function moveTip(x, y) {
    if (!state.tip || state.tip.hidden) return;
    const pad = 14;
    const w = state.tip.offsetWidth;
    const h = state.tip.offsetHeight;
    let left = x + pad;
    let top = y + pad;
    if (left + w > window.innerWidth - 8) left = x - w - pad;
    if (top + h > window.innerHeight - 8) top = y - h - pad;
    state.tip.style.left = `${Math.max(8, left)}px`;
    state.tip.style.top = `${Math.max(8, top)}px`;
  }

  function hideTip() {
    if (state.tip) state.tip.hidden = true;
  }

  /* ------------------------------------------------------------------ graphs */

  function graphTitle(kind) {
    return kind === "tdg" ? "TDG" : kind === "ptpn" ? "PTPN" : "State-class graph";
  }

  function getGraphView(kind) {
    if (state.views[kind]) return state.views[kind];
    const stage = $(`#${kind}-stage`);
    const hint = $(`#${kind}-hint`);
    const legend = $(`#${kind}-legend`);
    const view = new GraphView(stage, {
      kind,
      hint,
      legend,
      onSelect: (id) => selectGraphNode(kind, id),
      ariaLabel: graphTitle(kind),
    });
    state.views[kind] = view;
    return view;
  }

  function renderGraphs() {
    renderTdg();
    renderPtpn();
    renderScg();
  }

  function setGraphHint(kind, html) {
    const hint = $(`#${kind}-hint`);
    if (html) {
      hint.innerHTML = html;
      hint.hidden = false;
    } else {
      hint.hidden = true;
    }
  }

  function renderTdg() {
    const view = getGraphView("tdg");
    const legend = $("#tdg-legend");
    const tdg = state.response && state.response.tdg;
    if (!tdg || !Array.isArray(tdg.nodes) || !tdg.nodes.length) {
      view.clear();
      setGraphHint("tdg", `<div><strong>NO TDG</strong>Run an analysis to show the task dependency graph.</div>`);
      legend.innerHTML = "";
      renderTdgInspector(null);
      state.graphs.tdg = null;
      return;
    }
    const graph = buildTdgGraph(tdg);
    state.graphs.tdg = graph;
    setGraphHint("tdg", "");
    legend.innerHTML = `
      <span class="legend-item"><i class="legend-swatch viz2"></i>task</span>
      <span class="legend-item"><i class="legend-swatch viz3"></i>fork / join</span>
      <span class="legend-item"><i class="legend-swatch" style="background:var(--brass)"></i>start / end</span>
      <span class="legend-item"><i class="legend-swatch" style="background:var(--brass-dim)"></i>lock holder</span>
      <span class="legend-item">Core color bar C0-C2</span>`;
    view.setData(graph, "tdg");
    if (state.selected.tdg) view.select(state.selected.tdg);
    renderTdgInspector(state.selected.tdg);
  }

  function renderPtpn() {
    const view = getGraphView("ptpn");
    const legend = $("#ptpn-legend");
    const graph = state.response && state.response.graphs && state.response.graphs.ptpn;
    if (!graph || !Array.isArray(graph.nodes) || !graph.nodes.length) {
      view.clear();
      const truncated = graph && graph.truncated;
      setGraphHint("ptpn", truncated
        ? `<div><strong>GRAPH OMITTED</strong>${fmtInt(graph.node_count)} places/transitions exceed the render limit.</div>`
        : `<div><strong>NO PTPN</strong>Run an analysis to show the timed Petri net structure.</div>`);
      legend.innerHTML = "";
      renderPtpnInspector(null);
      state.graphs.ptpn = null;
      return;
    }
    setGraphHint("ptpn", "");
    legend.innerHTML = `
      <span class="legend-item"><i class="legend-swatch viz2"></i>place</span>
      <span class="legend-item"><i class="legend-swatch viz1"></i>execution</span>
      <span class="legend-item"><i class="legend-swatch" style="background:var(--brass)"></i>timing / lock</span>
      <span class="legend-item"><i class="legend-swatch" style="background:var(--muted)"></i>control</span>
      <span class="legend-item"><i class="legend-swatch viz3"></i>resource / period</span>
      <span class="legend-item">Drag to pan | wheel to zoom</span>`;
    view.setData(graph, "ptpn", { focus: true });
    view.setEdgeLabels(state.filters.ptpnLabels);
    if (state.selected.ptpn) view.select(state.selected.ptpn);
    renderPtpnInspector(state.selected.ptpn);
  }

  function renderScg() {
    const view = getGraphView("scg");
    const legend = $("#scg-legend");
    const graph = state.response && state.response.graphs && state.response.graphs.scg;
    if (!graph || !Array.isArray(graph.nodes) || !graph.nodes.length) {
      view.clear();
      const truncated = graph && graph.truncated;
      setGraphHint("scg", truncated
        ? `<div><strong>GRAPH OMITTED</strong>${fmtInt(graph.node_count)} state classes exceed the render limit. Metrics remain available.</div>`
        : `<div><strong>NO SCG</strong>Run an analysis to show the state-class reachability graph.</div>`);
      legend.innerHTML = "";
      renderScgInspector(null);
      state.graphs.scg = null;
      return;
    }
    setGraphHint("scg", "");
    legend.innerHTML = `
      <span class="legend-item"><i class="legend-swatch viz2"></i>state class</span>
      <span class="legend-item"><i class="legend-swatch err"></i>deadlock</span>
      <span class="legend-item"><i class="legend-swatch" style="background:#3b4654"></i>dwell 0</span>
      <span class="legend-item"><i class="legend-swatch" style="background:#8a6a45"></i>dwell 1-4</span>
      <span class="legend-item"><i class="legend-swatch viz1"></i>dwell 5+</span>
      <span class="legend-item">Drag to pan | wheel to zoom</span>`;
    view.setData(graph, "scg");
    view.emphasizeDeadlocks(state.filters.scgDeadlocks);
    if (state.selected.scg) view.select(state.selected.scg);
    renderScgInspector(state.selected.scg);
  }

  function selectGraphNode(kind, id) {
    state.selected[kind] = id;
    const view = state.views[kind];
    if (view) view.select(id);
    if (kind === "tdg") renderTdgInspector(id);
    else if (kind === "ptpn") renderPtpnInspector(id);
    else renderScgInspector(id);
  }

  function renderTdgInspector(id) {
    const root = $("#tdg-inspector");
    const graph = state.graphs.tdg;
    if (!graph || !id) {
      root.innerHTML = `<div class="inspector-empty"><strong>SELECT A NODE</strong><span>Click a task, fork or join to inspect details.</span></div>`;
      return;
    }
    const node = graph.nodes.find((n) => n.id === id);
    if (!node) {
      root.innerHTML = `<div class="inspector-empty"><strong>UNKNOWN NODE</strong><span>${esc(id)}</span></div>`;
      return;
    }
    const raw = node.raw || {};
    const kind = node.type || "task";
    const incoming = graph.edges.filter((e) => e.to === id);
    const outgoing = graph.edges.filter((e) => e.from === id);
    const segs = Array.isArray(raw.time) ? raw.time : [];
    const interval = segs.length ? `[${sumSegments(segs, 0)}, ${sumSegments(segs, 1)}] ms` : "-";
    const periodic = graph.meta && graph.meta.periodicMap && graph.meta.periodicMap[id];
    const parts = [];
    parts.push(`<div class="inspector-title"><h4>${esc(id)}</h4><span class="inspector-kind">${esc(kind)}</span></div>`);
    if (kind === "task") {
      parts.push(`<dl class="kv">
        <dt>core</dt><dd>${raw.core === -1 || raw.core == null ? "control" : esc(raw.core)}</dd>
        <dt>priority</dt><dd>${esc(raw.priority ?? "-")}</dd>
        <dt>segments</dt><dd>${segs.length || 1}</dd>
        <dt>interval</dt><dd>${esc(interval)}</dd>
        <dt>period</dt><dd>${(raw.period || periodic) ? `${esc(raw.period || periodic)} ms` : "aperiodic"}</dd>
        <dt>deadline</dt><dd>${isNum(raw.deadline) && Number(raw.deadline) > 0 ? `${esc(raw.deadline)} ms` : "-"}</dd>
        <dt>locks</dt><dd>${(raw.locks || []).length ? esc((raw.locks || []).join(", ")) : "none"}</dd>
        <dt>allowed</dt><dd>${(raw.allowed_cores || []).length ? esc((raw.allowed_cores || []).join(", ")) : "-"}</dd>
        <dt>wcet</dt><dd>${esc(raw.wcet_kind || "-")}${raw.wcet_method ? ` | ${esc(raw.wcet_method)}` : ""}</dd>
        <dt>role</dt><dd>${esc((node.roles || []).join(" / ") || "task")}</dd>
      </dl>`);
      if (segs.length > 1) {
        parts.push(`<div class="inspector-section"><h5>Segments</h5><div class="edge-list">${segs.map((s, i) => `<li><span>seg ${i + 1}</span><span class="dwell">[${esc(s[0])}, ${esc(s[1])}]</span></li>`).join("")}</div></div>`);
      }
    } else {
      parts.push(`<dl class="kv"><dt>type</dt><dd>${esc(kind)}</dd><dt>in</dt><dd>${incoming.length}</dd><dt>out</dt><dd>${outgoing.length}</dd></dl>`);
    }
    parts.push(edgeSection("Incoming", incoming, "from"));
    parts.push(edgeSection("Outgoing", outgoing, "to"));
    root.innerHTML = parts.join("");
  }

  function renderPtpnInspector(id) {
    const root = $("#ptpn-inspector");
    const graph = state.response && state.response.graphs && state.response.graphs.ptpn;
    if (!graph || !id) {
      root.innerHTML = `<div class="inspector-empty"><strong>SELECT A NODE</strong><span>Click a place or transition to inspect its structure.</span></div>`;
      return;
    }
    const node = graph.nodes.find((n) => n.id === id);
    if (!node) {
      root.innerHTML = `<div class="inspector-empty"><strong>UNKNOWN NODE</strong><span>${esc(id)}</span></div>`;
      return;
    }
    const incoming = graph.edges.filter((e) => e.to === id);
    const outgoing = graph.edges.filter((e) => e.from === id);
    const rows = [];
    rows.push(`<dt>kind</dt><dd>${esc(node.kind || "-")}</dd>`);
    if (node.core !== undefined && node.core !== null) rows.push(`<dt>core</dt><dd>${esc(node.core)}</dd>`);
    if (node.priority !== undefined && node.priority !== null) rows.push(`<dt>priority</dt><dd>${esc(node.priority)}</dd>`);
    if (node.interval) rows.push(`<dt>interval</dt><dd>[${esc(node.interval[0])}, ${esc(node.interval[1])}]</dd>`);
    if (node.marking !== undefined) rows.push(`<dt>marking</dt><dd>${esc(node.marking)}</dd>`);
    if (node.capacity !== undefined) rows.push(`<dt>capacity</dt><dd>${esc(node.capacity)}</dd>`);
    rows.push(`<dt>in / out</dt><dd>${incoming.length} / ${outgoing.length}</dd>`);
    root.innerHTML = `
      <div class="inspector-title"><h4>${esc(node.text || node.id)}</h4><span class="inspector-kind">${esc(node.kind || "node")}</span></div>
      <dl class="kv">${rows.join("")}</dl>
      ${node.raw_label ? `<div class="inspector-section"><h5>Raw label</h5><pre class="zone">${esc(node.raw_label)}</pre></div>` : ""}
      ${edgeSection("Pre-set", incoming, "from")}
      ${edgeSection("Post-set", outgoing, "to")}`;
  }

  function renderScgInspector(id) {
    const root = $("#scg-inspector");
    const graph = state.response && state.response.graphs && state.response.graphs.scg;
    if (!graph || !id) {
      root.innerHTML = `<div class="inspector-empty"><strong>SELECT A STATE</strong><span>Click a state class to inspect marking M, E_struct, E_pri, R (suspended) and the DBM zone.</span></div>`;
      return;
    }
    const node = graph.nodes.find((n) => n.id === id);
    if (!node) {
      root.innerHTML = `<div class="inspector-empty"><strong>UNKNOWN STATE</strong><span>${esc(id)}</span></div>`;
      return;
    }
    const detail = node.detail || {};
    const incoming = graph.edges.filter((e) => e.to === id);
    const outgoing = graph.edges.filter((e) => e.from === id);
    const clocks = detail.clocks || [];
    const clocksHtml = clocks.length ? `<div class="inspector-section"><h5>DBM Clock Bounds</h5><ul class="edge-list">${
      clocks.map((c) => `<li><span>${esc(c.name)}</span><span class="dwell">[${esc(c.lower)}, ${c.upper == null ? "inf" : esc(c.upper)}]</span></li>`).join("")
    }</ul></div>` : "";
    root.innerHTML = `
      <div class="inspector-title"><h4>${esc(node.label || id)}</h4><span class="inspector-kind">${node.kind === "deadlock" ? "deadlock" : "state class"}</span></div>
      <dl class="kv">
        <dt>elapsed</dt><dd>${esc(detail.elapsed ?? "-")} ms</dd>
        <dt>marking M</dt><dd>${esc(detail.marking || "-")}</dd>
        <dt>E_struct</dt><dd>${esc(detail.e_struct || "-")}</dd>
        <dt>E_pri</dt><dd style="color:var(--brass-hi)">${esc(detail.enabled || "-")}</dd>
        <dt>R (susp)</dt><dd style="color:var(--warn)">${esc(detail.suspended || "none")}</dd>
        <dt>in / out</dt><dd>${incoming.length} / ${outgoing.length}</dd>
      </dl>
      ${clocksHtml}
      ${detail.zone ? `<div class="inspector-section"><h5>Raw DBM Zone</h5><pre class="zone">${esc(detail.zone)}</pre></div>` : ""}
      ${edgeSection("Incoming", incoming, "from", true)}
      ${edgeSection("Outgoing", outgoing, "to", true)}`;
  }

  function edgeSection(title, edges, dir, showDwell = false) {
    if (!edges.length) return "";
    return `<div class="inspector-section"><h5>${esc(title)}</h5><ul class="edge-list">
      ${edges.map((e) => {
        const other = dir === "from" ? e.from : e.to;
        const label = e.transition || e.label || "";
        const dwell = showDwell && e.dwell_min !== undefined ? `[${e.dwell_min}, ${e.dwell_max}]` : "";
        return `<li><span>${dir === "from" ? "<-" : "->"} ${esc(truncate(other, 24))}</span>${label ? `<span class="dim">${esc(truncate(label, 20))}</span>` : ""}${dwell ? `<span class="dwell">${esc(dwell)}</span>` : ""}</li>`;
      }).join("")}
    </ul></div>`;
  }

  /* ------------------------------------------------------------- TDG layout */

  function buildTdgGraph(tdg) {
    const rawNodes = Array.isArray(tdg.nodes) ? tdg.nodes : [];
    const rawEdges = Array.isArray(tdg.edges) ? tdg.edges : [];
    const config = tdg.configuration || {};
    const periodic = new Map();
    for (const p of config.periodic || []) {
      if (p && p.task) periodic.set(p.task, p.period);
    }
    const ids = new Set(rawNodes.map((n) => n.id));
    const nodes = rawNodes.map((n, idx) => {
      const type = n.type || "task";
      const size = type === "fork" || type === "join" ? { w: 46, h: 46 } : { w: 170, h: 70 };
      return {
        id: String(n.id),
        type,
        label: String(n.id),
        raw: n,
        w: size.w,
        h: size.h,
        x: 0,
        y: 0,
        order: idx,
      };
    });
    const edges = rawEdges
      .filter((e) => e && e.source != null && e.target != null)
      .map((e) => ({ from: String(e.source), to: String(e.target), label: e.label || "", pseudo: false }));

    // Start/end markers are badges on the task cards, not extra graph ranks:
    // keeping the dependency layout to its real depth preserves readable type.
    const startTasks = (config.start || []).map((s) => (typeof s === "string" ? s : s.task)).filter((id) => ids.has(id));
    const endTasks = (config.end || []).filter((id) => ids.has(id));
    for (const n of nodes) {
      n.roles = [];
      if (startTasks.includes(n.id)) n.roles.push("start");
      if (endTasks.includes(n.id)) n.roles.push("end");
    }

    // Ensure every edge endpoint exists.
    for (const e of edges) {
      if (!nodes.find((n) => n.id === e.from)) {
        nodes.push({ id: e.from, type: "task", label: e.from, raw: { id: e.from }, w: 130, h: 54, x: 0, y: 0, order: 999 });
      }
      if (!nodes.find((n) => n.id === e.to)) {
        nodes.push({ id: e.to, type: "task", label: e.to, raw: { id: e.to }, w: 130, h: 54, x: 0, y: 0, order: 999 });
      }
    }

    const nodeMap = new Map(nodes.map((n) => [n.id, n]));
    const succ = new Map(nodes.map((n) => [n.id, []]));
    const pred = new Map(nodes.map((n) => [n.id, []]));
    const indeg = new Map(nodes.map((n) => [n.id, 0]));
    for (const e of edges) {
      if (!nodeMap.has(e.from) || !nodeMap.has(e.to)) continue;
      succ.get(e.from).push(e.to);
      pred.get(e.to).push(e.from);
      indeg.set(e.to, (indeg.get(e.to) || 0) + 1);
    }

    // Longest-path rank (Kahn). Cycles fall back to max predecessor + 1.
    const rank = new Map(nodes.map((n) => [n.id, 0]));
    const queue = nodes.filter((n) => (indeg.get(n.id) || 0) === 0).map((n) => n.id);
    const seen = new Set();
    while (queue.length) {
      const id = queue.shift();
      if (seen.has(id)) continue;
      seen.add(id);
      for (const next of succ.get(id) || []) {
        rank.set(next, Math.max(rank.get(next) || 0, (rank.get(id) || 0) + 1));
        indeg.set(next, (indeg.get(next) || 0) - 1);
        if ((indeg.get(next) || 0) <= 0) queue.push(next);
      }
    }
    const maxRank = Math.max(0, ...Array.from(rank.values()));
    for (const n of nodes) {
      if (!seen.has(n.id)) rank.set(n.id, Math.min(maxRank + 1, (pred.get(n.id) || []).reduce((m, p) => Math.max(m, (rank.get(p) || 0) + 1), 0)));
    }

    // Group by rank and apply a few barycenter ordering passes.
    const ranks = new Map();
    for (const n of nodes) {
      const r = rank.get(n.id) || 0;
      if (!ranks.has(r)) ranks.set(r, []);
      ranks.get(r).push(n);
    }
    const rankList = Array.from(ranks.keys()).sort((a, b) => a - b);
    for (const r of rankList) ranks.get(r).sort((a, b) => (a.order || 0) - (b.order || 0));
    for (let pass = 0; pass < 3; pass += 1) {
      for (let i = 1; i < rankList.length; i += 1) {
        const r = rankList[i];
        const prev = ranks.get(rankList[i - 1]);
        const prevIndex = new Map(prev.map((n, idx) => [n.id, idx]));
        ranks.get(r).sort((a, b) => bary(a, pred, prevIndex) - bary(b, pred, prevIndex));
      }
      for (let i = rankList.length - 2; i >= 0; i -= 1) {
        const r = rankList[i];
        const next = ranks.get(rankList[i + 1]);
        const nextIndex = new Map(next.map((n, idx) => [n.id, idx]));
        ranks.get(r).sort((a, b) => bary(a, succ, nextIndex) - bary(b, succ, nextIndex));
      }
    }

    // Top-to-bottom layered layout: the workbench stage is wider than tall,
    // so depth uses the vertical axis and sibling tasks sit side by side.
    const colGap = 212;
    const rowGap = 104;
    let minX = Infinity;
    let minY = Infinity;
    let maxX = -Infinity;
    let maxY = -Infinity;
    rankList.forEach((r, rowIndex) => {
      const list = ranks.get(r);
      const total = (list.length - 1) * colGap;
      list.forEach((n, col) => {
        n.x = col * colGap + n.w / 2 - total / 2;
        n.y = 42 + rowIndex * rowGap + n.h / 2;
        minX = Math.min(minX, n.x - n.w / 2);
        minY = Math.min(minY, n.y - n.h / 2);
        maxX = Math.max(maxX, n.x + n.w / 2);
        maxY = Math.max(maxY, n.y + n.h / 2);
      });
    });
    const shiftX = 24 - minX;
    const shiftY = 24 - minY;
    for (const n of nodes) {
      n.x += shiftX;
      n.y += shiftY;
    }
    const bb = { w: Math.max(1, maxX - minX + 48), h: Math.max(1, maxY - minY + 48) };

    return {
      nodes,
      edges,
      bb,
      meta: {
        config,
        periodicMap: Object.fromEntries(periodic),
        startTasks,
        endTasks,
        locks: config.shared_locks || [],
        nodeMap,
      },
    };
  }

  function bary(node, links, index) {
    const list = links.get(node.id) || [];
    if (!list.length) return 0;
    let sum = 0;
    let count = 0;
    for (const other of list) {
      if (index.has(other)) {
        sum += index.get(other);
        count += 1;
      }
    }
    return count ? sum / count : 0;
  }

  /* --------------------------------------------------------------- GraphView */

  class GraphView {
    constructor(stage, opts = {}) {
      this.stage = stage;
      this.opts = opts;
      this.kind = opts.kind || "graph";
      this.graph = null;
      this.scale = 1;
      this.tx = 0;
      this.ty = 0;
      this.dragging = false;
      this.moved = false;
      this.startX = 0;
      this.startY = 0;
      this.startTx = 0;
      this.startTy = 0;
      this.selectedId = null;
      this.showEdgeLabels = false;
      this.markerId = `arrow-${this.kind}-${Math.random().toString(36).slice(2, 8)}`;
      this.svg = svgEl("svg", { role: "img", "aria-label": `${opts.ariaLabel || this.kind} graph` });
      this.defs = svgEl("defs");
      this.defs.innerHTML = `<marker id="${this.markerId}" viewBox="0 0 8 8" refX="7" refY="4" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0,0 L8,4 L0,8 z" fill="var(--line-3)"/></marker>`;
      this.root = svgEl("g");
      this.edgeLayer = svgEl("g", { class: "gedges" });
      this.nodeLayer = svgEl("g", { class: "gnodes" });
      this.root.append(this.edgeLayer, this.nodeLayer);
      this.svg.append(this.defs, this.root);
      stage.insertBefore(this.svg, stage.firstChild);
      this.note = document.createElement("div");
      this.note.className = "graph-focus-note";
      this.note.hidden = true;
      stage.appendChild(this.note);
      this.bind();
      this.resizeObserver = null;
      if (window.ResizeObserver) {
        this.resizeObserver = new ResizeObserver(() => {
          if (this.graph && !this.userMoved) this.fit();
        });
        this.resizeObserver.observe(stage);
      }
    }

    bind() {
      const svg = this.svg;
      svg.addEventListener("pointerdown", (ev) => {
        if (ev.button !== 0) return;
        if (ev.target.closest && ev.target.closest(".gnode")) return;
        this.dragging = true;
        this.moved = false;
        this.startX = ev.clientX;
        this.startY = ev.clientY;
        this.startTx = this.tx;
        this.startTy = this.ty;
        this.stage.classList.add("dragging");
        try { svg.setPointerCapture(ev.pointerId); } catch { /* ignore */ }
      });
      svg.addEventListener("pointermove", (ev) => {
        if (!this.dragging) return;
        const dx = ev.clientX - this.startX;
        const dy = ev.clientY - this.startY;
        if (Math.abs(dx) + Math.abs(dy) > 3) this.moved = true;
        this.tx = this.startTx + dx;
        this.ty = this.startTy + dy;
        this.userMoved = true;
        this.applyTransform();
      });
      const endDrag = (ev) => {
        if (!this.dragging) return;
        this.dragging = false;
        this.stage.classList.remove("dragging");
        try { svg.releasePointerCapture(ev.pointerId); } catch { /* ignore */ }
      };
      svg.addEventListener("pointerup", endDrag);
      svg.addEventListener("pointercancel", endDrag);
      svg.addEventListener("wheel", (ev) => {
        if (!this.graph) return;
        ev.preventDefault();
        const factor = Math.exp(-ev.deltaY * 0.0014);
        this.zoomAt(factor, ev.clientX, ev.clientY);
      }, { passive: false });
      svg.addEventListener("pointerleave", () => hideTip());
    }

    setData(graph, kind, opts = {}) {
      this.graph = graph;
      this.kind = kind || this.kind;
      this.fitMode = opts.focus ? "focus" : "all";
      this.userMoved = false;
      this.render();
      requestAnimationFrame(() => this.fit(this.fitMode));
    }

    clear() {
      this.graph = null;
      this.edgeLayer.innerHTML = "";
      this.nodeLayer.innerHTML = "";
    }

    render() {
      if (!this.graph) return;
      const graph = this.graph;
      const nodeMap = new Map(graph.nodes.map((n) => [n.id, n]));
      this.edgeLayer.innerHTML = graph.edges.map((e) => this.edgeHtml(e, nodeMap)).join("");
      this.nodeLayer.innerHTML = graph.nodes.map((n) => this.nodeHtml(n, graph)).join("");
      this.bindNodes();
      this.bindEdges();
    }

    nodeHtml(n, graph) {
      const kind = n.kind || n.type || "state";
      const w = Math.max(22, Number(n.w) || 34);
      const h = Math.max(22, Number(n.h) || 34);
      const x = Number(n.x) || 0;
      const y = Number(n.y) || 0;
      const shapeKind = this.shapeClass(n);
      let shape = "";
      if (this.isCircle(n)) {
        const r = Math.min(w, h) / 2;
        shape = `<circle class="shape ${shapeKind}" r="${r}"/>`;
        if (shapeKind === "resource") shape += `<circle class="shape-ring" r="${r + 3}"/>`;
      } else if (shapeKind === "fork" || shapeKind === "join") {
        shape = `<polygon class="shape ${shapeKind}" points="0,${-h / 2} ${w / 2},0 0,${h / 2} ${-w / 2},0"/>`;
      } else {
        const rx = shapeKind === "start" || shapeKind === "end" ? h / 2 : this.kind === "tdg" ? 5 : 3;
        shape = `<rect class="shape ${shapeKind}" x="${-w / 2}" y="${-h / 2}" width="${w}" height="${h}" rx="${rx}"/>`;
      }
      let extra = "";
      if (this.kind === "tdg" && n.type === "task") {
        const core = Number(n.raw && n.raw.core);
        const color = core >= 0 && core < CORE_COLORS.length ? CORE_COLORS[core] : CORE_NEUTRAL;
        extra += `<line class="core-stripe" x1="${-w / 2 + 5}" y1="${-h / 2 + 9}" x2="${-w / 2 + 5}" y2="${h / 2 - 9}" stroke="${color}"/>`;
        const locks = (n.raw && n.raw.locks) || [];
        if (locks.length) {
          extra += `<rect x="${w / 2 - 28}" y="${-h / 2 + 7}" width="20" height="13" rx="2" fill="rgba(201,138,75,.14)" stroke="var(--brass-dim)" stroke-width="0.8"/>`;
          extra += `<text class="node-chip" x="${w / 2 - 18}" y="${-h / 2 + 16}" text-anchor="middle">L${locks.length}</text>`;
        }
        const role = n.roles && n.roles.includes("start") ? "S" : n.roles && n.roles.includes("end") ? "E" : "";
        if (role) {
          extra += `<text class="node-chip" x="${w / 2 - 10}" y="${h / 2 - 9}" text-anchor="end" fill="var(--brass-hi)">${role}</text>`;
        }
      }
      if (this.kind === "scg" && n.kind === "deadlock") {
        extra += `<circle r="${Math.max(w, h) / 2 + 4}" fill="none" stroke="var(--critical)" stroke-width="0.8" opacity=".55"/>`;
      }
      if (this.kind === "scg" && n.id === "s0") {
        extra += `<circle r="3" cx="${-w / 2 + 5}" cy="${-h / 2 + 5}" fill="var(--brass-hi)"/>`;
      }
      const label = this.nodeLabel(n, graph);
      const sub = this.nodeSub(n, graph);
      const text = [];
      if (this.kind === "tdg" && n.type === "task") {
        const lines = wrapName(String(n.raw && n.raw.id ? n.raw.id : n.id), 19, 2);
        const startY = lines.length > 1 ? -h / 2 + 18 : -h / 2 + 24;
        lines.forEach((line, i) => {
          text.push(`<text class="node-label" x="${-w / 2 + 14}" y="${startY + i * 12}" text-anchor="start">${esc(line)}</text>`);
        });
        text.push(`<text class="node-sub" x="${-w / 2 + 14}" y="${h / 2 - 18}" text-anchor="start">${esc(sub || "")}</text>`);
      } else if (this.kind === "tdg") {
        text.push(`<text class="node-label" x="0" y="3" text-anchor="middle">${esc(label)}</text>`);
      } else if (this.kind === "ptpn") {
        const lines = wrapName(label, 14, 2);
        const y0 = lines.length > 1 ? -4 : 2;
        lines.forEach((line, i) => {
          text.push(`<text class="node-label" x="0" y="${y0 + i * 11}" text-anchor="middle">${esc(line)}</text>`);
        });
        if (sub) text.push(`<text class="node-sub" x="0" y="${h / 2 + 10}" text-anchor="middle">${esc(sub)}</text>`);
      } else {
        text.push(`<text class="node-label" x="0" y="2" text-anchor="middle">${esc(label)}</text>`);
        if (sub) text.push(`<text class="node-sub" x="0" y="${h / 2 + 11}" text-anchor="middle">${esc(sub)}</text>`);
      }
      const tip = n.tooltip || n.raw_label || `${label}${sub ? `\n${sub}` : ""}`;
      return `<g class="gnode ${shapeKind}${n.kind === "deadlock" ? " deadlock" : ""}" data-id="${esc(n.id)}" transform="translate(${x.toFixed(2)} ${y.toFixed(2)})" tabindex="0" role="button" aria-label="${esc(tip.replace(/\n/g, " | "))}">${shape}${extra}${text.join("")}</g>`;
    }

    nodeLabel(n, graph) {
      if (this.kind === "tdg") {
        if (n.type === "start") return "START";
        if (n.type === "end") return "END";
        if (n.type === "fork") return "F";
        if (n.type === "join") return "J";
        return n.id;
      }
      if (this.kind === "scg") return n.label || n.id.replace(/^s/, "S");
      return n.text || n.label || n.id;
    }

    nodeSub(n, graph) {
      if (this.kind === "tdg" && n.type === "task") {
        const raw = n.raw || {};
        const segs = Array.isArray(raw.time) ? raw.time : [];
        const interval = segs.length ? `[${sumSegments(segs, 0)},${sumSegments(segs, 1)}]` : "-";
        const locks = (raw.locks || []).length ? ` | L${(raw.locks || []).length}` : "";
        return `C${raw.core ?? "-"} | pi${raw.priority ?? "-"} | ${interval}${locks}`;
      }
      if (this.kind === "ptpn") {
        if (["place", "resource", "period", "hold", "segment"].includes(n.kind)) {
          const bits = [];
          if (n.marking !== undefined && n.marking !== null) bits.push(`M=${n.marking}`);
          if (n.capacity !== undefined && n.capacity !== null) bits.push(`C=${n.capacity}`);
          return bits.join(" ");
        }
        return "";
      }
      if (this.kind === "scg") {
        return n.detail && n.detail.elapsed !== undefined ? `t=${n.detail.elapsed}` : "";
      }
      return "";
    }

    isCircle(n) {
      if (this.kind === "tdg") return false;
      if (this.kind === "scg") return false;
      return ["place", "resource", "period", "hold", "segment"].includes(n.kind);
    }

    shapeClass(n) {
      if (this.kind === "tdg") return n.type || "task";
      if (this.kind === "scg") return n.kind === "deadlock" ? "deadlock" : "state";
      return n.kind || "place";
    }

    edgeHtml(e, nodeMap) {
      const a = nodeMap.get(e.from);
      const b = nodeMap.get(e.to);
      if (!a || !b) return "";
      const points = this.edgePoints(e, a, b);
      if (points.length < 2) return "";
      let d;
      if (this.kind === "tdg" || this.kind === "scg" && !(e.points && e.points.length)) {
        d = this.bezier(points[0], points[points.length - 1]);
      } else {
        d = `M ${points.map(([x, y]) => `${x.toFixed(2)} ${y.toFixed(2)}`).join(" L ")}`;
      }
      const dwell = e.dwell_max;
      let cls = "gedge";
      if (this.kind === "scg") {
        const bucket = !isNum(dwell) ? 0 : Number(dwell) <= 0 ? 1 : Number(dwell) <= 4 ? 2 : 3;
        cls += ` dwell-${bucket}`;
      }
      if (e.pseudo) cls += " pseudo";
      const label = e.transition || e.label || "";
      const tip = label || `${e.from} -> ${e.to}`;
      let labelEl = "";
      if (this.showEdgeLabels && label) {
        const mid = points[Math.floor(points.length / 2)];
        labelEl = `<text class="edge-label" x="${mid[0].toFixed(1)}" y="${(mid[1] - 4).toFixed(1)}" text-anchor="middle">${esc(truncate(label, 18))}</text>`;
      }
      return `<path class="${cls}" data-from="${esc(e.from)}" data-to="${esc(e.to)}" d="${d}" marker-end="url(#${this.markerId})"><title>${esc(tip)}</title></path>${labelEl}`;
    }

    edgePoints(e, a, b) {
      if (Array.isArray(e.points) && e.points.length >= 2) {
        return e.points.map((p) => [Number(p[0]) || 0, Number(p[1]) || 0]);
      }
      if (this.kind === "tdg") {
        return [[a.x, a.y + a.h / 2], [b.x, b.y - b.h / 2]];
      }
      return [[a.x, a.y], [b.x, b.y]];
    }

    bezier([x0, y0], [x1, y1]) {
      if (this.kind === "tdg") {
        const dy = Math.max(28, Math.abs(y1 - y0) * 0.45);
        return `M ${x0.toFixed(2)} ${y0.toFixed(2)} C ${x0.toFixed(2)} ${(y0 + dy).toFixed(2)}, ${x1.toFixed(2)} ${(y1 - dy).toFixed(2)}, ${x1.toFixed(2)} ${y1.toFixed(2)}`;
      }
      const dx = Math.max(36, Math.abs(x1 - x0) * 0.5);
      return `M ${x0.toFixed(2)} ${y0.toFixed(2)} C ${(x0 + dx).toFixed(2)} ${y0.toFixed(2)}, ${(x1 - dx).toFixed(2)} ${y1.toFixed(2)}, ${x1.toFixed(2)} ${y1.toFixed(2)}`;
    }

    bindNodes() {
      this.nodeEls = new Map();
      $$(".gnode", this.nodeLayer).forEach((el) => {
        const id = el.dataset.id;
        this.nodeEls.set(id, el);
        el.addEventListener("pointerenter", (ev) => {
          const node = this.graph && this.graph.nodes.find((n) => n.id === id);
          if (node) {
            const tip = node.tooltip || node.raw_label || this.nodeLabel(node, this.graph);
            showTip(tip, ev.clientX, ev.clientY);
          }
          this.highlightConnections(id, true);
        });
        el.addEventListener("pointermove", (ev) => moveTip(ev.clientX, ev.clientY));
        el.addEventListener("pointerleave", () => {
          hideTip();
          this.highlightConnections(id, false);
        });
        el.addEventListener("click", (ev) => {
          ev.stopPropagation();
          if (this.opts.onSelect) this.opts.onSelect(id);
        });
        el.addEventListener("keydown", (ev) => {
          if (ev.key === "Enter" || ev.key === " ") {
            ev.preventDefault();
            if (this.opts.onSelect) this.opts.onSelect(id);
          }
        });
      });
    }

    bindEdges() {
      $$(".gedge", this.edgeLayer).forEach((el) => {
        el.addEventListener("pointerenter", (ev) => {
          el.classList.add("hot");
          const from = this.nodeEls && this.nodeEls.get(el.dataset.from);
          const to = this.nodeEls && this.nodeEls.get(el.dataset.to);
          if (from) from.classList.add("hot");
          if (to) to.classList.add("hot");
          const title = el.querySelector("title");
          if (title) showTip(title.textContent, ev.clientX, ev.clientY);
        });
        el.addEventListener("pointermove", (ev) => moveTip(ev.clientX, ev.clientY));
        el.addEventListener("pointerleave", () => {
          el.classList.remove("hot");
          const from = this.nodeEls && this.nodeEls.get(el.dataset.from);
          const to = this.nodeEls && this.nodeEls.get(el.dataset.to);
          if (from) from.classList.remove("hot");
          if (to) to.classList.remove("hot");
          hideTip();
        });
      });
    }

    highlightConnections(id, on) {
      if (!this.edgeLayer) return;
      $$(".gedge", this.edgeLayer).forEach((el) => {
        if (el.dataset.from === id || el.dataset.to === id) el.classList.toggle("hot", on);
      });
    }

    select(id) {
      this.selectedId = id;
      if (!this.nodeEls) return;
      for (const [nodeId, el] of this.nodeEls) {
        el.classList.toggle("selected", nodeId === id);
      }
    }

    setEdgeLabels(on) {
      this.showEdgeLabels = !!on;
      if (this.graph) this.render();
      this.select(this.selectedId);
    }

    emphasizeDeadlocks(on) {
      this.stage.classList.toggle("deadlock-focus", !!on);
    }

    applyTransform() {
      this.root.setAttribute("transform", `translate(${this.tx.toFixed(2)} ${this.ty.toFixed(2)}) scale(${this.scale.toFixed(4)})`);
    }

    fit(mode) {
      if (!this.graph) return;
      const fitMode = mode || "all";
      const rect = this.stage.getBoundingClientRect();
      const W = Math.max(160, rect.width);
      const H = Math.max(200, rect.height);
      const bb = this.graph.bb || { w: 800, h: 600 };
      const pad = 40;
      const scale = clamp(Math.min((W - pad) / bb.w, (H - pad) / bb.h), 0.04, 2.6);
      let focused = false;
      if (fitMode === "focus" && scale < 0.62) {
        // Large nets (PTPN) are unreadable at whole-graph scale. Start zoomed
        // to the leftmost execution chain; FIT still reveals the whole net.
        this.scale = 0.78;
        const anchor = this.graph.nodes.reduce((a, b) => (a.x <= b.x ? a : b), this.graph.nodes[0]);
        this.tx = W * 0.12 - anchor.x * this.scale;
        this.ty = H * 0.5 - anchor.y * this.scale;
        this.fitMode = "focus";
        focused = true;
      } else {
        this.scale = scale;
        this.tx = (W - bb.w * scale) / 2;
        this.ty = (H - bb.h * scale) / 2;
        this.fitMode = fitMode;
      }
      if (this.note) {
        this.note.hidden = !focused;
        this.note.textContent = "First-chain view | FIT for all | drag to pan";
      }
      this.applyTransform();
      this.userMoved = false;
    }

    zoomAt(factor, clientX, clientY) {
      const rect = this.svg.getBoundingClientRect();
      const mx = clientX - rect.left;
      const my = clientY - rect.top;
      const next = clamp(this.scale * factor, 0.03, 4);
      const ratio = next / this.scale;
      this.tx = mx - (mx - this.tx) * ratio;
      this.ty = my - (my - this.ty) * ratio;
      this.scale = next;
      this.userMoved = true;
      this.applyTransform();
    }

    zoomBy(factor) {
      const rect = this.stage.getBoundingClientRect();
      this.zoomAt(factor, rect.left + rect.width / 2, rect.top + rect.height / 2);
    }
  }

  /* ----------------------------------------------------------------- metrics */

  function renderMetrics() {
    const metrics = state.response && state.response.metrics;
    const tasks = (metrics && metrics.tasks) || [];
    const taskTable = $("#task-table");
    if (!metrics) {
      taskTable.innerHTML = `<div class="empty"><strong>NO METRICS</strong>Run an analysis to show per-task timing metrics.</div>`;
      $("#core-table").innerHTML = `<div class="empty"><strong>NO DATA</strong></div>`;
      $("#lock-table-full").innerHTML = `<div class="empty"><strong>NO DATA</strong></div>`;
      $("#fact-grid").innerHTML = "";
      return;
    }
    if (!tasks.length) {
      taskTable.innerHTML = `<div class="empty"><strong>NO TASKS</strong>No tasks were observed in the metrics.</div>`;
    } else {
      taskTable.innerHTML = `<table class="data">
        <thead><tr>
          <th>task</th><th class="num">core</th><th class="num">pi</th>
          <th class="num">BCET</th><th class="num">WCET</th><th class="num">BCRT</th>
          <th class="num">WCRT</th><th class="num">WCRT_LB</th><th class="num">deadline</th>
          <th class="num">slack</th><th class="num">interf.</th><th class="num">block</th>
          <th class="num">preempt</th><th class="num">in flight</th><th>status</th>
        </tr></thead>
        <tbody>${tasks.map((t) => {
          const status = t.verdict === "misses" ? `<span class="row-status err">misses</span>`
            : t.verdict === "unknown" ? `<span class="row-status warn">unknown</span>`
            : t.verdict === "meets" ? `<span class="row-status ok">meets</span>`
            : t.deadline_missed ? `<span class="row-status err">miss</span>`
            : t.local_deadlock ? `<span class="row-status err">local deadlock</span>`
            : t.observed === false ? `<span class="row-status">unobserved</span>`
            : `<span class="row-status ok">ok</span>`;
          return `<tr>
            <td class="name">${esc(t.name)}</td>
            <td class="num">${esc(t.core)}</td>
            <td class="num">${esc(t.priority)}</td>
            <td class="num">${fmtNum(t.bcet)}</td>
            <td class="num">${fmtNum(t.wcet)}</td>
            <td class="num">${fmtNum(t.bcrt)}</td>
            <td class="num accent">${fmtNum(t.wcrt)}</td>
            <td class="num">${fmtNum(t.wcrt_lb)}</td>
            <td class="num ${t.deadline > 0 && t.wcrt > t.deadline ? "err-text" : ""}">${t.deadline > 0 ? fmtNum(t.deadline) : "-"}</td>
            <td class="num ${isNum(t.slack) && Number(t.slack) < 0 ? "err-text" : ""}">${isNum(t.slack) ? fmtNum(t.slack) : "-"}</td>
            <td class="num">${fmtNum(t.worst_interference)}</td>
            <td class="num">${fmtNum(t.worst_blocking)}</td>
            <td class="num">${fmtNum(t.max_preemptions)}</td>
            <td class="num">${fmtNum(t.max_in_flight)}</td>
            <td>${status}</td>
          </tr>`;
        }).join("")}</tbody>
      </table>`;
    }

    const cores = metrics.cores || [];
    $("#core-table").innerHTML = cores.length ? `<table class="data compact">
      <thead><tr><th>core</th><th class="num">util min</th><th class="num">util max</th><th class="num">graph busy</th></tr></thead>
      <tbody>${cores.map((c) => `<tr>
        <td class="name">core ${esc(c.core)}</td>
        <td class="num">${fmtPct(c.util_min)}</td>
        <td class="num">${fmtPct(c.util_max)}</td>
        <td class="num">${fmtPct(c.graph_busy_fraction, 2)}</td>
      </tr>`).join("")}</tbody></table>` : `<div class="empty"><strong>NO CORES</strong></div>`;

    const locks = metrics.locks || [];
    $("#lock-table-full").innerHTML = locks.length ? `<table class="data compact">
      <thead><tr><th>lock</th><th class="num">worst hold</th><th class="num">total hold</th><th class="num">total wait</th></tr></thead>
      <tbody>${locks.map((l) => `<tr>
        <td class="name">${esc(l.name)}</td>
        <td class="num">${fmtNum(l.worst_hold)}</td>
        <td class="num">${fmtNum(l.total_hold)}</td>
        <td class="num">${fmtNum(l.total_wait)}</td>
      </tr>`).join("")}</tbody></table>` : `<div class="empty"><strong>NO LOCKS</strong></div>`;

    const facts = [
      ["exact", metrics.exact ? "true" : "false"],
      ["wcrt_exact", metrics.wcrt_exact === undefined ? "-" : metrics.wcrt_exact ? "true" : "false"],
      ["wcrt_lb_valid", metrics.wcrt_lb_valid ? "true" : "false"],
      ["bounded", metrics.bounded ? "true" : "false"],
      ["schedulability", schedulabilityOf(metrics)],
      ["steady cycle", metrics.has_steady_cycle ? "yes" : "no"],
      ["recurrent SCC", fmtInt(metrics.recurrent_scc_size)],
      ["hyperperiod", `${fmtInt(metrics.hyperperiod)}<small> ms</small>`],
      ["deadlock states", fmtInt(deadlockCount(metrics))],
      ["states", fmtInt(metrics.states)],
      ["transitions", fmtInt(metrics.transitions)],
      ["tasks", fmtInt(tasks.length)],
      ["jobs / hyperperiod", fmtInt(tasks.reduce((a, t) => a + (Number(t.jobs_per_hyperperiod) || 0), 0))],
    ];
    $("#fact-grid").innerHTML = facts.map(([k, v]) => `<div class="fact"><dt>${esc(k)}</dt><dd>${v}</dd></div>`).join("");
  }

  /* ----------------------------------------------------------------- console */

  function renderConsole() {
    const summary = $("#console-summary");
    const timings = $("#console-timings");
    const log = $("#console-log");
    const resp = state.lastResponse;
    if (!resp) {
      if (summary) summary.textContent = "";
      if (timings) timings.innerHTML = "";
      if (log) log.textContent = "Awaiting first run...";
      return;
    }
    const lines = state.logs || [];
    if (summary) summary.textContent = `| exit ${resp.exit_code} | ${fmtDuration(resp.client_duration_ms ?? resp.duration_ms)} | ${lines.length} lines`;
    const t = resp.timings || {};
    const chips = [
      ["lower", t.lowering_ms],
      ["scg", t.scg_ms],
      ["metrics", t.metrics_ms],
      ["total", t.total_ms],
    ].filter(([, v]) => v != null);
    if (timings) {
      timings.innerHTML = chips.map(([k, v]) => `<span class="chip">${k} ${fmtMs(v)}</span>`).join("")
        + `<span class="chip" style="color:${resp.exit_code === 0 ? "var(--good)" : "var(--warn)"}">exit ${resp.exit_code}</span>`;
    }
    if (log) {
      const wasNearBottom = log.scrollHeight - log.scrollTop - log.clientHeight < 40;
      log.innerHTML = lines.length
        ? lines.map((line) => `<span class="log-line ${logClass(line)}">${esc(line)}</span>`).join("")
        : `<span class="log-line dim">No log output.</span>`;
      if (wasNearBottom) log.scrollTop = log.scrollHeight;
    }
  }

  function logClass(line) {
    if (/\berror\b|\[ERR\]/i.test(line)) return "err";
    if (/\bwarn(ing)?\b/i.test(line)) return "warn";
    if (/\[STATS\]/.test(line)) return "stat";
    if (/\[(OUTPUT|PETRI_EXPORT|SCG|METRICS)\]/.test(line)) return "dim";
    return "";
  }

  function renderConsoleError(message) {
    state.lastResponse = state.lastResponse || { exit_code: -1, duration_ms: 0, logs: [] };
    state.logs = [...(state.logs || []), `[ERR] ${message}`];
    renderConsole();
    $("#console").classList.add("open");
    $("#console-body").hidden = false;
    document.body.classList.add("console-open");
    $("#console-toggle").setAttribute("aria-expanded", "true");
  }

  /* ------------------------------------------------------------------ toast */

  function showToast(message, kind = "") {
    const el = $("#toast");
    el.textContent = message;
    el.className = `toast ${kind || ""}`.trim();
    el.hidden = false;
    clearTimeout(state.toastTimer);
    state.toastTimer = setTimeout(() => { el.hidden = true; }, 5200);
  }

  async function copyText(text, label) {
    if (!text) {
      showToast("There is nothing to copy.", "err");
      return;
    }
    try {
      await navigator.clipboard.writeText(text);
      showToast(`${label} copied.`, "ok");
    } catch {
      const ta = document.createElement("textarea");
      ta.value = text;
      ta.style.position = "fixed";
      ta.style.opacity = "0";
      document.body.appendChild(ta);
      ta.select();
      try {
        document.execCommand("copy");
        showToast(`${label} copied.`, "ok");
      } catch {
        showToast("Copy failed. Select the text manually.", "err");
      }
      ta.remove();
    }
  }

  /* ------------------------------------------------------------------- tabs */

  function setActiveTab(tab) {
    if (!TABS.includes(tab)) return;
    state.activeTab = tab;
    $$(".tab").forEach((el) => el.setAttribute("aria-selected", el.dataset.tab === tab ? "true" : "false"));
    $$(".tab-page").forEach((el) => { el.hidden = el.id !== `tab-${tab}`; });
    $$(".stage").forEach((el) => el.setAttribute("aria-current", el.dataset.tab === tab ? "true" : "false"));
    try { history.replaceState(null, "", `#${tab}`); } catch { /* ignore */ }
    if (tab === "tdg" || tab === "ptpn" || tab === "scg") {
      const view = state.views[tab];
      if (view && view.graph) {
        // The panel just became visible; force layout before fitting.
        if (!view.userMoved) view.fit(view.fitMode || "all");
        requestAnimationFrame(() => {
          if (!view.userMoved) view.fit(view.fitMode || "all");
        });
      }
    }
  }

  /* ----------------------------------------------------------------- events */

  function applyTheme() {
    document.documentElement.setAttribute("data-theme", state.theme);
  }

  function switchTray(name) {
    $$(".tray-tab").forEach((tab) => {
      const active = tab.dataset.tray === name;
      tab.classList.toggle("active", active);
      tab.setAttribute("aria-selected", active ? "true" : "false");
    });
    const diagBody = $("#tray-diagnostics");
    const pendBody = $("#tray-pending");
    if (diagBody) diagBody.hidden = name !== "diagnostics";
    if (pendBody) pendBody.hidden = name !== "pending";
  }

  function bindStatic() {
    applyTheme();

    const themeBtn = $("#btn-theme");
    if (themeBtn) {
      themeBtn.addEventListener("click", () => {
        state.theme = state.theme === "dark" ? "light" : "dark";
        localStorage.setItem("ptpn_theme", state.theme);
        applyTheme();
      });
    }

    const splitBtn = $("#btn-split-editor");
    if (splitBtn) {
      splitBtn.addEventListener("click", () => {
        const ws = $("#ide-workspace");
        if (!ws) return;
        const collapsed = ws.classList.toggle("editor-collapsed");
        splitBtn.setAttribute("aria-pressed", collapsed ? "false" : "true");
        requestAnimationFrame(() => {
          const view = state.views[state.activeTab];
          if (view && view.graph && !view.userMoved) view.fit(view.fitMode || "all");
        });
      });
    }

    $$(".tray-tab").forEach((tab) => {
      tab.addEventListener("click", () => switchTray(tab.dataset.tray));
    });

    const tbPolicy = $("#toolbar-policy");
    if (tbPolicy) {
      tbPolicy.addEventListener("change", () => {
        $("#opt-policy").value = tbPolicy.value;
        onOptionsChanged();
        if (!state.demo) runAnalysis("analyze", { keepTab: true });
      });
    }

    const tbRed = $("#toolbar-reduction");
    if (tbRed) {
      tbRed.addEventListener("change", () => {
        $("#opt-reduction").value = tbRed.value;
        onOptionsChanged();
        if (!state.demo) runAnalysis("analyze", { keepTab: true });
      });
    }

    ["#btn-llm-repair", "#btn-overview-repair", "#btn-model-repair"].forEach((sel) => {
      const btn = $(sel);
      if (btn) btn.addEventListener("click", openLlmRepairModal);
    });
    const copySrcBtn = $("#btn-copy-source");
    if (copySrcBtn) {
      copySrcBtn.addEventListener("click", () => copyText(state.source, "model source"));
    }

    $("#btn-analyze").addEventListener("click", () => runAnalysis("analyze"));
    $("#btn-check").addEventListener("click", () => runAnalysis("check"));
    $("#btn-help").addEventListener("click", openHelp);
    $("#btn-rail").addEventListener("click", () => {
      const shell = $("#ide-shell");
      const rail = $(".rail");
      if (window.matchMedia && window.matchMedia("(max-width: 920px)").matches) {
        const open = rail.classList.toggle("open");
        $("#btn-rail").setAttribute("aria-expanded", open ? "true" : "false");
      } else if (shell) {
        const collapsed = shell.classList.toggle("rail-collapsed");
        $("#btn-rail").setAttribute("aria-expanded", collapsed ? "false" : "true");
        requestAnimationFrame(() => {
          const view = state.views[state.activeTab];
          if (view && view.graph && !view.userMoved) view.fit(view.fitMode || "all");
        });
      }
    });
    $("#btn-new").addEventListener("click", newModel);
    $("#btn-goto-model").addEventListener("click", () => {
      const ws = $("#ide-workspace");
      if (ws && ws.classList.contains("editor-collapsed")) {
        ws.classList.remove("editor-collapsed");
        if ($("#btn-split-editor")) $("#btn-split-editor").setAttribute("aria-pressed", "true");
      }
      focusLine(1, 1);
    });
    $("#btn-reset-source").addEventListener("click", () => {
      setEditorSource(state.originalSource);
      state.dirty = false;
      updateEditorMeta();
      showToast("Restored the original model.", "ok");
    });
    $("#btn-copy-metrics").addEventListener("click", () => copyText(
      state.response && state.response.metrics ? JSON.stringify(state.response.metrics, null, 2) : "",
      "metrics JSON"
    ));
    $("#btn-copy-wcet").addEventListener("click", () => copyText(
      state.response && state.response.wcet ? JSON.stringify(state.response.wcet, null, 2) : "",
      "WCET JSON"
    ));
    $("#btn-copy-diag").addEventListener("click", () => copyText(
      JSON.stringify({ diagnostics: state.diagnostics, pending_measurements: state.pending }, null, 2),
      "diagnostics"
    ));

    $("#tabs").addEventListener("click", (ev) => {
      const tab = ev.target.closest(".tab");
      if (tab) setActiveTab(tab.dataset.tab);
    });
    $("#pipeline").addEventListener("click", (ev) => {
      const stage = ev.target.closest(".stage");
      if (stage) setActiveTab(stage.dataset.tab);
    });

    $("#options").addEventListener("change", onOptionsChanged);
    $("#options").addEventListener("input", onOptionsChanged);

    const ta = $("#code-input");
    ta.addEventListener("input", () => {
      state.source = ta.value;
      state.dirty = state.source !== state.originalSource;
      renderCode();
      updateEditorMeta();
    });
    ta.addEventListener("scroll", syncScroll);
    ta.addEventListener("keydown", (ev) => {
      if (ev.key === "Tab") {
        ev.preventDefault();
        const start = ta.selectionStart;
        const end = ta.selectionEnd;
        ta.setRangeText("  ", start, end, "end");
        state.source = ta.value;
        state.dirty = state.source !== state.originalSource;
        renderCode();
        updateEditorMeta();
      }
    });

    $("#console-toggle").addEventListener("click", () => {
      const open = !$("#console").classList.contains("open");
      $("#console").classList.toggle("open", open);
      $("#console-body").hidden = !open;
      document.body.classList.toggle("console-open", open);
      $("#console-toggle").setAttribute("aria-expanded", open ? "true" : "false");
    });

    $("#modal-close").addEventListener("click", closeModal);
    $("#modal").addEventListener("click", (ev) => { if (ev.target === $("#modal")) closeModal(); });

    $$("[data-graph-tools]").forEach((bar) => {
      const kind = bar.dataset.graphTools;
      bar.addEventListener("click", (ev) => {
        const btn = ev.target.closest("[data-action]");
        if (!btn) return;
        const action = btn.dataset.action;
        const view = getGraphView(kind);
        if (action === "fit") view.fit("all");
        else if (action === "zoom-in") view.zoomBy(1.25);
        else if (action === "zoom-out") view.zoomBy(0.8);
        else if (action === "labels") {
          state.filters.ptpnLabels = !state.filters.ptpnLabels;
          btn.setAttribute("aria-pressed", state.filters.ptpnLabels ? "true" : "false");
          view.setEdgeLabels(state.filters.ptpnLabels);
        } else if (action === "deadlocks") {
          state.filters.scgDeadlocks = !state.filters.scgDeadlocks;
          btn.setAttribute("aria-pressed", state.filters.scgDeadlocks ? "true" : "false");
          view.emphasizeDeadlocks(state.filters.scgDeadlocks);
        }
      });
    });

    document.addEventListener("keydown", (ev) => {
      if (ev.key === "Escape") {
        if (!$("#modal").hidden) { closeModal(); return; }
        hideTip();
        return;
      }
      const mod = ev.metaKey || ev.ctrlKey;
      if (mod && ev.key === "Enter") {
        ev.preventDefault();
        runAnalysis(ev.shiftKey ? "check" : "analyze");
        return;
      }
      const tag = (ev.target && ev.target.tagName) || "";
      if (!mod && !ev.altKey && !/INPUT|TEXTAREA|SELECT/.test(tag)) {
        const idx = Number(ev.key);
        if (idx >= 1 && idx <= TABS.length) setActiveTab(TABS[idx - 1]);
      }
    });

    window.addEventListener("hashchange", () => {
      const token = String(location.hash || "").replace(/^#/, "");
      if (TABS.includes(token) && token !== state.activeTab) setActiveTab(token);
    });
  }

  function newModel() {
    if (state.demo) {
      openDemoHint();
      return;
    }
    const templates = {
      rtsl: {
        filename: "model.rtsl",
        kind: "rtsl",
        source: `// New PTPN model - Form 01: RTSL v1
system new_model {
  cpus 2
  cores_per_cpu 1
  time_unit ms
  policy fixed

  task ingest: periodic {
    period 100ms
    deadline 100ms
    core 0
    time [1, 2] estimated by "spec"
  }

  task handle: aperiodic {
    core 1
    time measured by llm { runs 10, timeout 2 }
  }

  start ingest
  end handle

  ingest -> handle
}
`,
      },
      tdg: {
        filename: "model.json",
        kind: "tdg",
        source: `{
  "graph": { "name": "NewTDGModel" },
  "configuration": {
    "num_cpus": 2,
    "cores_per_cpu": 1,
    "shared_locks": [],
    "policy": "fixed",
    "start": ["ingest"],
    "end": ["handle"],
    "periodic": [{ "task": "ingest", "period": 100 }]
  },
  "nodes": [
    { "id": "ingest", "type": "task", "priority": 100, "core": 0, "time": [[1, 2]], "locks": [] },
    { "id": "handle", "type": "task", "priority": 100, "core": 1, "time": [[2, 3]], "locks": [] }
  ],
  "edges": [
    { "source": "ingest", "target": "handle", "label": "0" }
  ]
}
`,
      },
      ptpn: {
        filename: "model.ptpn",
        kind: "ptpn",
        source: `// New PTPN model - Form 03: Native PTPN Net (.ptpn)
places
  P_entry:1, P_ready:1, P_exit:1

transitions
  T_get_core [0, 0] @priority=100, core=0
  T_exec     [2, 5] @priority=100, core=0, suspendable
  T_consume  [0, 0] @priority=0,   core=-1

P_entry -> T_get_core
T_get_core -> P_ready
P_ready -> T_exec
T_exec -> P_exit
P_exit -> T_consume

@init P_entry:1
`,
      },
    };

    let selectedKind = "rtsl";
    const html = `
      <p>Choose the model representation format to create (RTSL, TDG JSON, or native PTPN):</p>
      <div class="template-cards" id="new-model-cards">
        <button type="button" class="template-card selected" data-tpl="rtsl">
          <strong>01. RTSL Domain-Specific Language (.rtsl)</strong>
          <small>Declarative tasks, units, locks, periods, and <code class="code-inline">measured by llm</code> timing provenance</small>
        </button>
        <button type="button" class="template-card" data-tpl="tdg">
          <strong>02. TDG Task Dependency Graph JSON (.json)</strong>
          <small>Graph-structured nodes and edges for structured tool and LLM generation</small>
        </button>
        <button type="button" class="template-card" data-tpl="ptpn">
          <strong>03. Native PTPN Net Source (.ptpn)</strong>
          <small>Direct places, timed transitions, arcs, and <code class="code-inline">@init</code> marking without lowering</small>
        </button>
      </div>
      <div class="modal-actions">
        <button class="btn ghost" type="button" id="new-cancel">Cancel</button>
        <button class="btn primary" type="button" id="new-confirm">Create Model</button>
      </div>`;
    openModal({
      eyebrow: "Create Model",
      title: "New Real-Time System Model",
      html,
      onMount: () => {
        $$("#new-model-cards .template-card").forEach((card) => {
          card.addEventListener("click", () => {
            selectedKind = card.dataset.tpl;
            $$("#new-model-cards .template-card").forEach((c) => c.classList.toggle("selected", c === card));
          });
        });
        $("#new-cancel").addEventListener("click", closeModal);
        $("#new-confirm").addEventListener("click", () => {
          const tpl = templates[selectedKind] || templates.rtsl;
          closeModal();
          state.current = null;
          state.source = tpl.source;
          state.originalSource = tpl.source;
          state.filename = tpl.filename;
          state.kind = tpl.kind;
          state.options = defaultOptions();
          state.dirty = false;
          renderOptions(state.options, false);
          renderExamples();
          setEditorSource(tpl.source);
          resetRunState();
          renderAllEmpty(`Created ${tpl.filename} template. Run Check or Analyze.`);
          focusLine(1, 1);
        });
      },
    });
  }

  function renderAllEmpty(reason) {
    renderOverview();
    renderMetrics();
    renderGraphs();
    renderSignals();
    renderConsole();
    renderDiagnostics();
    renderPending();
    updatePipeline();
    if (reason) setRunMeta(reason);
  }

  /* ------------------------------------------------------------------- start */

  boot();
})();
