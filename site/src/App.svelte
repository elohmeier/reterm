<script lang="ts">
  import { onDestroy, onMount } from 'svelte';
  import '@fontsource/archivo/400.css';
  import '@fontsource/archivo/600.css';
  import '@fontsource/archivo-black';
  import '@fontsource/caveat/700.css';
  import '@fontsource/special-elite';
  import { InkStudio, LOOKS, type LookId } from '$lib/editor';
  import { DEVICES, INKINGS, resolveDevice, type DeviceProfile, type InkingId } from '$lib/devices';
  import { stickersFor } from '$lib/stickers';
  // Inlined so the worker also starts when the bundle is served cross-origin
  // from GitHub Pages into the device-hosted page.
  import DitherWorker from '$lib/dither.worker?worker&inline';
  import {
    describeError,
    readSession,
    startHeartbeat,
    uploadPacked,
    type Session
  } from '$lib/upload';

  const CANVAS_FONTS = [
    { family: 'Archivo Black', label: 'Poster' },
    { family: 'Caveat', label: 'Marker' },
    { family: 'Special Elite', label: 'Typed' }
  ];

  // The target frame: from the QR's model hint (query or hash), E1004 by
  // default. /api/status confirms it once a session connects.
  const profile = resolveDevice();
  const STICKERS = stickersFor(profile);

  let stageEl: HTMLCanvasElement;
  let proofEl: HTMLCanvasElement;
  let mountEl: HTMLDivElement;
  let studio = $state<InkStudio | null>(null);

  let session = $state<Session | null>(null);
  let statusText = $state('');
  let statusTone = $state<'idle' | 'ok' | 'busy' | 'error'>('idle');
  let busy = $state(false);
  let sending = $state(false);
  let view = $state<'photo' | 'ink'>('photo');
  let tool = $state<'move' | 'text' | 'stickers' | 'draw'>('move');
  let ink = $state<string>(profile.pigments[0].hex);
  let brushWidth = $state(16);
  let inking = $state<InkingId>('floyd');
  let look = $state<LookId>('none');
  let brightness = $state(0);
  let contrast = $state(0);
  let saturation = $state(0);
  let hasPhoto = $state(false);
  let hasContent = $state(false);
  let selected = $state({ count: 0, recolorable: false });
  let proofState = $state<'stale' | 'cooking' | 'fresh'>('stale');
  let inkProgress = $state(0);

  let stopHeartbeat: (() => void) | null = null;
  let proofTimer: ReturnType<typeof setTimeout> | undefined;
  let worker: Worker | null = null;
  let finishProof: ((packed: Uint8Array | null) => void) | null = null;
  let proofPacked: Uint8Array | null = null;

  function setStatus(tone: typeof statusTone, text: string) {
    statusTone = tone;
    statusText = text;
  }

  /** Reload with a model hint; the profile shapes the canvas, so it is fixed per page load. */
  function switchModel(device: DeviceProfile) {
    if (device.id === profile.id) return;
    const url = new URL(location.href);
    url.searchParams.set('model', device.id.replace('reterminal-', ''));
    url.hash = '';
    location.href = url.toString();
  }

  function adoptSession() {
    const next = readSession();
    if (!next) return;
    stopHeartbeat?.();
    session = next;
    setStatus('ok', 'Frame connected — take your time, the session stays open while you edit.');
    stopHeartbeat = startHeartbeat(
      next,
      () => {
        session = null;
        setStatus('error', 'The session ended. Press any button on the frame for a fresh QR.');
      },
      (status) => {
        // The frame's own status is authoritative; a mismatched editor would
        // dither for the wrong panel and be rejected on upload anyway.
        if (status.model && status.model !== profile.id) {
          stopHeartbeat?.();
          stopHeartbeat = null;
          session = null;
          setStatus(
            'error',
            `This frame is a ${status.model}, but the editor is set up for the ${profile.id}. Rescan the QR on the frame.`
          );
        }
      }
    );
  }

  onMount(() => {
    adoptSession();
    if (!session) {
      setStatus(
        'idle',
        `No frame linked — press a button on your ${profile.short} and scan its QR. Designing works without one.`
      );
    }

    const instance = new InkStudio(stageEl, profile, markDirty, (state) => (selected = state));
    instance.mountProofLayer(proofEl);
    proofEl.classList.add('proof');
    studio = instance;
    const observer = new ResizeObserver(() => instance.resize(mountEl.clientWidth));
    observer.observe(mountEl);
    instance.resize(mountEl.clientWidth);
    for (const font of CANVAS_FONTS) void document.fonts.load(`400 32px "${font.family}"`);

    return () => {
      observer.disconnect();
      stopHeartbeat?.();
      clearTimeout(proofTimer);
      worker?.terminate();
      instance.dispose();
    };
  });

  onDestroy(() => clearTimeout(proofTimer));

  $effect(() => {
    studio?.setBrush(ink, brushWidth);
  });
  $effect(() => {
    studio?.setDrawMode(tool === 'draw');
  });
  $effect(() => {
    studio?.setAdjust({ brightness, contrast, saturation });
  });

  function markDirty() {
    proofState = 'stale';
    proofPacked = null;
    hasPhoto = studio?.hasPhoto() ?? false;
    hasContent = !(studio?.isEmpty() ?? true);
    scheduleProof();
  }

  function scheduleProof(delay = 300) {
    clearTimeout(proofTimer);
    if (view !== 'ink') return;
    if (!studio || studio.isEmpty()) {
      proofEl?.getContext('2d')?.clearRect(0, 0, profile.width, profile.height);
      return;
    }
    proofTimer = setTimeout(() => void runProof(), delay);
  }

  function runProof(): Promise<Uint8Array | null> {
    finishProof?.(null);
    finishProof = null;
    worker?.terminate();
    if (!studio || studio.isEmpty()) return Promise.resolve(null);
    proofState = 'cooking';
    inkProgress = 0;
    const image = studio.exportFrame();
    const job = new DitherWorker();
    worker = job;
    return new Promise((resolve) => {
      const finish = (packed: Uint8Array | null) => {
        if (finishProof === finish) finishProof = null;
        if (worker === job) worker = null;
        job.terminate();
        resolve(packed);
      };
      finishProof = finish;
      job.onmessage = (event) => {
        if (typeof event.data.progress === 'number') {
          inkProgress = event.data.progress;
          return;
        }
        const preview = new Uint8ClampedArray(event.data.preview);
        proofEl.getContext('2d')!.putImageData(new ImageData(preview, profile.width, profile.height), 0, 0);
        proofPacked = new Uint8Array(event.data.packed);
        proofState = 'fresh';
        finish(proofPacked);
      };
      job.onerror = () => {
        proofState = 'stale';
        finish(null);
      };
      job.postMessage(
        {
          rgba: image.data.buffer,
          width: profile.width,
          height: profile.height,
          inking,
          palette: profile.pigments.map((pigment) => pigment.rgb),
          packing: profile.packing,
          wantPacked: true
        },
        [image.data.buffer]
      );
    });
  }

  function setView(next: 'photo' | 'ink') {
    view = next;
    if (next === 'ink' && proofState === 'stale') scheduleProof(0);
  }

  function chooseInking(id: InkingId) {
    inking = id;
    proofState = 'stale';
    proofPacked = null;
    setView('ink');
  }

  function chooseLook(id: LookId) {
    look = id;
    studio?.setLook(id);
  }

  function chooseInk(hex: string) {
    ink = hex;
    if (selected.recolorable) studio?.recolorSelection(hex);
  }

  async function addText(family: string) {
    await document.fonts.load(`400 32px "${family}"`);
    studio?.addText(family, ink);
  }

  async function addDateStamp() {
    await document.fonts.load('400 32px "Special Elite"');
    const stamp = new Date()
      .toLocaleDateString('en-GB', { day: '2-digit', month: 'short', year: 'numeric' })
      .toUpperCase();
    studio?.addText('Special Elite', ink, stamp);
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

  async function choose(event: Event) {
    const input = event.currentTarget as HTMLInputElement;
    const selectedFile = input.files?.[0];
    input.value = '';
    if (!selectedFile || !studio) return;
    busy = true;
    setStatus('busy', 'Developing your photo…');
    try {
      const isHeic =
        /hei[cf]/i.test(selectedFile.type) || /\.hei[cf]$/i.test(selectedFile.name);
      let decoded: ImageBitmap | HTMLImageElement;
      try {
        // Safari can decode HEIC natively even when createImageBitmap cannot.
        decoded = await decodeBlob(selectedFile);
      } catch (nativeError) {
        if (!isHeic) throw nativeError;
        try {
          // Loaded on demand: only non-Safari browsers fed a HEIC need it.
          const { default: heic2any } = await import('heic2any');
          const converted = await heic2any({ blob: selectedFile, toType: 'image/jpeg', quality: 0.95 });
          const jpeg = Array.isArray(converted) ? converted[0] : converted;
          if (!(jpeg instanceof Blob)) throw new Error('converter returned no image');
          decoded = await decodeBlob(jpeg);
        } catch (conversionError) {
          throw new Error(
            `native HEIC support failed (${describeError(nativeError)}); ` +
              `HEIC conversion failed (${describeError(conversionError)})`
          );
        }
      }
      studio.setPhoto(decoded);
      tool = 'move';
      view = 'photo';
      setStatus(
        session ? 'ok' : 'idle',
        'Photo on the bench. Drag to place it, then pick an inking.'
      );
    } catch (error) {
      setStatus('error', `Could not decode this photo: ${describeError(error)}`);
    } finally {
      busy = false;
    }
  }

  async function send() {
    if (!session || !studio || studio.isEmpty()) return;
    sending = true;
    busy = true;
    studio.deselect();
    setView('ink');
    // setView just scheduled a deferred proof; cancel it so it cannot cancel
    // the proof this send is about to run itself.
    clearTimeout(proofTimer);
    try {
      setStatus('busy', 'Pressing the inks…');
      const packed = proofPacked ?? (await runProof());
      if (!packed) throw new Error('the design changed mid-press — tap send again');
      await uploadPacked(session, packed, (message) => setStatus('busy', message));
      stopHeartbeat?.();
      stopHeartbeat = null;
      session = null;
      history.replaceState(null, '', location.pathname);
      setStatus('ok', 'Sent! The frame takes about 30 seconds to develop.');
    } catch (error) {
      setStatus(
        'error',
        `Upload failed: ${describeError(error)}. Keep this phone on the same non-guest Wi-Fi as the frame, then try again.`
      );
    } finally {
      sending = false;
      busy = false;
    }
  }

  function onKeydown(event: KeyboardEvent) {
    if (event.key !== 'Delete' && event.key !== 'Backspace') return;
    const target = event.target as HTMLElement | null;
    if (target && (target.tagName === 'INPUT' || target.tagName === 'TEXTAREA')) return;
    if (selected.count === 0) return;
    event.preventDefault();
    studio?.deleteSelection();
  }
</script>

<svelte:head>
  <title>Photo Magic · reTerminal {profile.short}</title>
</svelte:head>

<svelte:window onkeydown={onKeydown} onhashchange={adoptSession} />

<main>
  <header class="masthead">
    <div>
      <p class="eyebrow">reTerminal {profile.short} · {profile.tagline}</p>
      <h1>Photo Magic</h1>
    </div>
    <ul class="inkstrip" aria-hidden="true">
      {#each profile.pigments as pigment (pigment.hex)}
        <li style:background={pigment.hex}></li>
      {/each}
    </ul>
  </header>

  <p class="ticket" data-tone={statusTone} role="status">
    {statusText}{#if proofState === 'cooking' && sending}&nbsp;{inkProgress}%{/if}
  </p>

  {#if !session}
    <div class="framebar">
      <span class="framebar-label">Designing for</span>
      {#each Object.values(DEVICES) as device (device.id)}
        <button
          class="chip"
          class:active={device.id === profile.id}
          aria-pressed={device.id === profile.id}
          onclick={() => switchModel(device)}
        >
          {device.short}
        </button>
      {/each}
      <a class="flashlink" href="https://elohmeier.github.io/reterm/flash.html">
        Flash a frame&nbsp;&rarr;
      </a>
    </div>
  {/if}

  <div class="studio">
    <section class="bench">
      <div class="frame" class:inked={view === 'ink'}>
        <div
          class="panel-mount"
          style:aspect-ratio={`${profile.width} / ${profile.height}`}
          bind:this={mountEl}
        >
          <canvas bind:this={stageEl}></canvas>
          <canvas bind:this={proofEl} width={profile.width} height={profile.height}></canvas>
          {#if !hasContent}
            <div class="hint">
              <p>Add a photo —<br />or start with stickers, ink, and a note.</p>
            </div>
          {/if}
        </div>
      </div>
      <div class="caption">
        <div class="viewflip" role="group" aria-label="Preview mode">
          <button class:active={view === 'photo'} onclick={() => setView('photo')}>Photo</button>
          <button class:active={view === 'ink'} onclick={() => setView('ink')}>
            Ink proof{#if proofState === 'cooking'}<span class="cooking" aria-hidden="true">…</span>{/if}
          </button>
        </div>
        <span class="specs">{profile.width} × {profile.height} · {profile.pigments.length} inks</span>
      </div>
      {#if hasPhoto}
        <div class="caption">
          <div class="fitrow">
            <button class="chip" onclick={() => studio?.fitPhoto('cover')}>Fill frame</button>
            <button class="chip" onclick={() => studio?.fitPhoto('contain')}>Fit inside</button>
          </div>
        </div>
      {/if}
    </section>

    <section class="desk">
      <div class="rail" role="toolbar" aria-label="Tools">
        <label class="tool" class:busy>
          <svg viewBox="0 0 24 24"><rect x="3" y="5" width="18" height="14" rx="2" /><circle
              cx="9"
              cy="10"
              r="1.7"
            /><path d="M5 17l4.5-5 3.5 4 2.5-3 3.5 4" /></svg>
          <span>{hasPhoto ? 'Replace' : 'Photo'}</span>
          <input type="file" accept="image/*,.heic,.heif" onchange={choose} disabled={busy} />
        </label>
        <button
          class="tool"
          class:active={tool === 'text'}
          aria-pressed={tool === 'text'}
          onclick={() => (tool = tool === 'text' ? 'move' : 'text')}
        >
          <svg viewBox="0 0 24 24"><path d="M5 7V4h14v3M12 4v16M9 20h6" /></svg>
          <span>Text</span>
        </button>
        <button
          class="tool"
          class:active={tool === 'stickers'}
          aria-pressed={tool === 'stickers'}
          onclick={() => (tool = tool === 'stickers' ? 'move' : 'stickers')}
        >
          <svg viewBox="-58 -58 116 116"
            ><path
              d="M 0 -50 C 6 -18 18 -6 50 0 C 18 6 6 18 0 50 C -6 18 -18 6 -50 0 C -18 -6 -6 -18 0 -50 Z"
              fill="currentColor"
              stroke="none"
            /></svg>
          <span>Stickers</span>
        </button>
        <button
          class="tool"
          class:active={tool === 'draw'}
          aria-pressed={tool === 'draw'}
          onclick={() => (tool = tool === 'draw' ? 'move' : 'draw')}
        >
          <svg viewBox="0 0 24 24"><path d="M4 20l1-4L16 5l3 3-11 11-4 1z" /><path d="M14 7l3 3" /></svg>
          <span>Draw</span>
        </button>
        <button
          class="tool danger"
          disabled={selected.count === 0}
          onclick={() => studio?.deleteSelection()}
        >
          <svg viewBox="0 0 24 24"><path d="M4 7h16M9 7V4h6v3M7 7l1 13h8l1-13" /></svg>
          <span>Delete</span>
        </button>
      </div>

      {#if tool === 'text'}
        <div class="tray" aria-label="Add text">
          {#each CANVAS_FONTS as font (font.family)}
            <button class="chip fontchip" style:font-family={font.family} onclick={() => addText(font.family)}>
              {font.label}
            </button>
          {/each}
          <button class="chip fontchip" style:font-family="Special Elite" onclick={addDateStamp}>
            Date stamp
          </button>
        </div>
      {/if}

      {#if tool === 'stickers'}
        <div class="tray" aria-label="Stickers">
          {#each STICKERS as sticker (sticker.id)}
            <button class="stickerchip" title={sticker.label} onclick={() => studio?.addSticker(sticker)}>
              <svg viewBox="-58 -58 116 116">
                <path
                  d={sticker.path}
                  fill={sticker.fill}
                  fill-opacity={sticker.opacity ?? 1}
                  stroke={sticker.stroke ?? 'none'}
                  stroke-width={sticker.strokeWidth ?? 0}
                />
              </svg>
            </button>
          {/each}
        </div>
      {/if}

      {#if tool === 'draw'}
        <div class="tray brushtray">
          <label class="slider">
            Brush size
            <input type="range" min="6" max="60" step="2" bind:value={brushWidth} />
          </label>
        </div>
      {/if}

      <div class="wells">
        <span class="wells-label">{selected.recolorable ? 'Repaint with' : 'Ink'}</span>
        {#each profile.pigments as pigment (pigment.hex)}
          <button
            class="well"
            class:current={ink === pigment.hex}
            style:background={pigment.hex}
            aria-label={`${pigment.name} ink`}
            aria-pressed={ink === pigment.hex}
            onclick={() => chooseInk(pigment.hex)}
          ></button>
        {/each}
      </div>

      <fieldset class="panel">
        <legend>Inking</legend>
        <div class="chips">
          {#each INKINGS as style (style.id)}
            <button
              class="chip inkingchip"
              class:active={inking === style.id}
              disabled={!hasContent}
              onclick={() => chooseInking(style.id)}
            >
              <strong>{style.label}</strong>
              <small>{style.detail}</small>
            </button>
          {/each}
        </div>
      </fieldset>

      <fieldset class="panel" disabled={!hasPhoto}>
        <legend>Photo look</legend>
        <div class="chips">
          {#each LOOKS as entry (entry.id)}
            <button class="chip" class:active={look === entry.id} onclick={() => chooseLook(entry.id)}>
              {entry.label}
            </button>
          {/each}
        </div>
        <label class="slider">
          Brightness
          <input type="range" min="-0.35" max="0.35" step="0.01" bind:value={brightness} />
        </label>
        <label class="slider">
          Contrast
          <input type="range" min="-0.35" max="0.35" step="0.01" bind:value={contrast} />
        </label>
        {#if profile.color}
          <label class="slider">
            Color
            <input type="range" min="-0.9" max="0.9" step="0.01" bind:value={saturation} />
          </label>
        {/if}
      </fieldset>

      <div class="sendbar">
        <button class="send" disabled={!session || !hasContent || busy} onclick={send}>
          {sending ? 'Sending…' : 'Send to frame'}
        </button>
        <p class="sendnote">
          {#if session}
            {Math.round(profile.packedBytes / 1000)} KB over your Wi-Fi · {profile.refreshNote}.
          {:else}
            Press a button on the frame and scan its QR to go live.
          {/if}
        </p>
      </div>
    </section>
  </div>
</main>

<style>
  :global(*) {
    box-sizing: border-box;
  }
  :global(:root) {
    --paper: #ece9e0;
    --card: #ffffff;
    --ink: #131313;
    --muted: #6e6a5e;
    --red: #d21e28;
    --blue: #004bbe;
    --green: #009146;
    --yellow: #f5cd1e;
    --line: 2px solid var(--ink);
  }
  :global(html) {
    scroll-padding-bottom: 120px;
  }
  :global(body) {
    margin: 0;
    color: var(--ink);
    background:
      radial-gradient(rgba(19, 19, 19, 0.06) 1px, transparent 1.2px) 0 0 / 14px 14px,
      var(--paper);
    font-family: Archivo, 'Avenir Next', system-ui, sans-serif;
  }
  :global(button),
  :global(input),
  :global(select) {
    font: inherit;
    color: inherit;
  }
  :global(:focus-visible) {
    outline: 3px solid var(--blue);
    outline-offset: 2px;
  }
  :global(.proof) {
    image-rendering: pixelated;
    opacity: 0;
    transition: opacity 0.2s ease;
  }
  .frame.inked :global(.proof) {
    opacity: 1;
  }

  main {
    width: min(1080px, 100% - 28px);
    margin: 0 auto;
    padding: 22px 0 48px;
  }

  .masthead {
    display: flex;
    align-items: flex-end;
    justify-content: space-between;
    gap: 16px;
    flex-wrap: wrap;
  }
  .eyebrow {
    margin: 0 0 7px;
    font-family: 'Special Elite', monospace;
    font-size: 0.82rem;
    letter-spacing: 0.06em;
    color: var(--muted);
  }
  h1 {
    margin: 0;
    font-family: 'Archivo Black', Archivo, sans-serif;
    font-size: clamp(2.2rem, 6vw, 3.6rem);
    line-height: 0.95;
    letter-spacing: -0.02em;
    text-shadow:
      2px 2px 0 rgba(210, 30, 40, 0.55),
      -2px -2px 0 rgba(0, 75, 190, 0.45);
  }
  .inkstrip {
    display: flex;
    gap: 6px;
    margin: 0 0 10px;
    padding: 0;
    list-style: none;
  }
  .inkstrip li {
    width: 15px;
    height: 15px;
    border: 2px solid var(--ink);
    border-radius: 50%;
  }

  .ticket {
    margin: 16px 0 20px;
    padding: 11px 18px;
    border: var(--line);
    border-radius: 999px;
    background: var(--card);
    font-weight: 600;
    font-size: 0.95rem;
  }
  .ticket[data-tone='ok'] {
    border-color: var(--green);
    box-shadow: 3px 3px 0 var(--green);
  }
  .ticket[data-tone='error'] {
    border-color: var(--red);
    box-shadow: 3px 3px 0 var(--red);
  }
  .ticket[data-tone='busy'] {
    border-color: var(--blue);
    box-shadow: 3px 3px 0 var(--blue);
  }

  .framebar {
    display: flex;
    align-items: center;
    gap: 8px;
    flex-wrap: wrap;
    margin: -8px 0 20px;
    padding: 0 6px;
  }
  .framebar-label {
    font-family: 'Special Elite', monospace;
    font-size: 0.8rem;
    color: var(--muted);
  }
  .flashlink {
    margin-left: auto;
    font-weight: 600;
    font-size: 0.85rem;
    color: var(--blue);
  }

  .studio {
    display: grid;
    gap: 24px;
  }
  @media (min-width: 880px) {
    .studio {
      grid-template-columns: minmax(360px, 1fr) 400px;
      align-items: start;
    }
    .bench {
      position: sticky;
      top: 16px;
    }
  }

  .bench,
  .desk {
    min-width: 0;
  }
  .bench {
    max-width: 480px;
    width: 100%;
    margin: 0 auto;
  }
  .panel-mount :global(.canvas-container) {
    max-width: 100%;
  }
  .frame {
    position: relative;
    padding: 10px;
    border: var(--line);
    border-radius: 18px;
    background: var(--card);
    box-shadow: 7px 7px 0 var(--ink);
  }
  .panel-mount {
    position: relative;
    border-radius: 8px;
    overflow: hidden;
    outline: 1.5px solid #d9d5c9;
  }
  .hint {
    position: absolute;
    inset: 0;
    display: grid;
    place-items: center;
    pointer-events: none;
  }
  .hint p {
    margin: 16px;
    padding: 14px 18px;
    border: 2px dashed #b9b4a5;
    border-radius: 12px;
    color: var(--muted);
    font-weight: 600;
    text-align: center;
  }

  .caption {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 10px;
    margin-top: 12px;
  }
  .viewflip {
    display: inline-flex;
    border: var(--line);
    border-radius: 999px;
    background: var(--card);
    overflow: hidden;
  }
  .viewflip button {
    padding: 7px 14px;
    border: none;
    background: none;
    font-weight: 700;
    font-size: 0.88rem;
    cursor: pointer;
  }
  .viewflip button.active {
    background: var(--ink);
    color: var(--card);
  }
  .cooking {
    display: inline-block;
    width: 1em;
    text-align: left;
  }
  .specs {
    font-family: 'Special Elite', monospace;
    font-size: 0.78rem;
    color: var(--muted);
  }
  .fitrow {
    display: flex;
    gap: 8px;
  }

  .desk {
    display: grid;
    gap: 14px;
    align-content: start;
  }
  .rail {
    display: flex;
    gap: 8px;
    flex-wrap: wrap;
  }
  .tool {
    display: grid;
    justify-items: center;
    gap: 3px;
    min-width: 64px;
    padding: 9px 10px 7px;
    border: var(--line);
    border-radius: 13px;
    background: var(--card);
    cursor: pointer;
    font-size: 0.68rem;
    font-weight: 700;
    text-transform: uppercase;
    letter-spacing: 0.05em;
  }
  .tool svg {
    width: 21px;
    height: 21px;
    fill: none;
    stroke: currentColor;
    stroke-width: 2;
    stroke-linecap: round;
    stroke-linejoin: round;
  }
  .tool.active {
    background: var(--yellow);
    box-shadow: 3px 3px 0 var(--ink);
  }
  .tool:disabled,
  .tool.busy {
    opacity: 0.4;
    cursor: not-allowed;
  }
  .tool.danger {
    color: var(--red);
  }
  .tool input {
    display: none;
  }

  .tray {
    display: flex;
    gap: 8px;
    overflow-x: auto;
    padding: 2px;
  }
  .stickerchip {
    flex: 0 0 auto;
    width: 54px;
    height: 54px;
    padding: 5px;
    border: var(--line);
    border-radius: 13px;
    background: var(--card);
    cursor: pointer;
  }
  .stickerchip svg {
    width: 100%;
    height: 100%;
  }
  .fontchip {
    font-size: 1.02rem;
  }
  .brushtray {
    padding: 4px 2px;
  }

  .wells {
    display: flex;
    align-items: center;
    gap: 9px;
  }
  .wells-label {
    font-family: 'Special Elite', monospace;
    font-size: 0.8rem;
    color: var(--muted);
    margin-right: 2px;
  }
  .well {
    width: 30px;
    height: 30px;
    border: 2px solid var(--ink);
    border-radius: 50%;
    cursor: pointer;
    padding: 0;
  }
  .well.current {
    outline: 3px solid var(--ink);
    outline-offset: 2px;
  }

  .panel {
    margin: 0;
    padding: 12px 14px 14px;
    border: var(--line);
    border-radius: 15px;
    background: var(--card);
    display: grid;
    gap: 11px;
  }
  .panel[disabled] {
    opacity: 0.55;
  }
  legend {
    padding: 0 6px;
    font-family: 'Special Elite', monospace;
    font-size: 0.84rem;
  }
  .chips {
    display: flex;
    flex-wrap: wrap;
    gap: 7px;
  }
  .chip {
    padding: 7px 12px;
    border: var(--line);
    border-radius: 999px;
    background: var(--card);
    font-weight: 600;
    font-size: 0.85rem;
    cursor: pointer;
  }
  .chip.active {
    background: var(--ink);
    color: var(--card);
  }
  .chip:disabled {
    opacity: 0.4;
    cursor: not-allowed;
  }
  .inkingchip {
    display: grid;
    gap: 1px;
    justify-items: start;
    border-radius: 12px;
    text-align: left;
  }
  .inkingchip small {
    font-weight: 400;
    font-size: 0.68rem;
    opacity: 0.75;
  }
  .slider {
    display: grid;
    gap: 4px;
    font-weight: 600;
    font-size: 0.85rem;
  }
  .slider input {
    width: 100%;
    accent-color: var(--blue);
  }

  .sendbar {
    position: sticky;
    bottom: 10px;
    display: grid;
    gap: 5px;
    padding: 8px;
    border: var(--line);
    border-radius: 16px;
    background: var(--card);
    box-shadow: 0 -6px 18px rgba(19, 19, 19, 0.08);
  }
  .send {
    padding: 14px;
    border: var(--line);
    border-radius: 12px;
    background: var(--red);
    color: #fff;
    font-family: 'Archivo Black', Archivo, sans-serif;
    font-size: 1.02rem;
    letter-spacing: 0.02em;
    cursor: pointer;
    box-shadow: 4px 4px 0 var(--ink);
  }
  .send:disabled {
    opacity: 0.45;
    cursor: not-allowed;
    box-shadow: none;
  }
  .send:not(:disabled):active {
    translate: 2px 2px;
    box-shadow: 2px 2px 0 var(--ink);
  }
  .sendnote {
    margin: 0;
    text-align: center;
    font-size: 0.78rem;
    color: var(--muted);
  }

  @media (prefers-reduced-motion: reduce) {
    :global(.proof) {
      transition: none;
    }
    .send:not(:disabled):active {
      translate: none;
    }
  }
</style>
