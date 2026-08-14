<script lang="ts">
  import { onMount } from 'svelte';
  import heic2any from 'heic2any';

  const width = 1200;
  const height = 1600;
  let source: ImageBitmap | HTMLImageElement | null = null;
  let previewCanvas: HTMLCanvasElement;
  let resultCanvas: HTMLCanvasElement;
  let device = '';
  let token = '';
  let fit: 'cover' | 'contain' = 'cover';
  let rotation = 0;
  let zoom = 1;
  let offsetX = 0;
  let offsetY = 0;
  let status = 'Scan the QR code on your E1004 to begin.';
  let busy = false;
  let packed: Uint8Array | null = null;

  onMount(() => {
    const params = new URLSearchParams(location.hash.slice(1));
    device = (params.get('device') ?? '').replace(/\/$/, '');
    token = params.get('token') ?? '';
    if (device && token) status = 'Connected link received. Choose a photo.';
  });

  function describeError(error: unknown): string {
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

  async function decodeBlob(blob: Blob): Promise<ImageBitmap | HTMLImageElement> {
    try {
      return await createImageBitmap(blob, { imageOrientation: 'from-image' });
    } catch (bitmapError) {
      const url = URL.createObjectURL(blob);
      try {
        const image = new Image();
        image.decoding = 'async';
        image.src = url;
        await image.decode();
        return image;
      } catch (imageError) {
        throw new Error(
          `browser decoder: ${describeError(bitmapError)}; image fallback: ${describeError(imageError)}`
        );
      } finally {
        URL.revokeObjectURL(url);
      }
    }
  }

  function releaseSource() {
    if (source instanceof ImageBitmap) source.close();
    source = null;
  }

  async function choose(event: Event) {
    const input = event.currentTarget as HTMLInputElement;
    const selected = input.files?.[0];
    if (!selected) return;
    busy = true;
    status = 'Decoding photo…';
    try {
      const isHeic = /hei[cf]/i.test(selected.type) || /\.hei[cf]$/i.test(selected.name);
      let decodedSource: ImageBitmap | HTMLImageElement;
      try {
        // Safari can decode HEIC natively even when createImageBitmap cannot.
        decodedSource = await decodeBlob(selected);
      } catch (nativeError) {
        if (!isHeic) throw nativeError;
        try {
          const converted = await heic2any({ blob: selected, toType: 'image/jpeg', quality: 0.95 });
          const jpeg = Array.isArray(converted) ? converted[0] : converted;
          if (!(jpeg instanceof Blob)) throw new Error('converter returned no image');
          decodedSource = await decodeBlob(jpeg);
        } catch (conversionError) {
          throw new Error(
            `native HEIC support failed (${describeError(nativeError)}); ` +
            `HEIC conversion failed (${describeError(conversionError)})`
          );
        }
      }
      releaseSource();
      source = decodedSource;
      const normalError = Math.abs(Math.log((source.width / source.height) / (width / height)));
      const rotatedError = Math.abs(Math.log((source.height / source.width) / (width / height)));
      rotation = rotatedError < normalError ? 90 : 0;
      zoom = 1;
      offsetX = 0;
      offsetY = 0;
      packed = null;
      draw(previewCanvas, 450, 600);
      status = rotation ? 'Photo loaded and auto-rotated.' : 'Photo loaded.';
    } catch (error) {
      status = `Could not decode this photo: ${describeError(error)}`;
    } finally {
      busy = false;
    }
  }

  function draw(canvas: HTMLCanvasElement, targetWidth: number, targetHeight: number) {
    if (!source || !canvas) return;
    canvas.width = targetWidth;
    canvas.height = targetHeight;
    const context = canvas.getContext('2d', { alpha: false })!;
    context.fillStyle = 'white';
    context.fillRect(0, 0, targetWidth, targetHeight);
    const radians = rotation * Math.PI / 180;
    const sideways = Math.abs(rotation % 180) === 90;
    const orientedWidth = sideways ? source.height : source.width;
    const orientedHeight = sideways ? source.width : source.height;
    const scaleX = targetWidth / orientedWidth;
    const scaleY = targetHeight / orientedHeight;
    const scale = (fit === 'cover' ? Math.max(scaleX, scaleY) : Math.min(scaleX, scaleY)) * zoom;
    context.save();
    context.translate(targetWidth / 2 + offsetX * targetWidth * 0.35,
                      targetHeight / 2 + offsetY * targetHeight * 0.35);
    context.rotate(radians);
    context.scale(scale, scale);
    context.imageSmoothingEnabled = true;
    context.imageSmoothingQuality = 'high';
    context.drawImage(source, -source.width / 2, -source.height / 2);
    context.restore();
  }

  function redraw() {
    packed = null;
    draw(previewCanvas, 450, 600);
  }

  async function dither() {
    if (!source) return;
    busy = true;
    status = 'Resizing and dithering 1.92 million pixels…';
    draw(resultCanvas, width, height);
    const context = resultCanvas.getContext('2d', { alpha: false })!;
    const image = context.getImageData(0, 0, width, height);
    const worker = new Worker(new URL('../lib/dither.worker.ts', import.meta.url), { type: 'module' });
    const result = await new Promise<{ packed: ArrayBuffer; preview: ArrayBuffer }>((resolve, reject) => {
      worker.onmessage = (event) => resolve(event.data);
      worker.onerror = reject;
      worker.postMessage({ rgba: image.data.buffer, width, height }, [image.data.buffer]);
    });
    worker.terminate();
    packed = new Uint8Array(result.packed);
    context.putImageData(new ImageData(new Uint8ClampedArray(result.preview), width, height), 0, 0);
    status = 'Six-color preview ready.';
    busy = false;
  }

  function localRequest(url: string, init: RequestInit) {
    return new Request(url, { ...init, mode: 'cors', targetAddressSpace: 'local' } as RequestInit);
  }

  async function upload() {
    if (!packed || !device || !token) return;
    busy = true;
    status = 'Requesting local-network access and uploading 960 KB…';
    try {
      const response = await fetch(localRequest(`${device}/api/image`, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/octet-stream',
          'X-Upload-Token': token
        },
        body: packed.buffer as ArrayBuffer
      }));
      const result = await response.json();
      if (!response.ok) throw new Error(result.error ?? `HTTP ${response.status}`);
      status = 'Photo received! The e-paper refresh takes about 30 seconds.';
      token = '';
      history.replaceState(null, '', location.pathname);
    } catch (error) {
      status = `Upload failed: ${String(error)}. Allow local-network access and try again.`;
    } finally {
      busy = false;
    }
  }
</script>

<svelte:head><title>reTerminal Photo Magic</title></svelte:head>

<main>
  <header>
    <span class="sparkle">✦</span>
    <div><p class="eyebrow">reTerminal E1004</p><h1>Photo Magic</h1></div>
    <span class="sparkle green">✦</span>
  </header>

  <p class="status" class:ready={device && token}>{status}</p>

  <section class="workspace">
    <div class="panel controls">
      <label class="picker">Choose a photo<input type="file" accept="image/*,.heic,.heif" onchange={choose} /></label>

      <div class="row">
        <label>Fit<select bind:value={fit} onchange={redraw}><option value="cover">Fill screen</option><option value="contain">Fit whole photo</option></select></label>
        <label>Rotation<select bind:value={rotation} onchange={redraw}><option value={0}>0°</option><option value={90}>90°</option><option value={-90}>−90°</option><option value={180}>180°</option></select></label>
      </div>
      <label>Zoom <strong>{zoom.toFixed(2)}×</strong><input type="range" min="1" max="3" step="0.01" bind:value={zoom} oninput={redraw} /></label>
      <label>Move left/right<input type="range" min="-1" max="1" step="0.01" bind:value={offsetX} oninput={redraw} /></label>
      <label>Move up/down<input type="range" min="-1" max="1" step="0.01" bind:value={offsetY} oninput={redraw} /></label>

      <button class="secondary" onclick={dither} disabled={!source || busy}>Create six-color preview</button>
      <button class="primary" onclick={upload} disabled={!packed || !device || !token || busy}>Send to display</button>
    </div>

    <div class="panel preview">
      <div class="screen"><canvas bind:this={previewCanvas}></canvas></div>
      <p>Crop preview</p>
    </div>
    <div class="panel preview" class:hidden={!packed}>
      <div class="screen"><canvas bind:this={resultCanvas}></canvas></div>
      <p>Actual pigment preview</p>
    </div>
  </section>
</main>

<style>
  :global(*) { box-sizing: border-box; }
  :global(body) { margin: 0; color: #17213a; background: #fff7df; font-family: ui-rounded, "Avenir Next", system-ui, sans-serif; }
  :global(button), :global(input), :global(select) { font: inherit; }
  main { width: min(1180px, 94vw); margin: 0 auto; padding: 32px 0 70px; }
  header { display: flex; justify-content: center; align-items: center; gap: 28px; text-align: center; }
  h1 { margin: 0; font-size: clamp(2.6rem, 7vw, 5.5rem); line-height: .9; color: #1259ba; letter-spacing: -.05em; }
  .eyebrow { margin: 0 0 8px; color: #d22538; font-weight: 900; letter-spacing: .18em; text-transform: uppercase; }
  .sparkle { font-size: 4rem; color: #edb915; transform: rotate(12deg); }.sparkle.green { color: #159653; transform: rotate(-12deg); }
  .status { max-width: 720px; margin: 26px auto; padding: 14px 20px; border: 3px solid #17213a; border-radius: 999px; background: white; text-align: center; font-weight: 800; box-shadow: 5px 5px 0 #17213a; }
  .status.ready { border-color: #159653; box-shadow: 5px 5px 0 #159653; }
  .workspace { display: grid; grid-template-columns: minmax(270px, .85fr) repeat(2, minmax(250px, 1fr)); gap: 22px; align-items: start; }
  .panel { padding: 22px; border: 3px solid #17213a; border-radius: 28px; background: white; box-shadow: 7px 7px 0 #17213a; }
  .controls { display: grid; gap: 20px; }.controls label { display: grid; gap: 7px; font-weight: 800; }.row { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }
  select, input[type="range"] { width: 100%; }.picker { padding: 17px; border-radius: 16px; background: #edb915; cursor: pointer; text-align: center; }.picker input { display: none; }
  button { border: 3px solid #17213a; border-radius: 16px; padding: 15px 12px; font-weight: 900; cursor: pointer; box-shadow: 4px 4px 0 #17213a; }
  button:disabled { opacity: .4; cursor: not-allowed; }.primary { color: white; background: #d22538; }.secondary { background: #68b9f1; }
  .preview { text-align: center; }.screen { overflow: hidden; aspect-ratio: 3 / 4; border: 2px solid #17213a; background: #eee; }.screen canvas { width: 100%; height: 100%; display: block; object-fit: contain; image-rendering: auto; }.preview p { margin: 15px 0 0; font-weight: 900; }.hidden { visibility: hidden; }
  @media (max-width: 900px) { .workspace { grid-template-columns: 1fr 1fr; }.controls { grid-column: 1 / -1; }.hidden { display: none; } }
  @media (max-width: 600px) { main { padding-top: 20px; }.workspace { grid-template-columns: 1fr; }.controls { grid-column: auto; }.row { grid-template-columns: 1fr; }.sparkle { display: none; } }
</style>
