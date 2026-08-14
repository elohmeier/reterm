(() => {
  const root = document.querySelector('#reterm-uploader');
  const token = window.RETERM_TOKEN || '';
  root.innerHTML = `<h1>Photo Magic ✦</h1><p class="status">Choose a photo while the display is waiting.</p>
    <div class="card"><label class="pick">Choose photo<input class="file" type="file" accept="image/*,.heic,.heif"></label>
    <div class="row"><label>Fit<select class="fit"><option value="cover">Fill screen</option><option value="contain">Fit whole photo</option></select></label>
    <label>Rotation<select class="rotation"><option value="0">0°</option><option value="90">90°</option><option value="-90">−90°</option><option value="180">180°</option></select></label></div>
    <label>Zoom<input class="zoom" type="range" min="1" max="3" step=".01" value="1"></label>
    <label>Move left/right<input class="x" type="range" min="-1" max="1" step=".01" value="0"></label>
    <label>Move up/down<input class="y" type="range" min="-1" max="1" step=".01" value="0"></label>
    <canvas width="300" height="400"></canvas><button disabled>Make colors & send</button></div>`;
  const status = root.querySelector('.status');
  const file = root.querySelector('.file');
  const fit = root.querySelector('.fit');
  const rotation = root.querySelector('.rotation');
  const zoom = root.querySelector('.zoom');
  const offsetX = root.querySelector('.x');
  const offsetY = root.querySelector('.y');
  const preview = root.querySelector('canvas');
  const button = root.querySelector('button');
  let image;

  function draw(canvas, width, height) {
    canvas.width = width;
    canvas.height = height;
    const context = canvas.getContext('2d', { alpha: false });
    context.fillStyle = 'white';
    context.fillRect(0, 0, width, height);
    const angle = Number(rotation.value);
    const sideways = Math.abs(angle % 180) === 90;
    const orientedWidth = sideways ? image.naturalHeight : image.naturalWidth;
    const orientedHeight = sideways ? image.naturalWidth : image.naturalHeight;
    const scale = (fit.value === 'cover'
      ? Math.max(width / orientedWidth, height / orientedHeight)
      : Math.min(width / orientedWidth, height / orientedHeight)) * Number(zoom.value);
    context.save();
    context.translate(width / 2 + Number(offsetX.value) * width * .35,
      height / 2 + Number(offsetY.value) * height * .35);
    context.rotate(angle * Math.PI / 180);
    context.drawImage(image, -image.naturalWidth * scale / 2,
      -image.naturalHeight * scale / 2, image.naturalWidth * scale, image.naturalHeight * scale);
    context.restore();
  }

  function redraw() { if (image) draw(preview, 300, 400); }
  [fit, rotation, zoom, offsetX, offsetY].forEach(control => control.addEventListener('input', redraw));
  file.addEventListener('change', async () => {
    if (!file.files[0]) return;
    status.textContent = 'Decoding photo…';
    const url = URL.createObjectURL(file.files[0]);
    try {
      image = new Image();
      image.src = url;
      await image.decode();
      const normal = Math.abs(Math.log((image.naturalWidth / image.naturalHeight) / .75));
      const sideways = Math.abs(Math.log((image.naturalHeight / image.naturalWidth) / .75));
      rotation.value = sideways < normal ? '90' : '0';
      draw(preview, 300, 400);
      button.disabled = false;
      status.textContent = 'Adjust the crop, then tap the red button.';
    } catch (error) {
      status.textContent = `Could not decode photo: ${error.message || error}`;
    } finally { URL.revokeObjectURL(url); }
  });

  button.addEventListener('click', async () => {
    button.disabled = true;
    status.textContent = 'Resizing and making six e-paper colors…';
    await new Promise(resolve => setTimeout(resolve));
    const width = 1200, height = 1600;
    const canvas = document.createElement('canvas');
    draw(canvas, width, height);
    const source = canvas.getContext('2d', { alpha: false }).getImageData(0, 0, width, height).data;
    const palette = [[0,0,0],[255,255,255],[0,145,70],[0,75,190],[210,30,40],[245,205,30]];
    const packed = new Uint8Array(width * height / 2);
    let current = new Float32Array((width + 2) * 3), next = new Float32Array((width + 2) * 3);
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const pixel = (y * width + x) * 4, error = (x + 1) * 3;
        const rgb = [0,1,2].map(channel => Math.max(0, Math.min(255, source[pixel + channel] + current[error + channel])));
        let selected = 0, best = Infinity;
        palette.forEach((color, index) => {
          const dr=rgb[0]-color[0], dg=rgb[1]-color[1], db=rgb[2]-color[2];
          const distance=dr*dr*.3+dg*dg*.59+db*db*.11;
          if (distance < best) { best=distance; selected=index; }
        });
        const packedIndex = (y * width + x) >> 1;
        if (x & 1) packed[packedIndex] |= selected; else packed[packedIndex] = selected << 4;
        for (let channel=0; channel<3; channel++) {
          const value=rgb[channel]-palette[selected][channel];
          current[error+3+channel]+=value*7/16; next[error-3+channel]+=value*3/16;
          next[error+channel]+=value*5/16; next[error+3+channel]+=value/16;
        }
      }
      current=next; next=new Float32Array((width+2)*3);
    }
    status.textContent = 'Uploading to the display…';
    try {
      const response = await fetch('/api/image', { method: 'POST',
        headers: { 'Content-Type': 'application/octet-stream', 'X-Upload-Token': token }, body: packed });
      const result = await response.json();
      if (!response.ok) throw new Error(result.error || `HTTP ${response.status}`);
      status.textContent = 'Received! The display will refresh in about 30 seconds.';
    } catch (error) {
      status.textContent = `Upload failed: ${error.message || error}`;
      button.disabled = false;
    }
  });
})();
