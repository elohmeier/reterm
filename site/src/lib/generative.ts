// Generative prints: no photo required. Each pattern renders pure black ink
// on white at exact panel resolution, so it survives any inking style and
// turns the frame into a daily generative art print. Every call rerolls.
export type PatternId = 'flow' | 'truchet' | 'maze' | 'cells';

export const PATTERNS: { id: PatternId; label: string; detail: string }[] = [
  { id: 'flow', label: 'Flow', detail: 'field lines' },
  { id: 'truchet', label: 'Truchet', detail: 'arc tiles' },
  { id: 'maze', label: 'Maze', detail: 'one way out' },
  { id: 'cells', label: 'Cells', detail: 'reaction–diffusion' }
];

export function renderPattern(id: PatternId, width: number, height: number): HTMLCanvasElement {
  const canvas = document.createElement('canvas');
  canvas.width = width;
  canvas.height = height;
  const context = canvas.getContext('2d')!;
  context.fillStyle = '#ffffff';
  context.fillRect(0, 0, width, height);
  context.fillStyle = '#000000';
  context.strokeStyle = '#000000';
  context.lineCap = 'round';
  context.lineJoin = 'round';
  if (id === 'flow') drawFlow(context, width, height);
  else if (id === 'truchet') drawTruchet(context, width, height);
  else if (id === 'maze') drawMaze(context, width, height);
  else drawCells(context, width, height);
  return canvas;
}

/** Smooth value noise in [0, 1] on a wrapping lattice. */
function makeNoise(cell: number): (x: number, y: number) => number {
  const size = 64;
  const lattice = new Float32Array(size * size);
  for (let index = 0; index < lattice.length; index += 1) lattice[index] = Math.random();
  const at = (x: number, y: number) => lattice[(y & (size - 1)) * size + (x & (size - 1))];
  return (x, y) => {
    const fx = x / cell;
    const fy = y / cell;
    const x0 = Math.floor(fx);
    const y0 = Math.floor(fy);
    const tx = fx - x0;
    const ty = fy - y0;
    const sx = tx * tx * (3 - 2 * tx);
    const sy = ty * ty * (3 - 2 * ty);
    const top = at(x0, y0) * (1 - sx) + at(x0 + 1, y0) * sx;
    const bottom = at(x0, y0 + 1) * (1 - sx) + at(x0 + 1, y0 + 1) * sx;
    return top * (1 - sy) + bottom * sy;
  };
}

function drawFlow(context: CanvasRenderingContext2D, width: number, height: number) {
  const min = Math.min(width, height);
  const angleNoise = makeNoise(min / 3.4);
  const weightNoise = makeNoise(min / 5);
  const count = Math.round((width * height) / 2600);
  const step = Math.max(2, min / 240);
  for (let line = 0; line < count; line += 1) {
    let x = Math.random() * width;
    let y = Math.random() * height;
    const steps = 30 + Math.floor(Math.random() * 70);
    context.beginPath();
    context.lineWidth = 1 + weightNoise(x, y) * (min / 130);
    context.moveTo(x, y);
    for (let index = 0; index < steps; index += 1) {
      const theta = angleNoise(x, y) * Math.PI * 4;
      x += Math.cos(theta) * step;
      y += Math.sin(theta) * step;
      if (x < -20 || x > width + 20 || y < -20 || y > height + 20) break;
      context.lineTo(x, y);
    }
    context.stroke();
  }
}

function drawTruchet(context: CanvasRenderingContext2D, width: number, height: number) {
  const min = Math.min(width, height);
  const tile = Math.max(24, Math.round(min / 10));
  context.lineWidth = Math.max(4, Math.round(tile * 0.16));
  for (let y = 0; y < height + tile; y += tile) {
    for (let x = 0; x < width + tile; x += tile) {
      const flip = Math.random() < 0.5;
      context.beginPath();
      if (flip) {
        context.arc(x, y, tile / 2, 0, Math.PI / 2);
        context.moveTo(x + tile, y + tile);
        context.arc(x + tile, y + tile, tile / 2, Math.PI, Math.PI * 1.5);
      } else {
        context.arc(x + tile, y, tile / 2, Math.PI / 2, Math.PI);
        context.moveTo(x, y + tile);
        context.arc(x, y + tile, tile / 2, Math.PI * 1.5, Math.PI * 2);
      }
      context.stroke();
    }
  }
}

function drawMaze(context: CanvasRenderingContext2D, width: number, height: number) {
  const min = Math.min(width, height);
  const cell = Math.max(16, Math.round(min / 22));
  const cols = Math.floor(width / cell);
  const rows = Math.floor(height / cell);
  const offsetX = Math.floor((width - cols * cell) / 2);
  const offsetY = Math.floor((height - rows * cell) / 2);
  const wall = Math.max(4, Math.round(cell * 0.28));

  // Recursive backtracker; right/bottom walls per cell, carved as we go.
  const right = new Uint8Array(cols * rows).fill(1);
  const bottom = new Uint8Array(cols * rows).fill(1);
  const visited = new Uint8Array(cols * rows);
  const stack: number[] = [0];
  visited[0] = 1;
  while (stack.length > 0) {
    const current = stack[stack.length - 1];
    const cx = current % cols;
    const cy = (current / cols) | 0;
    const options: [number, number][] = [];
    if (cx > 0 && !visited[current - 1]) options.push([current - 1, 0]);
    if (cx < cols - 1 && !visited[current + 1]) options.push([current + 1, 1]);
    if (cy > 0 && !visited[current - cols]) options.push([current - cols, 2]);
    if (cy < rows - 1 && !visited[current + cols]) options.push([current + cols, 3]);
    if (options.length === 0) {
      stack.pop();
      continue;
    }
    const [next, direction] = options[Math.floor(Math.random() * options.length)];
    if (direction === 0) right[next] = 0;
    else if (direction === 1) right[current] = 0;
    else if (direction === 2) bottom[next] = 0;
    else bottom[current] = 0;
    visited[next] = 1;
    stack.push(next);
  }

  const px = (column: number) => offsetX + column * cell;
  const py = (row: number) => offsetY + row * cell;
  context.fillRect(px(0) - wall / 2, py(0) - wall / 2, cols * cell + wall, wall);
  context.fillRect(px(0) - wall / 2, py(0) - wall / 2, wall, rows * cell + wall);
  context.fillRect(px(0) - wall / 2, py(rows) - wall / 2, cols * cell + wall, wall);
  context.fillRect(px(cols) - wall / 2, py(0) - wall / 2, wall, rows * cell + wall);
  for (let cy = 0; cy < rows; cy += 1) {
    for (let cx = 0; cx < cols; cx += 1) {
      const cellIndex = cy * cols + cx;
      if (right[cellIndex] && cx < cols - 1) {
        context.fillRect(px(cx + 1) - wall / 2, py(cy) - wall / 2, wall, cell + wall);
      }
      if (bottom[cellIndex] && cy < rows - 1) {
        context.fillRect(px(cx) - wall / 2, py(cy + 1) - wall / 2, cell + wall, wall);
      }
    }
  }
}

function drawCells(context: CanvasRenderingContext2D, width: number, height: number) {
  // Gray–Scott reaction–diffusion at coarse resolution, then a smooth
  // upscale re-thresholded to pure black and white.
  const scale = Math.max(3, Math.round(Math.min(width, height) / 130));
  const sw = Math.max(32, Math.ceil(width / scale));
  const sh = Math.max(32, Math.ceil(height / scale));
  const u = new Float32Array(sw * sh).fill(1);
  const v = new Float32Array(sw * sh);
  for (let seed = 0; seed < 25; seed += 1) {
    const cx = 2 + Math.floor(Math.random() * (sw - 4));
    const cy = 2 + Math.floor(Math.random() * (sh - 4));
    for (let dy = -1; dy <= 1; dy += 1) {
      for (let dx = -1; dx <= 1; dx += 1) v[(cy + dy) * sw + cx + dx] = 1;
    }
  }
  const feed = 0.055;
  const kill = 0.062;
  const nextU = new Float32Array(sw * sh);
  const nextV = new Float32Array(sw * sh);
  for (let iteration = 0; iteration < 400; iteration += 1) {
    for (let y = 0; y < sh; y += 1) {
      const up = y === 0 ? 0 : -sw;
      const down = y === sh - 1 ? 0 : sw;
      for (let x = 0; x < sw; x += 1) {
        const index = y * sw + x;
        const left = x === 0 ? 0 : -1;
        const rightStep = x === sw - 1 ? 0 : 1;
        const lapU =
          u[index + up] + u[index + down] + u[index + left] + u[index + rightStep] - 4 * u[index];
        const lapV =
          v[index + up] + v[index + down] + v[index + left] + v[index + rightStep] - 4 * v[index];
        const reaction = u[index] * v[index] * v[index];
        nextU[index] = u[index] + 0.21 * lapU - reaction + feed * (1 - u[index]);
        nextV[index] = v[index] + 0.105 * lapV + reaction - (feed + kill) * v[index];
      }
    }
    u.set(nextU);
    v.set(nextV);
  }

  const small = document.createElement('canvas');
  small.width = sw;
  small.height = sh;
  const smallContext = small.getContext('2d')!;
  const smallImage = smallContext.createImageData(sw, sh);
  for (let index = 0; index < v.length; index += 1) {
    const value = v[index] > 0.17 ? 0 : 255;
    smallImage.data[index * 4] = value;
    smallImage.data[index * 4 + 1] = value;
    smallImage.data[index * 4 + 2] = value;
    smallImage.data[index * 4 + 3] = 255;
  }
  smallContext.putImageData(smallImage, 0, 0);
  context.imageSmoothingEnabled = true;
  context.imageSmoothingQuality = 'high';
  context.drawImage(small, 0, 0, width, height);
  const full = context.getImageData(0, 0, width, height);
  for (let index = 0; index < full.data.length; index += 4) {
    const value = full.data[index] < 128 ? 0 : 255;
    full.data[index] = value;
    full.data[index + 1] = value;
    full.data[index + 2] = value;
  }
  context.putImageData(full, 0, 0);
}
