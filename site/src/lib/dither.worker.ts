// Quantizes an RGBA frame to the target device's pigments and packs it into
// the firmware's GxEPD2 wire format: two 4-bit palette indices per byte for
// color panels (first pixel in the high nibble), or eight 1-bit pixels per
// byte for monochrome panels (bit set = white, MSB is the leftmost pixel).
//
// Inking families sharing this one pass:
// - diffusion: Floyd–Steinberg / Atkinson / Jarvis error diffusion
// - ordered: Bayer 8×8 / 4×4 / 2×2 threshold matrices
// - screen: rotated halftone dot / line / crosshatch screens (pitch + angle)
// - engrave: luminance-adaptive hatching that follows image contours
// - photocopy: hard threshold with hash-noise grain
// - flat: nearest ink
// Screens, engraving, and photocopy are AM-style: they draw with black and
// white only (palette indices 0 and 1), which both panels share.
//
// An optional tone mask (128 = neutral) dodges/burns luminance before
// quantization, and an optional zone mask switches marked pixels to a second
// inking so one design can mix two styles.
import type { InkingId, InkingParams, Packing } from './devices';

type Job = {
  rgba: ArrayBuffer;
  width: number;
  height: number;
  inking: InkingId;
  params: InkingParams;
  palette: [number, number, number][];
  packing: Packing;
  wantPacked: boolean;
  tone?: ArrayBuffer | null;
  zone?: ArrayBuffer | null;
  zoneInking?: InkingId | null;
};

type Kind = 'diffusion' | 'ordered' | 'screen' | 'engrave' | 'photocopy' | 'flat';

function kindOf(id: InkingId): Kind {
  if (id === 'floyd' || id === 'atkinson' || id === 'jarvis') return 'diffusion';
  if (id === 'bayer' || id === 'bayer4' || id === 'bayer2') return 'ordered';
  if (id === 'dot' || id === 'line' || id === 'cross') return 'screen';
  if (id === 'engrave') return 'engrave';
  if (id === 'photocopy') return 'photocopy';
  return 'flat';
}

// Error-diffusion kernels as [dx, dy, weight]. dx is mirrored on
// right-to-left rows (serpentine scanning hides directional worm artifacts).
const KERNELS: Record<string, [number, number, number][]> = {
  floyd: [
    [1, 0, 7 / 16],
    [-1, 1, 3 / 16],
    [0, 1, 5 / 16],
    [1, 1, 1 / 16]
  ],
  atkinson: [
    [1, 0, 1 / 8],
    [2, 0, 1 / 8],
    [-1, 1, 1 / 8],
    [0, 1, 1 / 8],
    [1, 1, 1 / 8],
    [0, 2, 1 / 8]
  ],
  jarvis: [
    [1, 0, 7 / 48],
    [2, 0, 5 / 48],
    [-2, 1, 3 / 48],
    [-1, 1, 5 / 48],
    [0, 1, 7 / 48],
    [1, 1, 5 / 48],
    [2, 1, 3 / 48],
    [-2, 2, 1 / 48],
    [-1, 2, 3 / 48],
    [0, 2, 5 / 48],
    [1, 2, 3 / 48],
    [2, 2, 1 / 48]
  ]
};

// prettier-ignore
const BAYER8 = [
  0, 32, 8, 40, 2, 34, 10, 42,
  48, 16, 56, 24, 50, 18, 58, 26,
  12, 44, 4, 36, 14, 46, 6, 38,
  60, 28, 52, 20, 62, 30, 54, 22,
  3, 35, 11, 43, 1, 33, 9, 41,
  51, 19, 59, 27, 49, 17, 57, 25,
  15, 47, 7, 39, 13, 45, 5, 37,
  63, 31, 55, 23, 61, 29, 53, 21
];
// prettier-ignore
const BAYER4 = [
  0, 8, 2, 10,
  12, 4, 14, 6,
  3, 11, 1, 9,
  15, 7, 13, 5
];
const BAYER2 = [0, 2, 3, 1];
const BAYER_SPREAD = 96;

const ORDERED: Partial<Record<InkingId, { matrix: number[]; size: number }>> = {
  bayer: { matrix: BAYER8, size: 8 },
  bayer4: { matrix: BAYER4, size: 4 },
  bayer2: { matrix: BAYER2, size: 2 }
};

function nearest(
  palette: [number, number, number][],
  red: number,
  green: number,
  blue: number
): number {
  let selected = 0;
  let best = Number.POSITIVE_INFINITY;
  for (let index = 0; index < palette.length; index += 1) {
    const [r, g, b] = palette[index];
    const dr = red - r;
    const dg = green - g;
    const db = blue - b;
    const distance = dr * dr * 0.3 + dg * dg * 0.59 + db * db * 0.11;
    if (distance < best) {
      best = distance;
      selected = index;
    }
  }
  return selected;
}

/** Deterministic per-pixel noise in [0, 1) so proofs are stable across runs. */
function hashNoise(x: number, y: number): number {
  let h = (x * 374761393 + y * 668265263) | 0;
  h = Math.imul(h ^ (h >>> 13), 1274126177);
  h ^= h >>> 16;
  return (h >>> 0) / 4294967296;
}

const TAU = Math.PI * 2;

type Field = { c2: Float32Array; s2: Float32Array; gw: number; gh: number; block: number };

/**
 * Contour orientation for the etching screen, on a coarse grid: Sobel
 * gradients of the downsampled luminance, accumulated as doubled-angle
 * vectors (so opposite gradients reinforce instead of canceling), then box
 * blurred into a smooth flow field.
 */
function orientationField(source: Uint8ClampedArray, width: number, height: number): Field {
  const block = 8;
  const gw = Math.max(3, Math.ceil(width / block));
  const gh = Math.max(3, Math.ceil(height / block));
  const small = new Float32Array(gw * gh);
  const counts = new Float32Array(gw * gh);
  for (let y = 0; y < height; y += 1) {
    const gy = Math.min(gh - 1, (y / block) | 0);
    for (let x = 0; x < width; x += 1) {
      const pixel = (y * width + x) * 4;
      const luma =
        source[pixel] * 0.299 + source[pixel + 1] * 0.587 + source[pixel + 2] * 0.114;
      const cell = gy * gw + Math.min(gw - 1, (x / block) | 0);
      small[cell] += luma;
      counts[cell] += 1;
    }
  }
  for (let cell = 0; cell < small.length; cell += 1) {
    if (counts[cell] > 0) small[cell] /= counts[cell];
  }

  const c2 = new Float32Array(gw * gh);
  const s2 = new Float32Array(gw * gh);
  for (let gy = 1; gy < gh - 1; gy += 1) {
    for (let gx = 1; gx < gw - 1; gx += 1) {
      const at = (dx: number, dy: number) => small[(gy + dy) * gw + gx + dx];
      const dx =
        at(1, -1) + 2 * at(1, 0) + at(1, 1) - (at(-1, -1) + 2 * at(-1, 0) + at(-1, 1));
      const dy =
        at(-1, 1) + 2 * at(0, 1) + at(1, 1) - (at(-1, -1) + 2 * at(0, -1) + at(1, -1));
      const magnitude = Math.hypot(dx, dy);
      if (magnitude < 1) continue;
      const theta = Math.atan2(dy, dx);
      const cell = gy * gw + gx;
      c2[cell] = magnitude * Math.cos(2 * theta);
      s2[cell] = magnitude * Math.sin(2 * theta);
    }
  }

  // Two separable box-blur passes, radius 2, on both components (in place).
  const radius = 2;
  for (let pass = 0; pass < 2; pass += 1) {
    for (const grid of [c2, s2]) {
      const buffer = new Float32Array(gw * gh);
      for (let gy = 0; gy < gh; gy += 1) {
        for (let gx = 0; gx < gw; gx += 1) {
          let sum = 0;
          let n = 0;
          for (let k = -radius; k <= radius; k += 1) {
            const sx = gx + k;
            if (sx < 0 || sx >= gw) continue;
            sum += grid[gy * gw + sx];
            n += 1;
          }
          buffer[gy * gw + gx] = sum / n;
        }
      }
      for (let gx = 0; gx < gw; gx += 1) {
        for (let gy = 0; gy < gh; gy += 1) {
          let sum = 0;
          let n = 0;
          for (let k = -radius; k <= radius; k += 1) {
            const sy = gy + k;
            if (sy < 0 || sy >= gh) continue;
            sum += buffer[sy * gw + gx];
            n += 1;
          }
          grid[gy * gw + gx] = sum / n;
        }
      }
    }
  }
  return { c2, s2, gw, gh, block };
}

/** Bilinear sample of the doubled-angle field; returns the stripe direction. */
function stripeAngle(field: Field, x: number, y: number): number {
  const fx = Math.min(field.gw - 1.001, Math.max(0, x / field.block - 0.5));
  const fy = Math.min(field.gh - 1.001, Math.max(0, y / field.block - 0.5));
  const x0 = fx | 0;
  const y0 = fy | 0;
  const tx = fx - x0;
  const ty = fy - y0;
  const index = y0 * field.gw + x0;
  const lerp = (grid: Float32Array) =>
    grid[index] * (1 - tx) * (1 - ty) +
    grid[index + 1] * tx * (1 - ty) +
    grid[index + field.gw] * (1 - tx) * ty +
    grid[index + field.gw + 1] * tx * ty;
  const c2 = lerp(field.c2);
  const s2 = lerp(field.s2);
  // Weak gradients fall back to a classic 45° burin stroke.
  if (c2 * c2 + s2 * s2 < 0.25) return Math.PI / 4;
  // Perpendicular to the gradient = along the contour.
  return 0.5 * Math.atan2(s2, c2) + Math.PI / 2;
}

self.onmessage = (event: MessageEvent<Job>) => {
  const { rgba, width, height, inking, palette, packing, wantPacked } = event.data;
  const params: InkingParams = event.data.params ?? {
    pitch: 8,
    angle: 45,
    threshold: 0.5,
    grain: 0.35
  };
  const tone = event.data.tone ? new Uint8Array(event.data.tone) : null;
  const zone = event.data.zone ? new Uint8Array(event.data.zone) : null;
  const zoneInking = zone ? (event.data.zoneInking ?? inking) : inking;
  const source = new Uint8ClampedArray(rgba);
  const preview = new Uint8ClampedArray(source.length);
  const packed = new Uint8Array(packing === '1bpp' ? (width * height) / 8 : (width * height) / 2);

  const mainKind = kindOf(inking);
  const zoneKind = kindOf(zoneInking);
  const field =
    mainKind === 'engrave' || zoneKind === 'engrave'
      ? orientationField(source, width, height)
      : null;

  const pitch = Math.max(3, params.pitch);
  const angleRadians = (params.angle * Math.PI) / 180;
  const cosA = Math.cos(angleRadians);
  const sinA = Math.sin(angleRadians);
  const grainAmp = params.grain * 160;
  const copyThreshold = params.threshold * 255;

  // Three rolling error rows with a two-pixel apron so kernels never bounds-check.
  const rowSize = (width + 4) * 3;
  const rows = [new Float32Array(rowSize), new Float32Array(rowSize), new Float32Array(rowSize)];
  const anyDiffusion = mainKind === 'diffusion' || zoneKind === 'diffusion';
  const progressStep = Math.max(1, Math.floor(height / 10));

  for (let y = 0; y < height; y += 1) {
    const reverse = anyDiffusion && (y & 1) === 1;
    for (let step = 0; step < width; step += 1) {
      const x = reverse ? width - 1 - step : step;
      const linear = y * width + x;
      const pixel = linear * 4;
      const errorIndex = (x + 2) * 3;

      const bias = tone ? (tone[linear] - 128) * 1.15 : 0;
      let red = source[pixel] + bias;
      let green = source[pixel + 1] + bias;
      let blue = source[pixel + 2] + bias;

      const styleId = zone && zone[linear] > 127 ? zoneInking : inking;
      const kind = zone && zone[linear] > 127 ? zoneKind : mainKind;
      const kernel = kind === 'diffusion' ? KERNELS[styleId] : undefined;

      let selected: number;
      if (kind === 'screen' || kind === 'engrave' || kind === 'photocopy') {
        // AM-style screens draw in pure black and white from luminance.
        const clampedRed = red < 0 ? 0 : red > 255 ? 255 : red;
        const clampedGreen = green < 0 ? 0 : green > 255 ? 255 : green;
        const clampedBlue = blue < 0 ? 0 : blue > 255 ? 255 : blue;
        const gray = clampedRed * 0.299 + clampedGreen * 0.587 + clampedBlue * 0.114;
        let black = false;
        if (kind === 'photocopy') {
          black = gray + (hashNoise(x, y) - 0.5) * grainAmp < copyThreshold;
        } else if (kind === 'engrave' && field) {
          if (gray <= 240) {
            const theta = stripeAngle(field, x, y);
            const darkness = 1 - gray / 255;
            const stripe = (x * Math.cos(theta) + y * Math.sin(theta)) / pitch;
            const phase = stripe - Math.floor(stripe);
            const coverage = Math.min(1, darkness * 1.3);
            if (Math.abs(2 * phase - 1) < coverage) black = true;
            else if (darkness > 0.55) {
              const crossStripe =
                (x * -Math.sin(theta) + y * Math.cos(theta)) / pitch;
              const crossPhase = crossStripe - Math.floor(crossStripe);
              const crossCoverage = Math.min(1, (darkness - 0.55) * 2.4);
              if (Math.abs(2 * crossPhase - 1) < crossCoverage) black = true;
            }
          }
        } else {
          const u = (x * cosA + y * sinA) / pitch;
          const v = (-x * sinA + y * cosA) / pitch;
          let threshold: number;
          if (styleId === 'dot') {
            threshold = ((Math.cos(u * TAU) + Math.cos(v * TAU)) / 4 + 0.5) * 255;
          } else if (styleId === 'line') {
            threshold = (Math.cos(u * TAU) / 2 + 0.5) * 255;
          } else {
            threshold =
              Math.max(Math.cos(u * TAU), Math.cos(v * TAU)) * 0.5 * 255 + 127.5;
          }
          black = gray < threshold;
        }
        selected = black ? 0 : 1;
      } else {
        if (kernel) {
          red += rows[0][errorIndex];
          green += rows[0][errorIndex + 1];
          blue += rows[0][errorIndex + 2];
        } else if (kind === 'ordered') {
          const { matrix, size } = ORDERED[styleId]!;
          const cells = size * size;
          const threshold =
            ((matrix[(y % size) * size + (x % size)] + 0.5) / cells - 0.5) * BAYER_SPREAD;
          red += threshold;
          green += threshold;
          blue += threshold;
        }
        red = red < 0 ? 0 : red > 255 ? 255 : red;
        green = green < 0 ? 0 : green > 255 ? 255 : green;
        blue = blue < 0 ? 0 : blue > 255 ? 255 : blue;
        selected = nearest(palette, red, green, blue);
        if (kernel) {
          const [pr, pg, pb] = palette[selected];
          const errRed = red - pr;
          const errGreen = green - pg;
          const errBlue = blue - pb;
          for (const [dx, dy, weight] of kernel) {
            const target = (x + (reverse ? -dx : dx) + 2) * 3;
            rows[dy][target] += errRed * weight;
            rows[dy][target + 1] += errGreen * weight;
            rows[dy][target + 2] += errBlue * weight;
          }
        }
      }

      const [pr, pg, pb] = palette[selected];
      preview[pixel] = pr;
      preview[pixel + 1] = pg;
      preview[pixel + 2] = pb;
      preview[pixel + 3] = 255;
      if (packing === '1bpp') {
        // Pigment 1 is white; GxEPD2 monochrome buffers use 1 = white.
        if (selected === 1) packed[linear >> 3] |= 0x80 >> (x & 7);
      } else if ((x & 1) === 0) {
        packed[linear >> 1] = selected << 4;
      } else {
        packed[linear >> 1] |= selected;
      }
    }
    const recycled = rows.shift()!;
    recycled.fill(0);
    rows.push(recycled);
    if (wantPacked && (y + 1) % progressStep === 0) {
      self.postMessage({ progress: Math.round(((y + 1) * 100) / height) });
    }
  }

  const post = self.postMessage as unknown as (message: unknown, transfer: Transferable[]) => void;
  if (wantPacked) post({ packed: packed.buffer, preview: preview.buffer }, [packed.buffer, preview.buffer]);
  else post({ preview: preview.buffer }, [preview.buffer]);
};
