// Panel core: protocol and policy logic with no DOM and no globals, so the
// logic is testable headlessly (tools/devtools/panel_test.mjs) and the DOM
// layer stays thin. Everything the panel does to the target flows through
// the authenticated transport routes; the token lives in memory only and
// must never appear in a URL, storage, or exported log line.

export const RPC_PATH = "/automation/v1/rpc";
export const SCHEMA_PATH = "/automation/v1/schema";
export const ARTIFACT_PATH = "/automation/v1/artifacts/";
export const SESSION_PATH = "/automation/v1/session";
export const SESSION_HEADER = "X-TabEngine-Session";

// Normalizes a user-typed key name to the engine's logical key contract:
// lowercase names from the bridge table (enter, f4, arrowleft, del, ...,
// single alphanumerics) plus optional ctrl/shift/alt modifier flags written
// as a chord ("ctrl+shift+s"). Modifier keys are not standalone keys in the
// protocol - they are boolean flags on a key event - so "ControlLeft"
// cannot be sent and is reported as unsupported instead of faked.
// Returns {ok:true, key, ctrl, shift, alt} or {ok:false, reason}.
export function normalizeKeyName(input) {
  const aliases = {
    enter: "enter", return: "enter", esc: "escape", escape: "escape",
    backspace: "backspace", tab: "tab", space: "space", spacebar: "space",
    pause: "pause", capslock: "capslock",
    pageup: "pageup", pagedown: "pagedown", end: "end", home: "home",
    insert: "insert", delete: "delete", del: "delete",
    f1: "f1", f2: "f2", f3: "f3", f4: "f4", f5: "f5", f6: "f6",
    f7: "f7", f8: "f8", f9: "f9", f10: "f10", f11: "f11", f12: "f12",
    arrowleft: "arrowleft", arrowup: "arrowup",
    arrowright: "arrowright", arrowdown: "arrowdown",
    left: "arrowleft", up: "arrowup", right: "arrowright", down: "arrowdown",
    multiply: "multiply", add: "add", separator: "separator",
    subtract: "subtract", decimal: "decimal", divide: "divide",
  };
  const text = String(input || "").trim();
  if (!text) return { ok: false, reason: "enter a key name" };
  const parts = text.split("+").map((part) => part.trim()).filter(Boolean);
  if (parts.length === 0) return { ok: false, reason: "enter a key name" };
  const keyPart = parts[parts.length - 1];
  const modifiers = { ctrl: false, shift: false, alt: false };
  for (const part of parts.slice(0, -1)) {
    const lower = part.toLowerCase();
    if (lower === "ctrl" || lower === "control") modifiers.ctrl = true;
    else if (lower === "shift") modifiers.shift = true;
    else if (lower === "alt") modifiers.alt = true;
    else return { ok: false, reason: "unknown modifier '" + part + "' (use ctrl, shift, alt)" };
  }
  const lower = keyPart.toLowerCase();
  if (["ctrl", "control", "controlleft", "controlright", "shift", "shiftleft",
       "shiftright", "alt", "altleft", "altright", "meta", "win", "super"]
      .includes(lower)) {
    return { ok: false,
             reason: "'" + keyPart + "' is a modifier flag (ctrl/shift/alt on another key), not a standalone key" };
  }
  let key = null;
  if (aliases[lower]) key = aliases[lower];
  else if (lower.length === 1 && /[a-z0-9]/.test(lower)) key = lower;
  else return { ok: false,
                reason: "unsupported key '" + keyPart + "' (lowercase logical names: enter, f1..f12, arrowleft, del, ..., single letters/digits)" };
  return { ok: true, key, ...modifiers };
}

// Creates the JSON-RPC client over an injected transport. `fetchText(path,
// options)` where options = {method, headers, body, signal} returns
// {status, body}; optional `fetchBytes` returns {status, bytes: ArrayBuffer}
// for artifacts (never text-decoded). The client adds the bearer header
// itself, keeps one logical input session alive across requests, stamps
// every call with a connection epoch, and aborts in-flight work when the
// epoch advances. The token is never placed in a path or query.
export function createRpcClient(transport, policy = {}) {
  const fetchText = typeof transport === "function" ? transport : transport.fetchText;
  const fetchBytes = typeof transport === "object" && transport.fetchBytes
    ? transport.fetchBytes : null;
  let token = "";
  let nextId = 1;
  let epoch = 0;
  let sessionId = "";
  const inFlight = new Set();
  const sessionHeaders = () => {
    const headers = { "Authorization": "Bearer " + token,
                      "Content-Type": "application/json" };
    if (sessionId) headers[SESSION_HEADER] = sessionId;
    return headers;
  };
  const rawFetch = async (path, options, reader = fetchText, abortOnEpoch = true) => {
    const controller = new AbortController();
    if (abortOnEpoch) inFlight.add(controller);
    try {
      return await reader(path, { ...options, signal: controller.signal });
    } finally {
      inFlight.delete(controller);
    }
  };
  const isSessionInvalid = (response) =>
    response.status === 404 && String(response.body || "").indexOf("session not found") >= 0;
  return {
    setToken(value) { token = String(value || ""); },
    hasToken() { return token.length > 0; },
    clearToken() { token = ""; },
    get sessionId() { return sessionId; },
    get epoch() { return epoch; },
    // Advances the connection epoch: every in-flight call still running
    // under an older epoch resolves as stale and its result is discarded
    // by the caller, so a reconnecting panel never mixes results from two
    // instances. Also aborts the in-flight fetches themselves.
    newEpoch() {
      epoch += 1;
      sessionId = "";
      for (const controller of inFlight) controller.abort();
      inFlight.clear();
    },
    // Begins one logical session (raw input gestures keep one connection
    // identity across down/move/up). Existing sessions are replaced.
    async beginSession() {
      if (!token) return { ok: false, error: "no token" };
      const attemptEpoch = epoch;
      let response;
      try {
        response = await rawFetch(SESSION_PATH, {
          method: "POST",
          headers: { "Authorization": "Bearer " + token },
          body: "",
          timeoutS: 10,
        });
      } catch (_) {
        return { ok: false, stale: epoch !== attemptEpoch,
                 error: "session begin failed: network error" };
      }
      if (epoch !== attemptEpoch) return { ok: false, stale: true, error: "stale epoch" };
      if (response.status !== 200) {
        return { ok: false, error: "session begin failed: http " + response.status };
      }
      try {
        sessionId = String(JSON.parse(response.body).sessionId || "");
      } catch (_) {
        return { ok: false, error: "session begin returned invalid JSON" };
      }
      if (!sessionId) return { ok: false, error: "session begin returned no id" };
      return { ok: true, sessionId };
    },
    // Ends the session explicitly (disconnect): releases any held input.
    async endSession() {
      if (!token || !sessionId) return { ok: !sessionId, ended: false };
      const id = sessionId;
      sessionId = "";
      try {
        // This best-effort release owns a snapshot of the old credentials.
        // Advancing the epoch must not abort the DELETE itself.
        await rawFetch(SESSION_PATH, {
          method: "DELETE",
          headers: { "Authorization": "Bearer " + token,
                     [SESSION_HEADER]: id },
          body: "",
          timeoutS: 5,
        }, fetchText, false);
      } catch (_) {
        // A failed end is tolerable: idle expiry or transport stop also
        // releases held input server-side.
      }
      return { ok: true, ended: true };
    },
    async call(method, params = {}, timeoutS = 10, options = {}) {
      const { retryOnNetworkError = false, sessionRetried = false } = options;
      const decision = policy.authorize ? policy.authorize(method) : { permitted: true };
      if (!decision.permitted) {
        return { ok: false, error: { code: 0, message: decision.reason || "action refused" } };
      }
      if (!token) {
        return { ok: false, error: { code: 0, message: "no token" } };
      }
      const body = JSON.stringify({
        jsonrpc: "2.0",
        id: String(nextId++),
        method,
        params,
      });
      const attemptEpoch = epoch;
      let response = null;
      try {
        response = await rawFetch(RPC_PATH, {
          method: "POST",
          headers: sessionHeaders(),
          body,
          timeoutS,
        });
      } catch (error) {
        // A response from a dead epoch is stale even when the fetch itself
        // aborted: discard it instead of surfacing a network error.
        if (epoch !== attemptEpoch) {
          return { ok: false, stale: true, error: { code: 0, message: "stale epoch" } };
        }
        // Network failure: the request may or may not have been dispatched,
        // so mutation calls never retry. Reads retry once, still within the
        // same epoch, and both failures surface to the caller.
        if (retryOnNetworkError) {
          try {
            response = await rawFetch(RPC_PATH, {
              method: "POST",
              headers: sessionHeaders(),
              body,
              timeoutS,
            });
          } catch (_) {
            return { ok: false, error: { code: 0, message: "network error" } };
          }
        } else {
          return { ok: false, error: { code: 0, message: "network error" } };
        }
      }
      // The response belongs to a previous connection: discard it - the
      // caller already moved on (reconnect switched windows/targets).
      if (epoch !== attemptEpoch) {
        return { ok: false, stale: true, error: { code: 0, message: "stale epoch" } };
      }
      if (response.status === 401) {
        return { ok: false, error: { code: 401, message: "unauthorized" }, unauthorized: true };
      }
      if (response.status === 429) {
        return { ok: false, error: { code: 429, message: "rate limited (auth lockout)" } };
      }
      // The transport validates sessions before dispatching any request,
      // so a session-expired reply means the request never ran. Drop the
      // dead session, begin a new one, and replay the same request exactly
      // once - safe for mutations too, because the request never dispatched.
      if (isSessionInvalid(response) && !sessionRetried) {
        const attemptEpoch = epoch;
        sessionId = "";
        const begun = await this.beginSession();
        if (begun.ok && epoch === attemptEpoch) {
          return this.call(method, params, timeoutS,
                           { retryOnNetworkError, sessionRetried: true });
        }
        return { ok: false,
                 error: { code: 0, message: "input session expired and could not be renewed" } };
      }
      if (response.status !== 200) {
        return { ok: false, error: { code: response.status, message: "http " + response.status } };
      }
      let envelope = null;
      try { envelope = JSON.parse(response.body); } catch (_) {
        return { ok: false, error: { code: -32700, message: "invalid JSON response" } };
      }
      if (envelope.error) {
        return { ok: false, error: envelope.error };
      }
      return { ok: true, result: envelope.result };
    },
    artifactUrl(id) {
      // The fetch for artifacts adds the header at request time; callers
      // must use fetchArtifact(id) and never append the token.
      return ARTIFACT_PATH + encodeURIComponent(id);
    },
    async fetchArtifact(id) {
      if (!token) return { ok: false, error: "no token" };
      if (!fetchBytes) {
        return { ok: false, error: "transport cannot fetch binary artifacts" };
      }
      const attemptEpoch = epoch;
      let response;
      try {
        response = await rawFetch(this.artifactUrl(id), {
          method: "GET",
          headers: sessionHeaders(),
          body: "",
          timeoutS: 15,
        }, fetchBytes);
      } catch (_) {
        return { ok: false, stale: epoch !== attemptEpoch,
                 error: "artifact fetch failed: network error" };
      }
      if (epoch !== attemptEpoch) {
        return { ok: false, stale: true, error: "stale epoch" };
      }
      if (response.status !== 200) {
        return { ok: false, error: "artifact fetch failed: " + response.status };
      }
      return { ok: true, bytes: response.bytes };
    },
    async fetchSchema() {
      if (!token) return { ok: false, error: "no token" };
      const attemptEpoch = epoch;
      let response;
      try {
        response = await rawFetch(SCHEMA_PATH, {
          method: "GET",
          headers: sessionHeaders(),
          body: "",
          timeoutS: 15,
        });
      } catch (_) {
        return { ok: false, stale: epoch !== attemptEpoch,
                 error: "schema fetch failed: network error" };
      }
      if (epoch !== attemptEpoch) return { ok: false, stale: true, error: "stale epoch" };
      if (response.status !== 200) {
        return { ok: false, error: "http " + response.status };
      }
      try { return { ok: true, result: JSON.parse(response.body) }; }
      catch (_) { return { ok: false, error: "invalid schema JSON" }; }
    },
  };
}

// Event cursor with gap semantics: the protocol reports a trimmed ring via
// a gap marker; a consumer must learn that it missed events instead of
// silently continuing from an arbitrary point.
export function createEventCursor() {
  let next = 0;
  let gaps = 0;
  return {
    get since() { return next; },
    get gapCount() { return gaps; },
    // A reconnect targets a new instance whose sequence space restarts:
    // callers reset the cursor on every fresh connect.
    reset() { next = 0; gaps = 0; },
    // `since` is the last sequence seen, exclusive. Event.poll returns
    // {events, next, gap, oldestAvailable}; preserve retained events on gaps.
    apply(batch) {
      const events = Array.isArray(batch && batch.events) ? batch.events : [];
      const cursor = Number(batch && batch.next);
      let gapped = false;
      if (batch && batch.gap) {
        gaps += 1;
        gapped = true;
        const oldest = Number(batch.oldestAvailable);
        if (Number.isFinite(oldest)) next = Math.max(next, oldest - 1);
      }
      const accepted = events.filter((event) => Number(event.sequence) > next);
      for (const event of accepted) {
        const sequence = Number(event.sequence);
        if (Number.isFinite(sequence) && sequence > next) {
          next = sequence;
        }
      }
      if (Number.isFinite(cursor)) next = Math.max(next, cursor);
      return { events: accepted, gapped, dropped: false };
    },
  };
}

// Per-source log cursor: Log.read takes an `after`/`appAfter` pair and
// answers {sources: {engine: {records, next}, app: {records, next}}}. The
// cursor keeps one position per source, merges newly fetched records, and
// applies a level filter (records below the minimum level never display,
// but their sequences still advance the cursor).
const LOG_LEVELS = ["debug", "info", "warning", "error"];
export function createLogCursor() {
  const positions = { engine: 0, app: 0 };
  let minLevelIndex = 0; // debug and above
  const reset = () => { positions.engine = 0; positions.app = 0; };
  return {
    get positions() { return { ...positions }; },
    get minLevel() { return LOG_LEVELS[minLevelIndex]; },
    setMinLevel(level) {
      const index = LOG_LEVELS.indexOf(String(level || "").toLowerCase());
      if (index >= 0) minLevelIndex = index;
    },
    reset,
    // Params for the next Log.read call. `max` bounds per-source batches.
    nextParams(max = 200) {
      return { after: positions.engine, appAfter: positions.app, max };
    },
    // Applies one Log.read result: returns the newly accepted records
    // (sorted, level-filtered) and advances each source cursor to the
    // server-reported `next` (or past the last seen sequence).
    apply(batch) {
      const sources = (batch && batch.sources) || {};
      const accepted = [];
      for (const name of ["engine", "app"]) {
        const source = sources[name];
        if (!source) continue;
        const records = Array.isArray(source.records) ? source.records : [];
        let highest = positions[name];
        for (const record of records) {
          const sequence = Number(record.sequence);
          if (!Number.isFinite(sequence) || sequence <= positions[name]) continue;
          const levelIndex = LOG_LEVELS.indexOf(String(record.level || "info").toLowerCase());
          if (levelIndex < 0 || levelIndex >= minLevelIndex) {
            accepted.push({ source: name, record });
          }
          if (sequence > highest) highest = sequence;
        }
        const next = Number(source.next);
        positions[name] = Number.isFinite(next) ? Math.max(positions[name], next) : highest;
      }
      accepted.sort((a, b) => a.record.sequence - b.record.sequence);
      return { records: accepted };
    },
  };
}

// Both screenshots and automation bounds/input use client physical pixels.
// Browser CSS scaling is undone by the caller; only the crop origin remains.
export function imageToClient(px, py, meta) {
  const bounds = (meta && meta.bounds) || { x: 0, y: 0 };
  if (![px, py, bounds.x, bounds.y].every(Number.isFinite)) {
    throw new Error("invalid capture coordinates");
  }
  return {
    x: Math.round(bounds.x + px),
    y: Math.round(bounds.y + py),
  };
}

export function nodeClickParams(windowId, node, snapshot) {
  return { windowId, node: String(node.node), snapshotId: snapshot.layoutRevision, strict: true };
}

export function wheelParams(windowId, notches, point = { x: 0, y: 0 }) {
  return { windowId, x: Math.round(point.x), y: Math.round(point.y),
           deltaY: Math.round(notches * 120) };
}

// Mutating methods stay refused until the operator explicitly opts in for
// the session; the gate is advisory UI policy - the server re-checks
// authorization on every request anyway.
export function createMutationGate() {
  let allow = false;
  return {
    get allowed() { return allow; },
    setAllowed(value) { allow = Boolean(value); },
    // Returns {permitted, reason} for a schema method descriptor
    // ({mutating: bool, grants: [..]}).
    check(method) {
      if (!method || !method.mutating) return { permitted: true };
      if (!allow) {
        return { permitted: false,
                 reason: "write actions are off (enable them in Capture & Input)" };
      }
      return { permitted: true };
    },
  };
}

// Strips every provided secret from text before it is shown or exported.
export function redact(text, secrets) {
  let out = String(text);
  for (const secret of secrets) {
    if (secret) {
      out = out.split(secret).join("«redacted»");
    }
  }
  return out;
}

// Builds a downloadable diagnostics snapshot. Secrets never enter it: the
// caller passes the redaction list; the core also drops any field whose
// name suggests credentials.
export function diagnosticsSnapshot(sections, secrets) {
  const clean = {};
  for (const [name, value] of Object.entries(sections || {})) {
    if (/token|secret|password|authorization/i.test(name)) continue;
    clean[name] = JSON.parse(JSON.stringify(value, (key, item) =>
      /token|secret|password|authorization/i.test(key) ? undefined : item));
  }
  return redact(JSON.stringify(clean, null, 2), secrets);
}
