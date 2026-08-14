// The six E Ink Spectra 6 pigments in the GxEPD2 order the firmware expects.
// Index into this array IS the wire value: 0 black … 5 yellow.
export const PIGMENTS = [
  { name: 'Black', rgb: [0, 0, 0], hex: '#000000' },
  { name: 'White', rgb: [255, 255, 255], hex: '#ffffff' },
  { name: 'Green', rgb: [0, 145, 70], hex: '#009146' },
  { name: 'Blue', rgb: [0, 75, 190], hex: '#004bbe' },
  { name: 'Red', rgb: [210, 30, 40], hex: '#d21e28' },
  { name: 'Yellow', rgb: [245, 205, 30], hex: '#f5cd1e' }
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
