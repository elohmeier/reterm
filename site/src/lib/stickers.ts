// Flat sticker shapes drawn only with the target device's pigments, so they
// land on the panel exactly as previewed. Paths are centered on (0,0),
// roughly 100 units. On monochrome frames the colored fills collapse to black
// so the shapes stay crisp instead of dithering to gray.
import type { DeviceProfile } from './devices';

export type StickerDef = {
  id: string;
  label: string;
  path: string;
  fill: string;
  stroke?: string;
  strokeWidth?: number;
  opacity?: number;
};

function starPath(points: number, outer: number, inner: number): string {
  const parts: string[] = [];
  for (let index = 0; index < points * 2; index += 1) {
    const radius = index % 2 === 0 ? outer : inner;
    const angle = (Math.PI * index) / points - Math.PI / 2;
    const command = index === 0 ? 'M' : 'L';
    parts.push(`${command} ${(radius * Math.cos(angle)).toFixed(1)} ${(radius * Math.sin(angle)).toFixed(1)}`);
  }
  return `${parts.join(' ')} Z`;
}

function sunPath(): string {
  const parts = ['M 26 0 A 26 26 0 1 0 -26 0 A 26 26 0 1 0 26 0'];
  for (let ray = 0; ray < 8; ray += 1) {
    const angle = (Math.PI * ray) / 4;
    const spread = Math.PI / 22;
    const tip = `${(50 * Math.cos(angle)).toFixed(1)} ${(50 * Math.sin(angle)).toFixed(1)}`;
    const left = `${(33 * Math.cos(angle - spread)).toFixed(1)} ${(33 * Math.sin(angle - spread)).toFixed(1)}`;
    const right = `${(33 * Math.cos(angle + spread)).toFixed(1)} ${(33 * Math.sin(angle + spread)).toFixed(1)}`;
    parts.push(`M ${left} L ${tip} L ${right} Z`);
  }
  return parts.join(' ');
}

export function stickersFor(profile: DeviceProfile): StickerDef[] {
  const hex = (name: string) =>
    profile.pigments.find((pigment) => pigment.name === name)?.hex ?? profile.pigments[0].hex;
  const black = hex('Black');
  const white = hex('White');
  const yellow = profile.color ? hex('Yellow') : black;
  const red = profile.color ? hex('Red') : black;
  const blue = profile.color ? hex('Blue') : black;

  // Print-culture motifs for the monochrome frame: woodcut and letterpress
  // shapes that read as designed artwork instead of collapsed color.
  const monochromeExtras: StickerDef[] = [
    {
      id: 'moon',
      label: 'Moon',
      path: 'M 12 -46 A 46 46 0 1 0 12 46 A 37 37 0 1 1 12 -46 Z',
      fill: black
    },
    {
      id: 'fleuron',
      label: 'Fleuron',
      path:
        'M 0 48 C -34 28 -44 -6 -20 -34 C -8 -46 6 -48 18 -42 C 4 -30 -4 -16 -2 2 ' +
        'C 10 -10 26 -12 40 -2 C 34 22 18 40 0 48 Z',
      fill: black
    },
    {
      id: 'divider',
      label: 'Divider',
      path: 'M -50 -2 H -12 V 2 H -50 Z M 12 -2 H 50 V 2 H 12 Z M 0 -9 L 9 0 L 0 9 L -9 0 Z',
      fill: black
    },
    {
      id: 'postmark',
      label: 'Postmark',
      path:
        'M 46 0 A 46 46 0 1 0 -46 0 A 46 46 0 1 0 46 0 ' +
        'M 36 0 A 36 36 0 1 0 -36 0 A 36 36 0 1 0 36 0 ' +
        'M -50 -14 Q -38 -22 -25 -14 T 0 -14 T 25 -14 T 50 -14 ' +
        'M -50 0 Q -38 -8 -25 0 T 0 0 T 25 0 T 50 0 ' +
        'M -50 14 Q -38 6 -25 14 T 0 14 T 25 14 T 50 14',
      fill: '',
      stroke: black,
      strokeWidth: 4
    },
    {
      id: 'corner',
      label: 'Corner',
      path:
        'M -46 14 L -46 -32 Q -46 -46 -32 -46 L 14 -46 L 14 -36 L -28 -36 ' +
        'Q -36 -36 -36 -28 L -36 14 Z M -24 -18 L -18 -24 L 2 -24 L -24 2 Z',
      fill: black
    }
  ];

  return [
    {
      id: 'sparkle',
      label: 'Sparkle',
      path: 'M 0 -50 C 6 -18 18 -6 50 0 C 18 6 6 18 0 50 C -6 18 -18 6 -50 0 C -18 -6 -6 -18 0 -50 Z',
      fill: yellow
    },
    { id: 'star', label: 'Star', path: starPath(5, 50, 21), fill: yellow },
    {
      id: 'heart',
      label: 'Heart',
      path: 'M 0 42 C -46 8 -50 -20 -32 -34 C -18 -44 -4 -38 0 -26 C 4 -38 18 -44 32 -34 C 50 -20 46 8 0 42 Z',
      fill: red
    },
    { id: 'burst', label: 'Burst', path: starPath(12, 50, 38), fill: red },
    { id: 'sun', label: 'Sun', path: sunPath(), fill: yellow },
    {
      id: 'bubble',
      label: 'Bubble',
      path: 'M -50 -35 Q -50 -45 -40 -45 L 40 -45 Q 50 -45 50 -35 L 50 10 Q 50 20 40 20 L -5 20 L -22 40 L -18 20 L -40 20 Q -50 20 -50 10 Z',
      fill: white,
      stroke: black,
      strokeWidth: 4
    },
    {
      id: 'arrow',
      label: 'Arrow',
      path: 'M -50 -14 L 8 -14 L 8 -32 L 50 0 L 8 32 L 8 14 L -50 14 Z',
      fill: blue
    },
    {
      id: 'tape',
      label: 'Tape',
      path: 'M -50 -16 L 50 -16 L 44 -8 L 50 0 L 44 8 L 50 16 L -50 16 L -44 8 L -50 0 L -44 -8 Z',
      fill: yellow,
      opacity: 0.75
    },
    ...(profile.color ? [] : monochromeExtras)
  ];
}
