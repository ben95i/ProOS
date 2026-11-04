// Automate - animated stars background.
// new AnimatedBackground('star-canvas', enabled) then .activate(). pause() / resume()
// follow the activity timeout, setEnabled() is the user's setting, destroy() ends it.
// The still field is painted once, on a resize. Each shooting star is a small canvas of
// its own, painted once per flight and moved by the compositor, so nothing here runs per
// frame and the stars keep moving while the page is busy.

const BG_COLOR = '#000000';
const BG_STAR_DENSITY = 2500; // one star per this many pixels
const BG_STAR_SIZE = 1.7;
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
// clear space around a star's stroke inside its canvas
const SH_STAR_CANVAS_PADDING = 3;
const SH_STAR_CLASS = 'shooting-star';
const SH_LAYER_ID = 'shooting-star-layer';
const MAX_PIXEL_RATIO = 1.5;
// speeds and delays count frames at this rate, so a star looks the same on any screen
const FRAMES_PER_SECOND = 60;
const MS_PER_FRAME = 1000 / FRAMES_PER_SECOND;

// the pixel ratio the canvases are drawn at
function canvasPixelRatio() {
  return Math.min(window.devicePixelRatio || 1, MAX_PIXEL_RATIO);
}

// one still star, laid out on the grid by placeInCell()
class BackgroundStar {
  // A star's place inside its cell is fixed, so a resize moves the cell and keeps the
  // field's shape.
  constructor() {
    this.jitterX = Math.random();
    this.jitterY = Math.random();
    this.x = 0;
    this.y = 0;
    this.size = Math.random() * BG_STAR_SIZE;
    this.alpha = Math.random();
  }

  // Lays the star in its cell; one star per cell spreads the field evenly.
  placeInCell(column, row, cellWidth, cellHeight) {
    this.x = (column + this.jitterX) * cellWidth;
    this.y = (row + this.jitterY) * cellHeight;
  }

  // paints the star
  draw(ctx) {
    ctx.globalAlpha = this.alpha;
    ctx.fillRect(this.x, this.y, this.size, this.size);
  }
}

// One shooting star: falls across the window from above it, waits, then falls again.
// launch(delayFrames) starts a flight and stop() ends it.
class ShootingStar {
  // its canvas goes into the layer the stars share
  constructor(layer) {
    this.element = document.createElement('canvas');
    this.element.className = SH_STAR_CLASS;
    this.ctx = this.element.getContext('2d');
    this.animation = null;
    layer.appendChild(this.element);
  }

  // Paints the star on its canvas with the head at the bottom right corner and the tail
  // fading out up and left. Answers where the head is, in CSS pixels: { x, y }.
  paint(length, thickness, angle) {
    const padding = SH_STAR_CANVAS_PADDING;
    const tailWidth = Math.ceil(length * Math.cos(angle));
    const tailHeight = Math.ceil(length * Math.sin(angle));
    const cssWidth = tailWidth + 2 * padding;
    const cssHeight = tailHeight + 2 * padding;
    const dpr = canvasPixelRatio();
    this.element.width = cssWidth * dpr;
    this.element.height = cssHeight * dpr;
    this.element.style.width = `${cssWidth}px`;
    this.element.style.height = `${cssHeight}px`;
    const headX = cssWidth - padding;
    const headY = cssHeight - padding;
    const ctx = this.ctx;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.translate(headX, headY);
    ctx.rotate(angle);
    const gradient = ctx.createLinearGradient(0, 0, -length, 0);
    gradient.addColorStop(0, '#fff');
    gradient.addColorStop(1, 'rgba(255, 255, 255, 0)');
    ctx.strokeStyle = gradient;
    ctx.lineWidth = thickness;
    ctx.lineCap = 'round';
    ctx.beginPath();
    ctx.moveTo(0, 0);
    ctx.lineTo(-length, 0);
    ctx.stroke();
    return { x: headX, y: headY };
  }

  // Picks a new start, speed, length and angle and flies the star after delayFrames, held
  // above the window until then. The flight ends once the head is a star's length past the
  // window's right or bottom edge.
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
    this.animation = this.element.animate([
      { transform: `translate(${fromX}px, ${fromY}px)` },
      { transform: `translate(${toX}px, ${toY}px)` }
    ], {
      duration: flightFrames * MS_PER_FRAME,
      delay: delayFrames * MS_PER_FRAME,
      easing: 'linear',
      fill: 'backwards'
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
  constructor(canvasId, enabled) {
    this.enabled = enabled !== false;
    this.canvas = document.getElementById(canvasId);
    // the field covers the whole canvas, so it needs no alpha channel
    this.ctx = this.canvas.getContext('2d', { alpha: false });
    this.backgroundStars = [];
    this.shootingStars = [];
    // clips the shooting stars to the window, so they can never widen the page
    this.starLayer = document.createElement('div');
    this.starLayer.id = SH_LAYER_ID;
    this.starLayer.style.display = 'none';
    this.canvas.after(this.starLayer);
    this.running = false;
    // what the field was last cut for
    this.fieldWidth = -1;
    this.fieldHeight = -1;
    this.fieldRatio = 0;
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

  // folds a burst of resize events into one redraw
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
      this.shootingStars.push(new ShootingStar(this.starLayer));
    }
    this.shootingStars.forEach(star => {
      let delayFrames = ShootingStar.nextDelayFrames();
      if (firstDelayFrames > 0) {
        delayFrames = Math.random() * firstDelayFrames;
      }
      star.launch(delayFrames);
    });
  }

  // ends the instance
  destroy() {
    window.removeEventListener('resize', this.onResize);
    this.pause();
  }

  // Keeps the field for a window that only got shorter or came back to a height it has
  // been: a phone's toolbar and keyboard change only the height, often while scrolling, and
  // the part of the field under them does not show.
  fitToWindow() {
    const width = window.innerWidth;
    const height = window.innerHeight;
    const dpr = canvasPixelRatio();
    const fieldCovers = width === this.fieldWidth && height <= this.fieldHeight;
    if (fieldCovers && dpr === this.fieldRatio) {
      return;
    }
    this.fieldWidth = width;
    this.fieldHeight = height;
    this.fieldRatio = dpr;
    this.canvas.width = width * dpr;
    this.canvas.height = height * dpr;
    this.canvas.style.width = `${width}px`;
    this.canvas.style.height = `${height}px`;
    this.starLayer.style.width = `${width}px`;
    this.starLayer.style.height = `${height}px`;
    this.initBackgroundStars(width, height);
    this.renderStarField(width, height, dpr);
  }

  // paints the still field; setting the canvas size has just wiped it
  renderStarField(width, height, dpr) {
    this.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    this.ctx.globalAlpha = 1;
    this.ctx.fillStyle = BG_COLOR;
    this.ctx.fillRect(0, 0, width, height);
    this.ctx.fillStyle = '#fff';
    this.backgroundStars.forEach(star => star.draw(this.ctx));
    this.ctx.globalAlpha = 1;
  }

  // Sizes the field to the window and lays all of it out again, keeping the stars it has
  // so a resize does not reshuffle them.
  initBackgroundStars(width, height) {
    const screenArea = width * height;
    const numStars = ~~(screenArea / BG_STAR_DENSITY);
    if (this.backgroundStars.length > numStars) {
      this.backgroundStars.length = numStars;
    }
    while (this.backgroundStars.length < numStars) {
      this.backgroundStars.push(new BackgroundStar());
    }
    this.layoutBackgroundStars(width, height);
  }

  // Spreads the field over a window-shaped grid of roughly square cells, one star per
  // cell, with any spare cells spread through the grid.
  layoutBackgroundStars(windowWidth, windowHeight) {
    const total = this.backgroundStars.length;
    if (total === 0) {
      return;
    }
    const width = Math.max(1, windowWidth);
    const height = Math.max(1, windowHeight);
    const idealColumns = Math.sqrt(total * width / height);
    const columns = Math.max(1, Math.round(idealColumns));
    const rows = Math.max(1, Math.ceil(total / columns));
    const cells = columns * rows;
    const cellWidth = width / columns;
    const cellHeight = height / rows;
    for (let i = 0; i < total; i++) {
      const cell = ~~(i * cells / total);
      const column = cell % columns;
      const row = ~~(cell / columns);
      this.backgroundStars[i].placeInCell(column, row, cellWidth, cellHeight);
    }
  }
}
