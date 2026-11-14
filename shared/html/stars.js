// Automate - animated stars background.
// new AnimatedBackground('star-field', enabled) then .activate(). pause() / resume()
// follow the activity timeout, setEnabled() is the user's setting, destroy() ends it.
// The still field is two square tiles of stars, each drawn once into a PNG and repeated
// over the page as a background; their sides share no factor, so the pattern does not
// visibly repeat, and a resize draws nothing. Each shooting star is a small SVG line moved
// by the compositor, so nothing here runs per frame and the stars keep moving while the
// page is busy. No canvas stays on the page, so nothing can come back blank after a lost
// GPU context, a hidden tab or a page the browser loaded ahead of time.

import { svgElement } from './utils.js';
import { logError } from './error.js';

const BG_STAR_DENSITY = 2500; // one star per this many pixels, over both tiles together
const BG_STAR_SIZE = 1.7;
// The tiles' sides in CSS pixels. Both are prime, so the two patterns line up again only
// every 509 × 383 px.
const BG_TILE_SIZES = [509, 383];
const MAX_SH_STARS = 6;
const SH_STAR_MIN_SPEED = 5;
const SH_STAR_SPEED_VARIANCE = 10;
const SH_STAR_MIN_LENGTH = 50;
const SH_STAR_LENGTH_VARIANCE = 150;
const SH_STAR_MIN_THICKNESS = 0.3;
const SH_STAR_THICKNESS_VARIANCE = 1.2;
const SH_STAR_BASE_ANGLE = Math.PI / 4;
const SH_STAR_ANGLE_VARIANCE = 0.05;
const SH_STAR_START_Y_OFFSET = -150;
const SH_STAR_FIRST_DELAY_FRAMES = 300;
const SH_STAR_MIN_DELAY_FRAMES = 50;
const SH_STAR_DELAY_VARIANCE_FRAMES = 200;
// clear space around a star's stroke inside its SVG, for the round line caps
const SH_STAR_PADDING = 3;
const SH_STAR_CLASS = 'shooting-star';
const SH_LAYER_ID = 'shooting-star-layer';
const MAX_PIXEL_RATIO = 1.5;
// speeds and delays count frames at this rate, so a star looks the same on any screen
const FRAMES_PER_SECOND = 60;
const MS_PER_FRAME = 1000 / FRAMES_PER_SECOND;

// the pixel ratio the field's tiles are drawn at
function canvasPixelRatio() {
  return Math.min(window.devicePixelRatio || 1, MAX_PIXEL_RATIO);
}

// Draws one tile of the still field: starCount stars on a grid of roughly square cells, one
// star per cell at a random place inside it, so no star is cut by the tile's edge. Answers
// { url, size }, an object URL of the tile's PNG and its side in CSS pixels, or null when
// the browser could not encode it. drawFieldTile(509, 1.5, 51)
async function drawFieldTile(tileSize, pixelRatio, starCount) {
  // a whole number of canvas pixels, so the tile is shown at exactly one per device pixel
  const pixelSize = Math.round(tileSize * pixelRatio);
  const cssSize = pixelSize / pixelRatio;
  const canvas = document.createElement('canvas');
  canvas.width = pixelSize;
  canvas.height = pixelSize;
  const ctx = canvas.getContext('2d');
  ctx.setTransform(pixelRatio, 0, 0, pixelRatio, 0, 0);
  ctx.fillStyle = '#fff';
  const columns = Math.max(1, Math.round(Math.sqrt(starCount)));
  const rows = Math.max(1, Math.ceil(starCount / columns));
  const cells = columns * rows;
  const cellWidth = cssSize / columns;
  const cellHeight = cssSize / rows;
  for (let index = 0; index < starCount; index++) {
    // any spare cells are spread through the grid
    const cell = ~~(index * cells / starCount);
    const column = cell % columns;
    const row = ~~(cell / columns);
    const starSize = Math.random() * BG_STAR_SIZE;
    const offsetX = Math.random() * (cellWidth - starSize);
    const offsetY = Math.random() * (cellHeight - starSize);
    const starX = (column * cellWidth) + offsetX;
    const starY = (row * cellHeight) + offsetY;
    ctx.globalAlpha = Math.random();
    ctx.fillRect(starX, starY, starSize, starSize);
  }
  const blob = await new Promise(resolve => canvas.toBlob(resolve, 'image/png'));
  // frees the canvas's pixels now rather than at the next garbage collection
  canvas.width = 0;
  canvas.height = 0;
  if (!blob) {
    return null;
  }
  return { url: URL.createObjectURL(blob), size: cssSize };
}

// releases the tiles' object URLs: [{ url, size }]
function revokeTiles(tiles) {
  tiles.forEach(tile => URL.revokeObjectURL(tile.url));
}

// One shooting star: falls across the window from above it, waits, then falls again.
// launch(delayFrames) starts a flight and stop() ends it.
class ShootingStar {
  // Its SVG goes into the layer the stars share; index keeps its gradient's id apart from
  // the other stars'.
  constructor(layer, index) {
    const gradientId = `${SH_STAR_CLASS}-tail-${index}`;
    // tail to head along the line's bounding box, whose corners are the line's two ends
    const gradient = svgElement('linearGradient', { id: gradientId, x1: 0, y1: 0, x2: 1, y2: 1 });
    gradient.append(
      svgElement('stop', { offset: 0, 'stop-color': '#fff', 'stop-opacity': 0 }),
      svgElement('stop', { offset: 1, 'stop-color': '#fff' })
    );
    const defs = svgElement('defs');
    defs.append(gradient);
    this.line = svgElement('line', { stroke: `url(#${gradientId})`, 'stroke-linecap': 'round' });
    this.element = svgElement('svg', { class: SH_STAR_CLASS, width: 0, height: 0 });
    this.element.append(defs, this.line);
    this.animation = null;
    layer.appendChild(this.element);
  }

  // Draws the star with the head at the bottom right corner of its SVG and the tail fading
  // out up and left. Answers where the head is, in CSS pixels: { x, y }.
  paint(length, thickness, angle) {
    const padding = SH_STAR_PADDING;
    const tailWidth = length * Math.cos(angle);
    const tailHeight = length * Math.sin(angle);
    const doublePadding = 2 * padding;
    const width = Math.ceil(tailWidth) + doublePadding;
    const height = Math.ceil(tailHeight) + doublePadding;
    const headX = width - padding;
    const headY = height - padding;
    const tailX = headX - tailWidth;
    const tailY = headY - tailHeight;
    this.element.setAttribute('width', width);
    this.element.setAttribute('height', height);
    this.line.setAttribute('x1', tailX);
    this.line.setAttribute('y1', tailY);
    this.line.setAttribute('x2', headX);
    this.line.setAttribute('y2', headY);
    this.line.setAttribute('stroke-width', thickness);
    return { x: headX, y: headY };
  }

  // Picks a new start, speed, length and angle and flies the star after delayFrames, held
  // above the window until then and past it after. The flight ends once the head is a
  // star's length past the window's right or bottom edge.
  launch(delayFrames) {
    this.stop();
    const startSpread = window.innerWidth * 0.5;
    const startInset = window.innerWidth * 0.2;
    const startX = (Math.random() * startSpread) - startInset;
    const startY = Math.random() * SH_STAR_START_Y_OFFSET;
    const speed = (Math.random() * SH_STAR_SPEED_VARIANCE) + SH_STAR_MIN_SPEED;
    const length = (Math.random() * SH_STAR_LENGTH_VARIANCE) + SH_STAR_MIN_LENGTH;
    const thickness = (Math.random() * SH_STAR_THICKNESS_VARIANCE) + SH_STAR_MIN_THICKNESS;
    const angleSpread = SH_STAR_ANGLE_VARIANCE * 2;
    const angleOffset = (Math.random() * angleSpread) - SH_STAR_ANGLE_VARIANCE;
    const angle = SH_STAR_BASE_ANGLE + angleOffset;
    const stepX = speed * Math.cos(angle);
    const stepY = speed * Math.sin(angle);
    const framesToRight = (window.innerWidth + length - startX) / stepX;
    const framesToBottom = (window.innerHeight + length - startY) / stepY;
    const flightFrames = Math.min(framesToRight, framesToBottom);
    const head = this.paint(length, thickness, angle);
    const fromX = startX - head.x;
    const fromY = startY - head.y;
    const travelX = stepX * flightFrames;
    const travelY = stepY * flightFrames;
    const toX = fromX + travelX;
    const toY = fromY + travelY;
    // 'both' keeps the star off screen between its landing and the next launch, which
    // the finish event can reach a frame or more later
    this.animation = this.element.animate([
      { transform: `translate(${fromX}px, ${fromY}px)` },
      { transform: `translate(${toX}px, ${toY}px)` }
    ], {
      duration: flightFrames * MS_PER_FRAME,
      delay: delayFrames * MS_PER_FRAME,
      easing: 'linear',
      fill: 'both'
    });
    this.animation.onfinish = () => this.launch(ShootingStar.nextDelayFrames());
  }

  // a star's wait between flights, in frames
  static nextDelayFrames() {
    return Math.random() * SH_STAR_DELAY_VARIANCE_FRAMES + SH_STAR_MIN_DELAY_FRAMES;
  }

  // ends the flight, if there is one
  stop() {
    if (this.animation) {
      this.animation.onfinish = null;
      this.animation.cancel();
      this.animation = null;
    }
  }
}

export default class AnimatedBackground {

  // enabled is the user's setting: off leaves the field on screen with nothing moving.
  constructor(fieldId, enabled) {
    this.enabled = enabled !== false;
    this.field = document.getElementById(fieldId);
    this.shootingStars = [];
    // inside the field, which clips the shooting stars to the window so they can never
    // widen the page
    this.starLayer = document.createElement('div');
    this.starLayer.id = SH_LAYER_ID;
    this.starLayer.style.display = 'none';
    this.field.appendChild(this.starLayer);
    this.running = false;
    // the tiles on the field, [{ url, size }], and the pixel ratio they were drawn at
    this.fieldTiles = [];
    this.fieldRatio = 0;
    // counts the field's draws, so one overtaken by a newer draw or destroy() is dropped
    this.fieldDrawCount = 0;
    // a burst of resize events is folded into the next frame
    this.resizePending = false;
    this.onResize = this.onResize.bind(this);
  }

  // draws the field and, when enabled, starts the stars
  activate() {
    window.addEventListener('resize', this.onResize);
    this.fitToWindow();
    if (this.enabled) {
      this.startFlights(SH_STAR_FIRST_DELAY_FRAMES);
    }
  }

  // the user's setting
  setEnabled(on) {
    this.enabled = on !== false;
    if (this.enabled) {
      this.resume();
    } else {
      this.pause();
    }
  }

  // folds a burst of resize events into one fit
  onResize() {
    if (this.resizePending) {
      return;
    }
    this.resizePending = true;
    requestAnimationFrame(() => {
      this.resizePending = false;
      this.fitToWindow();
    });
  }

  // stops the stars, leaving the field on screen
  pause() {
    this.running = false;
    this.shootingStars.forEach(star => star.stop());
    this.starLayer.style.display = 'none';
  }

  // restarts the stars unless the user switched them off or they are already flying
  resume() {
    if (!this.enabled || this.running) {
      return;
    }
    // fresh delays, so they do not all shoot at once
    this.startFlights(0);
  }

  // Shows the stars and flies each after a random wait: up to firstDelayFrames, or the
  // usual wait between flights when that is 0.
  startFlights(firstDelayFrames) {
    this.running = true;
    this.starLayer.style.display = 'block';
    while (this.shootingStars.length < MAX_SH_STARS) {
      const index = this.shootingStars.length;
      this.shootingStars.push(new ShootingStar(this.starLayer, index));
    }
    this.shootingStars.forEach(star => {
      let delayFrames = ShootingStar.nextDelayFrames();
      if (firstDelayFrames > 0) {
        delayFrames = Math.random() * firstDelayFrames;
      }
      star.launch(delayFrames);
    });
  }

  // ends the instance, dropping a draw still under way
  destroy() {
    window.removeEventListener('resize', this.onResize);
    this.pause();
    this.fieldDrawCount += 1;
    revokeTiles(this.fieldTiles);
    this.fieldTiles = [];
  }

  // Sizes the field to the window, and draws its tiles again only when the pixel ratio
  // changed (a zoom, or a move to another screen).
  fitToWindow() {
    this.field.style.width = `${window.innerWidth}px`;
    this.field.style.height = `${window.innerHeight}px`;
    const pixelRatio = canvasPixelRatio();
    if (pixelRatio !== this.fieldRatio) {
      this.fieldRatio = pixelRatio;
      this.drawField(pixelRatio);
    }
  }

  // Draws both tiles and lays them over the field, the old tiles staying until the new
  // ones are ready. A draw overtaken by a newer one or by destroy() is thrown away.
  async drawField(pixelRatio) {
    this.fieldDrawCount += 1;
    const drawNumber = this.fieldDrawCount;
    const starsPerArea = BG_STAR_DENSITY * BG_TILE_SIZES.length;
    const tiles = await Promise.all(BG_TILE_SIZES.map(tileSize => {
      const tileArea = tileSize * tileSize;
      const starCount = ~~(tileArea / starsPerArea);
      return drawFieldTile(tileSize, pixelRatio, starCount);
    }));
    const drawnTiles = tiles.filter(tile => tile !== null);
    if (drawNumber !== this.fieldDrawCount) {
      revokeTiles(drawnTiles);
      return;
    }
    if (drawnTiles.length !== tiles.length) {
      revokeTiles(drawnTiles);
      logError('drawing the star field', 'the browser could not encode a tile');
      // the next resize tries again
      this.fieldRatio = 0;
      return;
    }
    const images = drawnTiles.map(tile => `url("${tile.url}")`);
    const sizes = drawnTiles.map(tile => `${tile.size}px ${tile.size}px`);
    this.field.style.backgroundImage = images.join(', ');
    this.field.style.backgroundSize = sizes.join(', ');
    revokeTiles(this.fieldTiles);
    this.fieldTiles = drawnTiles;
  }
}
