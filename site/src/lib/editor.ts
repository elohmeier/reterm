// Fabric.js editing surface for the 1200×1600 six-pigment frame. All objects
// live in panel coordinates; the viewport zoom only fits them on screen, so
// exporting at panel size needs no reprojection.
import {
  Canvas,
  FabricImage,
  IText,
  InteractiveFabricObject,
  Path,
  PencilBrush,
  filters
} from 'fabric';
import { SCREEN_HEIGHT, SCREEN_WIDTH } from './pigments';
import type { StickerDef } from './stickers';

export type LookId = 'none' | 'punch' | 'kodak' | 'polaroid' | 'vintage' | 'mono';
export type Adjust = { brightness: number; contrast: number; saturation: number };
export type Selection = { count: number; recolorable: boolean };

export const LOOKS: { id: LookId; label: string }[] = [
  { id: 'none', label: 'As shot' },
  { id: 'punch', label: 'Punch' },
  { id: 'kodak', label: 'Kodak' },
  { id: 'polaroid', label: 'Polaroid' },
  { id: 'vintage', label: 'Vintage' },
  { id: 'mono', label: 'Ink & paper' }
];

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

export class InkStudio {
  readonly canvas: Canvas;
  private photo: FabricImage | null = null;
  private look: LookId = 'none';
  private adjust: Adjust = { brightness: 0, contrast: 0, saturation: 0 };
  private brush: PencilBrush;

  constructor(
    element: HTMLCanvasElement,
    private changed: () => void,
    selection: (state: Selection) => void
  ) {
    this.canvas = new Canvas(element, {
      width: SCREEN_WIDTH,
      height: SCREEN_HEIGHT,
      backgroundColor: '#ffffff',
      preserveObjectStacking: true,
      selection: false
    });
    this.brush = new PencilBrush(this.canvas);
    this.brush.color = '#111111';
    this.brush.width = 14;
    this.canvas.freeDrawingBrush = this.brush;

    const notify = () => this.changed();
    this.canvas.on('object:added', notify);
    this.canvas.on('object:modified', notify);
    this.canvas.on('text:changed', notify);
    this.canvas.on('object:removed', (event) => {
      if (event.target === this.photo) this.photo = null;
      notify();
    });
    const report = () => {
      const objects = this.canvas.getActiveObjects();
      selection({
        count: objects.length,
        recolorable: objects.some((object) => object !== this.photo)
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
    this.canvas.setDimensions({ width, height: (width * SCREEN_HEIGHT) / SCREEN_WIDTH });
    this.canvas.setZoom(width / SCREEN_WIDTH);
    this.canvas.requestRenderAll();
  }

  hasPhoto(): boolean {
    return this.photo !== null;
  }

  isEmpty(): boolean {
    return this.canvas.getObjects().length === 0;
  }

  setPhoto(source: ImageBitmap | HTMLImageElement) {
    // Cap the working resolution: plenty for a 1200×1600 panel and safely
    // below the WebGL filter texture limit.
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
      left: SCREEN_WIDTH / 2,
      top: SCREEN_HEIGHT / 2
    });
    const target = SCREEN_WIDTH / SCREEN_HEIGHT;
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
        ? Math.max(SCREEN_WIDTH / spanX, SCREEN_HEIGHT / spanY)
        : Math.min(SCREEN_WIDTH / spanX, SCREEN_HEIGHT / spanY);
    photo.set({ scaleX: factor, scaleY: factor, left: SCREEN_WIDTH / 2, top: SCREEN_HEIGHT / 2 });
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
    if (this.look === 'punch') {
      list.push(new filters.Contrast({ contrast: 0.16 }), new filters.Saturation({ saturation: 0.45 }));
    } else if (this.look === 'kodak') list.push(new filters.Kodachrome());
    else if (this.look === 'polaroid') list.push(new filters.Polaroid());
    else if (this.look === 'vintage') list.push(new filters.Vintage());
    else if (this.look === 'mono') {
      list.push(new filters.Grayscale(), new filters.Contrast({ contrast: 0.12 }));
    }
    const { brightness, contrast, saturation } = this.adjust;
    if (brightness !== 0) list.push(new filters.Brightness({ brightness }));
    if (contrast !== 0) list.push(new filters.Contrast({ contrast }));
    if (saturation !== 0) list.push(new filters.Saturation({ saturation }));
    photo.filters = list;
    photo.applyFilters();
    this.canvas.requestRenderAll();
    this.changed();
  }

  addText(fontFamily: string, fill: string, content = 'Hello!') {
    const text = new IText(content, {
      left: SCREEN_WIDTH / 2,
      top: SCREEN_HEIGHT / 2,
      fontFamily,
      fontSize: 140,
      fill,
      textAlign: 'center'
    });
    this.canvas.add(text);
    this.canvas.setActiveObject(text);
    text.enterEditing();
    text.selectAll();
    this.canvas.requestRenderAll();
  }

  addSticker(definition: StickerDef) {
    const sticker = new Path(definition.path, {
      left: SCREEN_WIDTH / 2 + (Math.random() - 0.5) * 160,
      top: SCREEN_HEIGHT / 2 + (Math.random() - 0.5) * 160,
      angle: (Math.random() - 0.5) * 24,
      fill: definition.fill,
      stroke: definition.stroke ?? null,
      strokeWidth: definition.strokeWidth ?? 0,
      opacity: definition.opacity ?? 1,
      scaleX: 3.4,
      scaleY: 3.4
    });
    this.canvas.add(sticker);
    this.canvas.setActiveObject(sticker);
    this.canvas.requestRenderAll();
  }

  setDrawMode(on: boolean) {
    if (on) this.canvas.discardActiveObject();
    this.canvas.isDrawingMode = on;
    this.canvas.requestRenderAll();
  }

  setBrush(color: string, width: number) {
    this.brush.color = color;
    this.brush.width = width;
  }

  /** Repaint the selection with a pigment: fills keep fills, strokes keep strokes. */
  recolorSelection(hex: string) {
    const objects = this.canvas.getActiveObjects();
    for (const object of objects) {
      if (object === this.photo) continue;
      if (object.get('fill')) object.set('fill', hex);
      else object.set('stroke', hex);
    }
    if (objects.length > 0) {
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

  /** Render the composition at exactly panel resolution (controls excluded). */
  exportFrame(): ImageData {
    const snapshot = this.canvas.toCanvasElement(SCREEN_WIDTH / this.canvas.getWidth());
    const out = document.createElement('canvas');
    out.width = SCREEN_WIDTH;
    out.height = SCREEN_HEIGHT;
    const context = out.getContext('2d', { alpha: false, willReadFrequently: true })!;
    context.fillStyle = '#ffffff';
    context.fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    context.drawImage(snapshot, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    return context.getImageData(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
  }

  dispose() {
    void this.canvas.dispose();
  }
}
