// Panel core: protocol and policy logic with no DOM and no globals, so the
// logic is testable headlessly (tools/devtools/panel_test.mjs) and the DOM
// layer stays thin. Everything the panel does to the target flows through
// the authenticated transport routes; the token lives in memory only and
// must never appear in a URL, storage, or exported log line.

export const RPC_PATH = "/automation/v1/rpc";
export const SCHEMA_PATH = "/automation/v1/schema";
export const ARTIFACT_PATH = "/automation/v1/artifacts/";

// Creates the JSON-RPC client over an injected `fetchText(path, options)`
// where options = {method, headers, body}. `fetchText` returns
// {status, body}. The client adds the bearer header itself; the token is
// never placed in a path or query. `transport.fetchBytes` (optional) must
// return {status, bytes: ArrayBuffer} - artifacts are binary and must never
// round-trip through text decoding.
export function createRpcClient(transport, policy = {}) {
  const fetchText = typeof transport === "function" ? transport : transport.fetchText;
  const fetchBytes = typeof transport === "object" && transport.fetchBytes
    ? transport.fetchBytes : null;
  let token = "";
  let nextId = 1;
  return {
    setToken(value) { token = String(value || ""); },
    hasToken() { return token.length > 0; },
    clearToken() { token = ""; },
    async call(method, params = {}, timeoutS = 10) {
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
      const response = await fetchText(RPC_PATH, {
        method: "POST",
        headers: { "Authorization": "Bearer " + token,
                   "Content-Type": "application/json" },
        body,
        timeoutS,
      });
      if (response.status === 401) {
        return { ok: false, error: { code: 401, message: "unauthorized" }, unauthorized: true };
      }
      if (response.status === 429) {
        return { ok: false, error: { code: 429, message: "rate limited (auth lockout)" } };
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
      const response = await fetchBytes(this.artifactUrl(id), {
        method: "GET",
        headers: { "Authorization": "Bearer " + token },
        body: "",
        timeoutS: 15,
      });
      if (response.status !== 200) {
        return { ok: false, error: "artifact fetch failed: " + response.status };
      }
      return { ok: true, bytes: response.bytes };
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

export function keyParams(windowId, key, kind) {
  return { windowId, key, kind };
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
