// Fabric.js editing surface for the target frame. All objects live in panel
// coordinates; the viewport zoom only fits them on screen, so exporting at
// panel size needs no reprojection.
import {
  Canvas,
  FabricImage,
  IText,
  InteractiveFabricObject,
  Path,
  PencilBrush,
  Rect,
  StaticCanvas,
  filters,
  type FabricObject
} from 'fabric';
import type { StickerDef } from './stickers';

export type LookId =
  | 'none'
  | 'punch'
  | 'kodak'
  | 'polaroid'
  | 'vintage'
  | 'mono'
  | 'redfilter'
  | 'orangefilter'
  | 'yellowfilter'
  | 'greenfilter'
  | 'trix';
export type Adjust = {
  brightness: number;
  contrast: number;
  saturation: number;
  sharpen: number;
  grain: number;
};
export type Selection = { count: number; recolorable: boolean };
export type ToneKind = 'lighten' | 'darken' | 'zone';
export type DrawMode = 'ink' | ToneKind | null;
export type FrameId = 'none' | 'polaroid' | 'stamp' | 'news';
export type ExportedFrame = {
  image: ImageData;
  tone: Uint8Array | null;
  zone: Uint8Array | null;
};

export const FRAMES: { id: FrameId; label: string }[] = [
  { id: 'none', label: 'None' },
  { id: 'polaroid', label: 'Polaroid' },
  { id: 'stamp', label: 'Stamp' },
  { id: 'news', label: 'Front page' }
];

/**
 * Photo looks per panel type. Monochrome frames trade the color-film looks
 * for classic B/W photography: virtual lens filters (a red filter darkens
 * skies), and a pushed Tri-X newsroom look.
 */
export function looksFor(color: boolean): { id: LookId; label: string }[] {
  if (color) {
    return [
      { id: 'none', label: 'As shot' },
      { id: 'punch', label: 'Punch' },
      { id: 'kodak', label: 'Kodak' },
      { id: 'polaroid', label: 'Polaroid' },
      { id: 'vintage', label: 'Vintage' },
      { id: 'mono', label: 'Ink & paper' }
    ];
  }
  return [
    { id: 'none', label: 'As shot' },
    { id: 'punch', label: 'Punch' },
    { id: 'mono', label: 'Ink & paper' },
    { id: 'redfilter', label: 'Red filter' },
    { id: 'orangefilter', label: 'Orange filter' },
    { id: 'yellowfilter', label: 'Yellow filter' },
    { id: 'greenfilter', label: 'Green filter' },
    { id: 'trix', label: 'Tri-X' }
  ];
}

// Grayscale channel mixes for the virtual lens filters, as R/G/B luminance
// weights. A red filter passes red light, so red things render bright and
// blue skies render dark — the classic B/W landscape trick.
const GRAY_MIXES: Partial<Record<LookId, [number, number, number]>> = {
  redfilter: [0.85, 0.15, 0.0],
  orangefilter: [0.7, 0.3, 0.0],
  yellowfilter: [0.5, 0.45, 0.05],
  greenfilter: [0.2, 0.7, 0.1],
  trix: [0.3, 0.59, 0.11]
};

// Selection handles styled like print registration marks.
InteractiveFabricObject.ownDefaults = {
  ...InteractiveFabricObject.ownDefaults,
  transparentCorners: false,
  cornerStyle: 'circle',
  cornerColor: '#ffffff',
  cornerStrokeColor: '#111111',
  cornerSize: 11,
  touchCornerSize: 26,
  borderColor: '#111111',
  borderScaleFactor: 1.6,
  borderOpacityWhenMoving: 0.5,
  padding: 2
};

type PhotoFilters = NonNullable<FabricImage['filters']>;

function toneKindOf(object: FabricObject): ToneKind | null {
  return (object.get('toneKind') as ToneKind | undefined) ?? null;
}

export class InkStudio {
  readonly canvas: Canvas;
  readonly width: number;
  readonly height: number;
  private photo: FabricImage | null = null;
  private look: LookId = 'none';
  private adjust: Adjust = { brightness: 0, contrast: 0, saturation: 0, sharpen: 0, grain: 0 };
  private brush: PencilBrush;
  private inkColor = '#111111';
  private inkWidth = 14;
  private drawMode: DrawMode = null;
  private frameObjects: FabricObject[] = [];

  constructor(
    element: HTMLCanvasElement,
    size: { width: number; height: number },
    private changed: () => void,
    selection: (state: Selection) => void
  ) {
    this.width = size.width;
    this.height = size.height;
    this.canvas = new Canvas(element, {
      width: size.width,
      height: size.height,
      backgroundColor: '#ffffff',
      preserveObjectStacking: true,
      selection: false
    });
    this.brush = new PencilBrush(this.canvas);
    this.canvas.freeDrawingBrush = this.brush;
    this.applyBrush();

    const notify = () => this.changed();
    this.canvas.on('object:added', notify);
    this.canvas.on('object:modified', notify);
    this.canvas.on('text:changed', notify);
    this.canvas.on('object:removed', (event) => {
      if (event.target === this.photo) this.photo = null;
      notify();
    });
    this.canvas.on('path:created', (event) => {
      const mode = this.drawMode;
      if (mode && mode !== 'ink') {
        (event as unknown as { path: Path }).path.set('toneKind', mode);
      }
    });
    const report = () => {
      const objects = this.canvas.getActiveObjects();
      selection({
        count: objects.length,
        recolorable: objects.some(
          (object) => object !== this.photo && toneKindOf(object) === null
        )
      });
    };
    this.canvas.on('selection:created', report);
    this.canvas.on('selection:updated', report);
    this.canvas.on('selection:cleared', report);
  }

  /** Attach the ink-proof canvas between objects and controls, so handles stay visible. */
  mountProofLayer(layer: HTMLCanvasElement) {
    layer.style.position = 'absolute';
    layer.style.inset = '0';
    layer.style.width = '100%';
    layer.style.height = '100%';
    layer.style.pointerEvents = 'none';
    this.canvas.wrapperEl.insertBefore(layer, this.canvas.upperCanvasEl);
  }

  resize(displayWidth: number) {
    const width = Math.max(120, displayWidth);
    this.canvas.setDimensions({ width, height: (width * this.height) / this.width });
    this.canvas.setZoom(width / this.width);
    this.canvas.requestRenderAll();
  }

  hasPhoto(): boolean {
    return this.photo !== null;
  }

  isEmpty(): boolean {
    return this.canvas.getObjects().length === 0;
  }

  hasZoneStrokes(): boolean {
    return this.canvas.getObjects().some((object) => toneKindOf(object) === 'zone');
  }

  setPhoto(source: ImageBitmap | HTMLImageElement | HTMLCanvasElement) {
    // Cap the working resolution: plenty for either panel and safely below
    // the WebGL filter texture limit.
    const maxSide = 3200;
    const scale = Math.min(1, maxSide / Math.max(source.width, source.height));
    const width = Math.round(source.width * scale);
    const height = Math.round(source.height * scale);
    const buffer = document.createElement('canvas');
    buffer.width = width;
    buffer.height = height;
    buffer.getContext('2d')!.drawImage(source, 0, 0, width, height);
    if (source instanceof ImageBitmap) source.close();

    if (this.photo) this.canvas.remove(this.photo);
    const photo = new FabricImage(buffer, {
      left: this.width / 2,
      top: this.height / 2
    });
    const target = this.width / this.height;
    const normalError = Math.abs(Math.log(width / height / target));
    const rotatedError = Math.abs(Math.log(height / width / target));
    if (rotatedError < normalError) photo.angle = 90;
    this.photo = photo;
    this.canvas.add(photo);
    this.canvas.sendObjectToBack(photo);
    this.fitPhoto('cover');
    this.applyPhotoFilters();
    this.canvas.setActiveObject(photo);
  }

  /** Rescale and recenter the photo over the panel, honoring its rotation. */
  fitPhoto(mode: 'cover' | 'contain') {
    const photo = this.photo;
    if (!photo) return;
    const radians = (photo.angle * Math.PI) / 180;
    const spanX =
      Math.abs(photo.width * Math.cos(radians)) + Math.abs(photo.height * Math.sin(radians));
    const spanY =
      Math.abs(photo.width * Math.sin(radians)) + Math.abs(photo.height * Math.cos(radians));
    const factor =
      mode === 'cover'
        ? Math.max(this.width / spanX, this.height / spanY)
        : Math.min(this.width / spanX, this.height / spanY);
    photo.set({ scaleX: factor, scaleY: factor, left: this.width / 2, top: this.height / 2 });
    photo.setCoords();
    this.canvas.requestRenderAll();
    this.changed();
  }

  setLook(look: LookId) {
    this.look = look;
    this.applyPhotoFilters();
  }

  setAdjust(adjust: Adjust) {
    this.adjust = adjust;
    this.applyPhotoFilters();
  }

  private applyPhotoFilters() {
    const photo = this.photo;
    if (!photo) return;
    const list: PhotoFilters = [];
    const mix = GRAY_MIXES[this.look];
    if (this.look === 'punch') {
      list.push(new filters.Contrast({ contrast: 0.16 }), new filters.Saturation({ saturation: 0.45 }));
    } else if (this.look === 'kodak') list.push(new filters.Kodachrome());
    else if (this.look === 'polaroid') list.push(new filters.Polaroid());
    else if (this.look === 'vintage') list.push(new filters.Vintage());
    else if (this.look === 'mono') {
      list.push(new filters.Grayscale(), new filters.Contrast({ contrast: 0.12 }));
    } else if (mix) {
      const [r, g, b] = mix;
      // prettier-ignore
      list.push(new filters.ColorMatrix({
        matrix: [
          r, g, b, 0, 0,
          r, g, b, 0, 0,
          r, g, b, 0, 0,
          0, 0, 0, 1, 0
        ]
      }));
      if (this.look === 'trix') {
        list.push(new filters.Contrast({ contrast: 0.22 }), new filters.Noise({ noise: 45 }));
      }
    }
    const { brightness, contrast, saturation, sharpen, grain } = this.adjust;
    if (brightness !== 0) list.push(new filters.Brightness({ brightness }));
    if (contrast !== 0) list.push(new filters.Contrast({ contrast }));
    if (saturation !== 0) list.push(new filters.Saturation({ saturation }));
    if (sharpen > 0) {
      const s = sharpen;
      // prettier-ignore
      list.push(new filters.Convolute({
        matrix: [
          0, -s, 0,
          -s, 1 + 4 * s, -s,
          0, -s, 0
        ]
      }));
    }
    if (grain > 0) list.push(new filters.Noise({ noise: Math.round(grain * 110) }));
    photo.filters = list;
    photo.applyFilters();
    this.canvas.requestRenderAll();
    this.changed();
  }

  addText(fontFamily: string, fill: string, content = 'Hello!', invert = false) {
    const text = new IText(content, {
      left: this.width / 2,
      top: this.height / 2,
      fontFamily,
      // Proportional to the panel: 140px on the 1200×1600 E1004.
      fontSize: Math.round(Math.min(this.width, this.height) * (140 / 1200)),
      fill: invert ? '#ffffff' : fill,
      textAlign: 'center',
      // Knockout text: white through 'difference' inverts whatever is
      // underneath — the only reliable "contrast anywhere" ink on 1-bit.
      globalCompositeOperation: invert ? 'difference' : undefined
    });
    this.canvas.add(text);
    this.canvas.setActiveObject(text);
    text.enterEditing();
    text.selectAll();
    this.canvas.requestRenderAll();
  }

  /** Small film-camera date stamp in the lower-right corner. */
  addDateStamp(fill: string, stroke: string) {
    const min = Math.min(this.width, this.height);
    const stamp = new Date()
      .toLocaleDateString('en-GB', { day: '2-digit', month: 'short', year: 'numeric' })
      .toUpperCase();
    const text = new IText(stamp, {
      fontFamily: 'Special Elite',
      fontSize: Math.round(min * 0.055),
      fill,
      stroke,
      strokeWidth: Math.max(2, Math.round(min * 0.004)),
      paintFirst: 'stroke',
      originX: 'right',
      originY: 'bottom',
      left: this.width - Math.round(min * 0.045),
      top: this.height - Math.round(min * 0.04),
      textAlign: 'right'
    });
    this.canvas.add(text);
    this.canvas.setActiveObject(text);
    this.canvas.requestRenderAll();
  }

  addSticker(definition: StickerDef) {
    const scale = (Math.min(this.width, this.height) * 3.4) / 1200;
    const jitter = Math.min(this.width, this.height) / 7.5;
    const sticker = new Path(definition.path, {
      left: this.width / 2 + (Math.random() - 0.5) * jitter,
      top: this.height / 2 + (Math.random() - 0.5) * jitter,
      angle: (Math.random() - 0.5) * 24,
      fill: definition.fill,
      stroke: definition.stroke ?? null,
      strokeWidth: definition.strokeWidth ?? 0,
      opacity: definition.opacity ?? 1,
      scaleX: scale,
      scaleY: scale
    });
    this.canvas.add(sticker);
    this.canvas.setActiveObject(sticker);
    this.canvas.requestRenderAll();
  }

  /**
   * Decorative frame overlays. They sit directly above the photo so ink,
   * text, and stickers still land on top — like stickers on a real polaroid.
   */
  setFrame(id: FrameId, accent = '#000000') {
    for (const object of this.frameObjects) this.canvas.remove(object);
    this.frameObjects = [];
    if (id !== 'none') {
      this.frameObjects = this.buildFrame(id, accent);
      for (const object of this.frameObjects) this.canvas.add(object);
      for (const object of [...this.frameObjects].reverse()) this.canvas.sendObjectToBack(object);
      if (this.photo) this.canvas.sendObjectToBack(this.photo);
    }
    this.canvas.requestRenderAll();
  }

  private buildFrame(id: FrameId, accent: string): FabricObject[] {
    const w = this.width;
    const h = this.height;
    const min = Math.min(w, h);
    // Fabric v6 defaults origins to center; frame geometry is authored from
    // the top-left corner.
    const still = {
      selectable: false as const,
      evented: false as const,
      originX: 'left' as const,
      originY: 'top' as const
    };

    if (id === 'polaroid') {
      const margin = Math.round(min * 0.055);
      const bottom = Math.round(min * 0.2);
      return [
        new Path(
          `M 0 0 H ${w} V ${h} H 0 Z ` +
            `M ${margin} ${margin} H ${w - margin} V ${h - bottom} H ${margin} Z`,
          { fill: '#ffffff', fillRule: 'evenodd', ...still }
        )
      ];
    }

    if (id === 'stamp') {
      const margin = Math.round(min * 0.05);
      const border = new Path(
        `M 0 0 H ${w} V ${h} H 0 Z ` +
          `M ${margin} ${margin} H ${w - margin} V ${h - margin} H ${margin} Z`,
        { fill: '#ffffff', fillRule: 'evenodd', ...still }
      );
      // Perforation: punched holes straddling the outer edge, reading as the
      // scalloped border of a postage stamp against a dark mat.
      const r = Math.max(6, Math.round(min * 0.02));
      const holes: string[] = [];
      const circle = (cx: number, cy: number) =>
        `M ${cx - r} ${cy} A ${r} ${r} 0 1 0 ${cx + r} ${cy} A ${r} ${r} 0 1 0 ${cx - r} ${cy} Z`;
      const across = Math.max(6, Math.round(w / (r * 3.2)));
      const down = Math.max(6, Math.round(h / (r * 3.2)));
      for (let i = 0; i < across; i += 1) {
        const cx = ((i + 0.5) * w) / across;
        holes.push(circle(cx, 0), circle(cx, h));
      }
      for (let i = 0; i < down; i += 1) {
        const cy = ((i + 0.5) * h) / down;
        holes.push(circle(0, cy), circle(w, cy));
      }
      const perforation = new Path(holes.join(' '), { fill: '#000000', ...still });
      const hairline = new Rect({
        left: Math.round(margin * 1.4),
        top: Math.round(margin * 1.4),
        width: w - Math.round(margin * 2.8),
        height: h - Math.round(margin * 2.8),
        fill: 'transparent',
        stroke: '#000000',
        strokeWidth: Math.max(2, Math.round(min * 0.006)),
        ...still
      });
      return [border, perforation, hairline];
    }

    // Newspaper front page: white masthead band with editable headline text.
    const bandHeight = Math.round(h * (w > h ? 0.26 : 0.2));
    const ruleWeight = Math.max(2, Math.round(min * 0.005));
    const inset = Math.round(w * 0.03);
    const rule = (top: number, weight: number) =>
      new Rect({ left: inset, top, width: w - inset * 2, height: weight, fill: '#000000', ...still });
    const band = new Rect({ left: 0, top: 0, width: w, height: bandHeight, fill: '#ffffff', ...still });
    const masthead = new IText('THE DAILY FRAME', {
      fontFamily: 'Archivo Black',
      fontSize: Math.round(bandHeight * 0.3),
      fill: accent,
      originX: 'center',
      originY: 'top',
      left: w / 2,
      top: Math.round(bandHeight * 0.05),
      textAlign: 'center'
    });
    const date = new Date()
      .toLocaleDateString('en-GB', { weekday: 'long', day: '2-digit', month: 'long', year: 'numeric' })
      .toUpperCase();
    const dateline = new IText(`NO. 1 · ${date} · 50¢`, {
      fontFamily: 'Special Elite',
      fontSize: Math.round(bandHeight * 0.11),
      fill: '#000000',
      originX: 'center',
      originY: 'top',
      left: w / 2,
      top: Math.round(bandHeight * 0.45),
      textAlign: 'center'
    });
    const headline = new IText('PICTURE OF THE DAY', {
      fontFamily: 'Archivo Black',
      fontSize: Math.round(bandHeight * 0.22),
      fill: '#000000',
      originX: 'center',
      originY: 'top',
      left: w / 2,
      top: Math.round(bandHeight * 0.66),
      textAlign: 'center'
    });
    return [
      band,
      rule(Math.round(bandHeight * 0.42), ruleWeight),
      rule(Math.round(bandHeight * 0.62), ruleWeight),
      rule(bandHeight - ruleWeight * 2, ruleWeight * 2),
      masthead,
      dateline,
      headline
    ];
  }

  setDrawMode(mode: DrawMode) {
    this.drawMode = mode;
    if (mode) this.canvas.discardActiveObject();
    this.canvas.isDrawingMode = mode !== null;
    this.applyBrush();
    this.canvas.requestRenderAll();
  }

  setBrush(color: string, width: number) {
    this.inkColor = color;
    this.inkWidth = width;
    this.applyBrush();
  }

  private applyBrush() {
    const mode = this.drawMode;
    if (mode === 'lighten') {
      this.brush.color = 'rgba(255, 255, 255, 0.55)';
      this.brush.width = this.inkWidth * 2.4;
    } else if (mode === 'darken') {
      this.brush.color = 'rgba(0, 0, 0, 0.5)';
      this.brush.width = this.inkWidth * 2.4;
    } else if (mode === 'zone') {
      // Screen-only hint color; zone strokes never print — they select the
      // second inking style in the dither worker.
      this.brush.color = 'rgba(232, 60, 180, 0.4)';
      this.brush.width = this.inkWidth * 2.8;
    } else {
      this.brush.color = this.inkColor;
      this.brush.width = this.inkWidth;
    }
  }

  /** Repaint the selection with a pigment: fills keep fills, strokes keep strokes. */
  recolorSelection(hex: string) {
    const objects = this.canvas.getActiveObjects();
    let touched = false;
    for (const object of objects) {
      if (object === this.photo || toneKindOf(object) !== null) continue;
      if (object.get('fill')) object.set('fill', hex);
      else object.set('stroke', hex);
      touched = true;
    }
    if (touched) {
      this.canvas.requestRenderAll();
      this.changed();
    }
  }

  deleteSelection() {
    const objects = this.canvas.getActiveObjects();
    this.canvas.discardActiveObject();
    for (const object of objects) this.canvas.remove(object);
    this.canvas.requestRenderAll();
  }

  deselect() {
    this.canvas.discardActiveObject();
    this.canvas.requestRenderAll();
  }

  /**
   * Render the composition at exactly panel resolution (controls excluded).
   * Tone and zone strokes are not part of the picture: they are hidden from
   * the image and rendered into separate masks for the dither worker —
   * tone as dodge/burn around neutral 128, zone as a white-on-black stencil.
   */
  async exportFrame(): Promise<ExportedFrame> {
    const tonePaths = this.canvas
      .getObjects()
      .filter((object) => toneKindOf(object) !== null);
    for (const path of tonePaths) path.set('visible', false);
    const snapshot = this.canvas.toCanvasElement(this.width / this.canvas.getWidth());
    for (const path of tonePaths) path.set('visible', true);
    this.canvas.requestRenderAll();

    const out = document.createElement('canvas');
    out.width = this.width;
    out.height = this.height;
    const context = out.getContext('2d', { alpha: false, willReadFrequently: true })!;
    context.fillStyle = '#ffffff';
    context.fillRect(0, 0, this.width, this.height);
    context.drawImage(snapshot, 0, 0, this.width, this.height);
    const image = context.getImageData(0, 0, this.width, this.height);

    const dodgePaths = tonePaths.filter((path) => toneKindOf(path) !== 'zone');
    const zonePaths = tonePaths.filter((path) => toneKindOf(path) === 'zone');
    const tone = dodgePaths.length
      ? await this.renderMask(dodgePaths, '#808080', (clone, original) =>
          clone.set({
            stroke: toneKindOf(original) === 'lighten' ? '#ffffff' : '#000000',
            opacity: 0.55,
            shadow: null
          })
        )
      : null;
    const zone = zonePaths.length
      ? await this.renderMask(zonePaths, '#000000', (clone) =>
          clone.set({ stroke: '#ffffff', opacity: 1, shadow: null })
        )
      : null;
    return { image, tone, zone };
  }

  private async renderMask(
    paths: FabricObject[],
    background: string,
    recolor: (clone: FabricObject, original: FabricObject) => void
  ): Promise<Uint8Array> {
    const scratch = new StaticCanvas(undefined, {
      width: this.width,
      height: this.height,
      backgroundColor: background,
      renderOnAddRemove: false
    });
    for (const path of paths) {
      const clone = await path.clone();
      recolor(clone, path);
      clone.set('visible', true);
      scratch.add(clone);
    }
    const element = scratch.toCanvasElement(1);
    const data = element
      .getContext('2d', { willReadFrequently: true })!
      .getImageData(0, 0, this.width, this.height).data;
    const mask = new Uint8Array(this.width * this.height);
    for (let index = 0; index < mask.length; index += 1) mask[index] = data[index * 4];
    void scratch.dispose();
    return mask;
  }

  dispose() {
    void this.canvas.dispose();
  }
}
