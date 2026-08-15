// Quantizes an RGBA frame to the target device's pigments and packs it into
// the firmware's GxEPD2 wire format: two 4-bit palette indices per byte for
// color panels (first pixel in the high nibble), or eight 1-bit pixels per
// byte for monochrome panels (bit set = white, MSB is the leftmost pixel).
// Several inking styles share one code path.
import type { InkingId, Packing } from './devices';

type Job = {
  rgba: ArrayBuffer;
  width: number;
  height: number;
  inking: InkingId;
  palette: [number, number, number][];
  packing: Packing;
  wantPacked: boolean;
};

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
const BAYER_SPREAD = 96;

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

self.onmessage = (event: MessageEvent<Job>) => {
  const { rgba, width, height, inking, palette, packing, wantPacked } = event.data;
  const source = new Uint8ClampedArray(rgba);
  const preview = new Uint8ClampedArray(source.length);
  const packed = new Uint8Array(packing === '1bpp' ? (width * height) / 8 : (width * height) / 2);
  const kernel = KERNELS[inking];

  // Three rolling error rows with a two-pixel apron so kernels never bounds-check.
  const rowSize = (width + 4) * 3;
  let rows = [new Float32Array(rowSize), new Float32Array(rowSize), new Float32Array(rowSize)];
  const progressStep = Math.max(1, Math.floor(height / 10));

  for (let y = 0; y < height; y += 1) {
    const reverse = kernel !== undefined && (y & 1) === 1;
    for (let step = 0; step < width; step += 1) {
      const x = reverse ? width - 1 - step : step;
      const pixel = (y * width + x) * 4;
      const error = (x + 2) * 3;
      let red = source[pixel];
      let green = source[pixel + 1];
      let blue = source[pixel + 2];
      if (kernel) {
        red += rows[0][error];
        green += rows[0][error + 1];
        blue += rows[0][error + 2];
      } else if (inking === 'bayer') {
        const threshold = ((BAYER8[(y & 7) * 8 + (x & 7)] + 0.5) / 64 - 0.5) * BAYER_SPREAD;
        red += threshold;
        green += threshold;
        blue += threshold;
      }
      red = red < 0 ? 0 : red > 255 ? 255 : red;
      green = green < 0 ? 0 : green > 255 ? 255 : green;
      blue = blue < 0 ? 0 : blue > 255 ? 255 : blue;

      const selected = nearest(palette, red, green, blue);
      const [pr, pg, pb] = palette[selected];
      preview[pixel] = pr;
      preview[pixel + 1] = pg;
      preview[pixel + 2] = pb;
      preview[pixel + 3] = 255;
      const linear = y * width + x;
      if (packing === '1bpp') {
        // Pigment 1 is white; GxEPD2 monochrome buffers use 1 = white.
        if (selected === 1) packed[linear >> 3] |= 0x80 >> (x & 7);
      } else if ((x & 1) === 0) {
        packed[linear >> 1] = selected << 4;
      } else {
        packed[linear >> 1] |= selected;
      }

      if (kernel) {
        const er = red - pr;
        const eg = green - pg;
        const eb = blue - pb;
        for (const [dx, dy, weight] of kernel) {
          const target = (x + (reverse ? -dx : dx) + 2) * 3;
          rows[dy][target] += er * weight;
          rows[dy][target + 1] += eg * weight;
          rows[dy][target + 2] += eb * weight;
        }
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
