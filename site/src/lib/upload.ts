// Session handling and the E1004 upload protocol. The request shapes here are
// load-bearing for iOS Safari and Chromium Local Network Access — change them
// only together with firmware (see AGENTS.md).

export type Session = { device: string; token: string };

export function readSession(): Session | null {
  const params = new URLSearchParams(location.hash.slice(1));
  const device = (params.get('device') ?? '').replace(/\/$/, '');
  const token = params.get('token') ?? '';
  return device && token ? { device, token } : null;
}

export function describeError(error: unknown): string {
  if (error instanceof Error) return error.message || error.name;
  if (typeof error === 'string') return error;
  try {
    const json = JSON.stringify(error);
    if (json && json !== '{}') return json;
  } catch {
    // Fall through for non-serializable browser/library errors.
  }
  return String(error);
}

export function localRequest(url: string, init: RequestInit): Request {
  // Chromium requires the Local Network Access hint. WebKit currently fails
  // the entire request when this Chromium-specific option is present.
  const chromium =
    /(?:Chrome|Chromium|Edg)\//.test(navigator.userAgent) &&
    !/(?:CriOS|EdgiOS)\//.test(navigator.userAgent);
  const options = chromium
    ? { ...init, mode: 'cors', targetAddressSpace: 'local' }
    : { ...init, mode: 'cors' };
  return new Request(url, options as RequestInit);
}

/**
 * Keeps the device's five-minute inactivity window open while the user edits,
 * mirroring the device-hosted uploader. Returns a stop function.
 */
export function startHeartbeat(session: Session, onExpired: () => void): () => void {
  let stopped = false;
  const beat = async () => {
    try {
      const response = await fetch(
        localRequest(`${session.device}/api/status`, {
          headers: { 'X-Upload-Token': session.token },
          cache: 'no-store'
        })
      );
      if (response.status === 401 && !stopped) onExpired();
    } catch {
      // Transient reachability problems surface on the upload path instead.
    }
  };
  beat();
  const timer = setInterval(beat, 15000);
  return () => {
    stopped = true;
    clearInterval(timer);
  };
}

export async function uploadPacked(
  session: Session,
  packed: Uint8Array,
  stage: (message: string) => void
): Promise<void> {
  const headers = { 'X-Upload-Token': session.token };
  stage('Checking the frame…');
  const statusResponse = await fetch(localRequest(`${session.device}/api/status`, { headers }));
  if (!statusResponse.ok) {
    const result = await statusResponse.json().catch(() => ({}));
    throw new Error(result.error ?? `display returned HTTP ${statusResponse.status}`);
  }
  stage('Sending 960 KB to the frame…');
  const response = await fetch(
    localRequest(`${session.device}/api/image/${encodeURIComponent(session.token)}`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/octet-stream', ...headers },
      body: packed.buffer as ArrayBuffer
    })
  );
  const result = await response.json();
  if (!response.ok) throw new Error(result.error ?? `HTTP ${response.status}`);
}
