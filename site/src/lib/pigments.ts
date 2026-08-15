// Measured E1004-COLOR-V1 pigments with linear-light black-point compensation.
// This keeps the calibrated hues but maps reflective paper black/white back to
// input-space 0/255. Index into this array IS the GxEPD2 wire value.
export const PIGMENTS = [
  { name: 'Black', rgb: [0, 0, 0], hex: '#000000' },
  { name: 'White', rgb: [255, 255, 255], hex: '#ffffff' },
  { name: 'Green', rgb: [46, 174, 96], hex: '#2eae60' },
  { name: 'Blue', rgb: [0, 118, 175], hex: '#0076af' },
  { name: 'Red', rgb: [204, 0, 0], hex: '#cc0000' },
  { name: 'Yellow', rgb: [255, 209, 0], hex: '#ffd100' }
] as const;

export const SCREEN_WIDTH = 1200;
export const SCREEN_HEIGHT = 1600;
export const PACKED_BYTES = (SCREEN_WIDTH * SCREEN_HEIGHT) / 2;

export type InkingId = 'floyd' | 'atkinson' | 'jarvis' | 'bayer' | 'flat';

export const INKINGS: { id: InkingId; label: string; detail: string }[] = [
  { id: 'floyd', label: 'Classic', detail: 'Floyd–Steinberg' },
  { id: 'atkinson', label: 'Retro', detail: 'Atkinson' },
  { id: 'jarvis', label: 'Smooth', detail: 'Jarvis' },
  { id: 'bayer', label: 'Print', detail: 'Bayer 8×8' },
  { id: 'flat', label: 'Poster', detail: 'nearest ink' }
];
