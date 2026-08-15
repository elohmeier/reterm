// Device profiles for the reTerminal E-series frames the editor can target.
// The firmware QR link carries `model=<id>`; `/api/status` confirms it. The
// pigment array order IS the wire format (palette index for 4bpp, bit value
// for 1bpp), and E1004 colors are calibrated measured values — see
// docs/color-calibration.md.
export type Pigment = { name: string; rgb: [number, number, number]; hex: string };
export type Packing = '4bpp' | '1bpp';

export type DeviceProfile = {
  id: string; // wire model id, e.g. "reterminal-e1004"
  short: string; // "E1004"
  tagline: string;
  width: number;
  height: number;
  packing: Packing;
  packedBytes: number;
  pigments: Pigment[];
  color: boolean;
  refreshNote: string;
};

const E1004: DeviceProfile = {
  id: 'reterminal-e1004',
  short: 'E1004',
  tagline: 'six-ink photo lab',
  width: 1200,
  height: 1600,
  packing: '4bpp',
  packedBytes: (1200 * 1600) / 2,
  pigments: [
    { name: 'Black', rgb: [0, 0, 0], hex: '#000000' },
    { name: 'White', rgb: [255, 255, 255], hex: '#ffffff' },
    { name: 'Green', rgb: [46, 174, 96], hex: '#2eae60' },
    { name: 'Blue', rgb: [0, 118, 175], hex: '#0076af' },
    { name: 'Red', rgb: [204, 0, 0], hex: '#cc0000' },
    { name: 'Yellow', rgb: [255, 209, 0], hex: '#ffd100' }
  ],
  color: true,
  refreshNote: 'the panel refresh takes about 30 seconds'
};

const E1001: DeviceProfile = {
  id: 'reterminal-e1001',
  short: 'E1001',
  tagline: 'ink & paper photo lab',
  width: 800,
  height: 480,
  packing: '1bpp',
  packedBytes: (800 * 480) / 8,
  pigments: [
    { name: 'Black', rgb: [0, 0, 0], hex: '#000000' },
    { name: 'White', rgb: [255, 255, 255], hex: '#ffffff' }
  ],
  color: false,
  refreshNote: 'the panel refresh takes a few seconds'
};

export const DEVICES: Record<string, DeviceProfile> = {
  [E1004.id]: E1004,
  [E1001.id]: E1001
};

export const DEFAULT_DEVICE = E1004;

export function deviceById(model: string | null | undefined): DeviceProfile | null {
  if (!model) return null;
  const id = model.toLowerCase();
  return DEVICES[id] ?? DEVICES[`reterminal-${id}`] ?? null;
}

/**
 * The model hint travels next to the session token: in the query string on
 * device-served pages, in the hash on the Pages-served Chromium path.
 */
export function resolveDevice(): DeviceProfile {
  const hash = new URLSearchParams(location.hash.slice(1));
  const query = new URLSearchParams(location.search);
  return deviceById(hash.get('model')) ?? deviceById(query.get('model')) ?? DEFAULT_DEVICE;
}

export type InkingId =
  | 'floyd'
  | 'atkinson'
  | 'jarvis'
  | 'bayer'
  | 'bayer4'
  | 'bayer2'
  | 'dot'
  | 'line'
  | 'cross'
  | 'engrave'
  | 'photocopy'
  | 'flat';

/**
 * Tunables for the screen-based inkings. Pitch and angle drive the halftone
 * and etching screens; threshold and grain drive the photocopy look. Always
 * sent with a job; styles ignore what they do not use.
 */
export type InkingParams = {
  pitch: number;
  angle: number;
  threshold: number;
  grain: number;
};

export function defaultInkingParams(profile: DeviceProfile): InkingParams {
  // A pitch that reads as newsprint at each panel's size and viewing distance.
  const pitch = Math.min(16, Math.max(5, Math.round(Math.min(profile.width, profile.height) / 80)));
  return { pitch, angle: 45, threshold: 0.5, grain: 0.35 };
}

/** Screens that expose the pitch (and for halftones, angle) sliders. */
export function usesScreen(id: InkingId): boolean {
  return id === 'dot' || id === 'line' || id === 'cross' || id === 'engrave';
}

export function usesAngle(id: InkingId): boolean {
  return id === 'dot' || id === 'line' || id === 'cross';
}

export const INKINGS: { id: InkingId; label: string; detail: string }[] = [
  { id: 'floyd', label: 'Classic', detail: 'Floyd–Steinberg' },
  { id: 'atkinson', label: 'Retro', detail: 'Atkinson' },
  { id: 'jarvis', label: 'Smooth', detail: 'Jarvis' },
  { id: 'bayer', label: 'Print', detail: 'Bayer 8×8' },
  { id: 'bayer4', label: 'Arcade', detail: 'Bayer 4×4' },
  { id: 'bayer2', label: 'Pixel', detail: 'Bayer 2×2' },
  { id: 'dot', label: 'Halftone', detail: 'dot screen' },
  { id: 'line', label: 'Linotone', detail: 'line screen' },
  { id: 'cross', label: 'Crosshatch', detail: 'crossed screens' },
  { id: 'engrave', label: 'Etching', detail: 'flow hatching' },
  { id: 'photocopy', label: 'Xerox', detail: 'threshold + grain' },
  { id: 'flat', label: 'Poster', detail: 'nearest ink' }
];
