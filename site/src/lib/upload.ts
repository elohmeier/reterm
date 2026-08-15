// Session handling and the device upload protocol. The request shapes here
// are load-bearing for iOS Safari and Chromium Local Network Access — change
// them only together with firmware (see AGENTS.md).

// device is '' when the page is served by the frame itself; API calls are
// then same-origin relative requests. model is the URL's device hint; the
// authoritative value arrives with /api/status.
export type Session = { device: string; token: string; model: string };

export type DeviceStatus = {
  model?: string;
  width?: number;
  height?: number;
  bytes?: number;
  format?: string;
  palette?: string[];
};

export function readSession(): Session | null {
  const params = new URLSearchParams(location.hash.slice(1));
  const device = (params.get('device') ?? '').replace(/\/$/, '');
  const hashToken = params.get('token') ?? '';
  if (device && hashToken) {
    return { device, token: hashToken, model: params.get('model') ?? '' };
  }
  // Device-served page: the firmware's QR puts token and model in the query.
  const query = new URLSearchParams(location.search);
  const token = query.get('token') ?? '';
  return token ? { device: '', token, model: query.get('model') ?? '' } : null;
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
  // Same-origin (device-served page): no CORS or address-space hints needed.
  if (url.startsWith('/')) return new Request(url, init);
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
 * mirroring the device-hosted uploader. Reports the first successful status
 * body so the app can confirm the device model. Returns a stop function.
 */
export function startHeartbeat(
  session: Session,
  onExpired: () => void,
  onStatus?: (status: DeviceStatus) => void
): () => void {
  let stopped = false;
  let reported = false;
  const beat = async () => {
    try {
      const response = await fetch(
        localRequest(`${session.device}/api/status`, {
          headers: { 'X-Upload-Token': session.token },
          cache: 'no-store'
        })
      );
      if (response.status === 401 && !stopped) onExpired();
      if (response.ok && !reported && !stopped && onStatus) {
        const status = (await response.json().catch(() => null)) as DeviceStatus | null;
        if (status) {
          reported = true;
          onStatus(status);
        }
      }
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
  const status = (await statusResponse.json().catch(() => ({}))) as DeviceStatus;
  if (typeof status.bytes === 'number' && status.bytes !== packed.byteLength) {
    throw new Error(
      `this frame is a ${status.model ?? 'different device'} — reopen its QR link and try again`
    );
  }
  stage(`Sending ${Math.round(packed.byteLength / 1000)} KB to the frame…`);
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
