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
// never placed in a path or query.
export function createRpcClient(fetchText) {
  let token = "";
  let nextId = 1;
  return {
    setToken(value) { token = String(value || ""); },
    hasToken() { return token.length > 0; },
    clearToken() { token = ""; },
    async call(method, params = {}, timeoutS = 10) {
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
      // must use fetchText(artifactUrl(id)) and never append the token.
      return ARTIFACT_PATH + encodeURIComponent(id);
    },
    async fetchArtifact(id) {
      if (!token) return { ok: false, error: "no token" };
      const response = await fetchText(this.artifactUrl(id), {
        method: "GET",
        headers: { "Authorization": "Bearer " + token },
        body: "",
        timeoutS: 15,
      });
      if (response.status !== 200) {
        return { ok: false, error: "artifact fetch failed: " + response.status };
      }
      return { ok: true, body: response.body };
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
    // Applies one Event.poll result ({events, nextCursor, gap?}) and
    // returns the accepted events. On a gap, the cursor resets to the
    // server's nextCursor and flags the discontinuity once.
    apply(batch) {
      const events = Array.isArray(batch && batch.events) ? batch.events : [];
      const cursor = Number(batch && batch.nextCursor);
      let gapped = false;
      if (batch && batch.gap) {
        gaps += 1;
        gapped = true;
        next = Number.isFinite(cursor) ? cursor : 0;
        return { events: [], gapped, dropped: true };
      }
      for (const event of events) {
        const sequence = Number(event.sequence);
        if (Number.isFinite(sequence) && sequence >= next) {
          next = sequence + 1;
        }
      }
      return { events, gapped, dropped: false };
    },
  };
}

// Maps a point in screenshot pixels back to window client logical
// coordinates. `meta` = {scale, bounds:{x,y}} from the capture result and
// the window entry. Pixel data is premultiplied sRGB; the mapping is the
// exact inverse of what the renderer applied.
export function imageToClient(px, py, meta) {
  const rawScale = meta ? Number(meta.scale) : NaN;
  if (!(rawScale > 0)) throw new Error("invalid scale");
  const bounds = (meta && meta.bounds) || { x: 0, y: 0 };
  return {
    x: bounds.x + px / rawScale,
    y: bounds.y + py / rawScale,
  };
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
    clean[name] = JSON.parse(JSON.stringify(value));
  }
  return redact(JSON.stringify(clean, null, 2), secrets);
}
