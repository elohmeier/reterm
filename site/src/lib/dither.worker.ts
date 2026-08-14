const palette = [
  [0, 0, 0],
  [255, 255, 255],
  [0, 145, 70],
  [0, 75, 190],
  [210, 30, 40],
  [245, 205, 30]
] as const;

self.onmessage = (event: MessageEvent<{ rgba: ArrayBuffer; width: number; height: number }>) => {
  const { rgba, width, height } = event.data;
  const source = new Uint8ClampedArray(rgba);
  const preview = new Uint8ClampedArray(source.length);
  const packed = new Uint8Array(width * height / 2);
  let current = new Float32Array((width + 2) * 3);
  let next = new Float32Array((width + 2) * 3);

  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const pixel = (y * width + x) * 4;
      const error = (x + 1) * 3;
      const red = Math.max(0, Math.min(255, source[pixel] + current[error]));
      const green = Math.max(0, Math.min(255, source[pixel + 1] + current[error + 1]));
      const blue = Math.max(0, Math.min(255, source[pixel + 2] + current[error + 2]));

      let selected = 0;
      let best = Number.POSITIVE_INFINITY;
      for (let index = 0; index < palette.length; index += 1) {
        const color = palette[index];
        const dr = red - color[0];
        const dg = green - color[1];
        const db = blue - color[2];
        const distance = dr * dr * 0.30 + dg * dg * 0.59 + db * db * 0.11;
        if (distance < best) {
          best = distance;
          selected = index;
        }
      }

      const chosen = palette[selected];
      preview[pixel] = chosen[0];
      preview[pixel + 1] = chosen[1];
      preview[pixel + 2] = chosen[2];
      preview[pixel + 3] = 255;
      const packedIndex = (y * width + x) >> 1;
      if ((x & 1) === 0) packed[packedIndex] = selected << 4;
      else packed[packedIndex] |= selected;

      const er = red - chosen[0];
      const eg = green - chosen[1];
      const eb = blue - chosen[2];
      for (let channel = 0; channel < 3; channel += 1) {
        const value = channel === 0 ? er : channel === 1 ? eg : eb;
        current[error + 3 + channel] += value * 7 / 16;
        next[error - 3 + channel] += value * 3 / 16;
        next[error + channel] += value * 5 / 16;
        next[error + 3 + channel] += value / 16;
      }
    }
    current = next;
    next = new Float32Array((width + 2) * 3);
  }

  const post = self.postMessage as unknown as
    (message: unknown, transfer: Transferable[]) => void;
  post({ packed: packed.buffer, preview: preview.buffer },
       [packed.buffer, preview.buffer]);
};
