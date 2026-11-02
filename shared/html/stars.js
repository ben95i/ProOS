// Automate - animated stars background.
// new AnimatedBackground('star-canvas', enabled) then .activate(). pause() / resume()
// follow the activity timeout, setEnabled() is the user's setting, destroy() ends it.

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
const MAX_PIXEL_RATIO = 1.5;

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

// one shooting star: falls across the canvas, then resets above it again
class ShootingStar {
  // starts above the canvas after a random delay
  constructor(ctx) {
    this.ctx = ctx;
    this.reset();
    this.delay = Math.random() * 300;
  }

  // picks a new start, speed, length and delay
  reset() {
    const startSpread = window.innerWidth * 0.5;
    const startInset = window.innerWidth * 0.2;
    this.x = (Math.random() * startSpread) - startInset;
    this.y = Math.random() * SH_STAR_START_Y_OFFSET;
    const speed = (Math.random() * SH_STAR_SPEED_VARIANCE) + SH_STAR_MIN_SPEED;
    this.length = (Math.random() * SH_STAR_LENGTH_VARIANCE) + SH_STAR_MIN_LENGTH;
    this.thickness = (Math.random() * SH_STAR_THICKNESS_VARIANCE) + SH_STAR_MIN_THICKNESS;
    const angleSpread = SH_STAR_ANGLE_VARIANCE * 2;
    const angleOffset = (Math.random() * angleSpread) - SH_STAR_ANGLE_VARIANCE;
    this.angle = SH_STAR_BASE_ANGLE + angleOffset;
    this.vx = speed * Math.cos(this.angle);
    this.vy = speed * Math.sin(this.angle);
    // the tail fades out behind the head
    this.gradient = this.ctx.createLinearGradient(0, 0, -this.length, 0);
    this.gradient.addColorStop(0, '#fff');
    this.gradient.addColorStop(1, 'rgba(255, 255, 255, 0)');
    this.delay = Math.random() * 200 + 50;
  }

  // moves the star and paints it, or resets it once off screen
  update() {
    if (this.delay > 0) {
      this.delay--;
      return;
    }
    this.x += this.vx;
    this.y += this.vy;
    const pastRight = this.x > window.innerWidth + this.length;
    const pastBottom = this.y > window.innerHeight + this.length;
    if (pastRight || pastBottom) {
      this.reset();
      return;
    }
    this.ctx.save();
    this.ctx.translate(this.x, this.y);
    this.ctx.rotate(this.angle);
    this.ctx.strokeStyle = this.gradient;
    this.ctx.lineWidth = this.thickness;
    this.ctx.lineCap = 'round';
    this.ctx.beginPath();
    this.ctx.moveTo(0, 0);
    this.ctx.lineTo(-this.length, 0);
    this.ctx.stroke();
    this.ctx.restore();
  }
}

export default class AnimatedBackground {

  // enabled is the user's setting: off leaves the field on screen with nothing moving.
  constructor(canvasId, enabled) {
    this.enabled = enabled !== false;
    this.canvas = document.getElementById(canvasId);
    this.ctx = this.canvas.getContext('2d');
    this.backgroundStars = [];
    this.shootingStars = [];
    this.animationId = null;
    this.running = false;
    // The still field is rasterised once off-screen and blitted each frame.
    this.fieldCanvas = document.createElement('canvas');
    this.fieldCtx = this.fieldCanvas.getContext('2d');
    this.dpr = 1;
    // a burst of resize events is folded into the next frame
    this.resizePending = false;
    this.onResize = this.onResize.bind(this);
    this.animateCanvas = this.animateCanvas.bind(this);
  }

  // draws the field and, when enabled, starts the loop
  activate() {
    window.addEventListener('resize', this.onResize);
    this.running = this.enabled;
    this.resizeCanvas();
    if (this.enabled) {
      this.animateCanvas();
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
      this.resizeCanvas();
    });
  }

  // stops the loop, leaving the field on screen
  pause() {
    this.running = false;
    if (this.animationId) {
      cancelAnimationFrame(this.animationId);
      this.animationId = null;
    }
    // so they do not resume mid-flight
    this.shootingStars.forEach(star => star.reset());
    this.drawStarField();
  }

  // restarts the loop unless the user switched it off or it is already running
  resume() {
    if (!this.enabled) {
      return;
    }
    if (this.animationId) {
      return;
    }
    this.running = true;
    // fresh delays, so they do not all shoot at once
    this.shootingStars.forEach(star => {
      star.delay = Math.random() * 200 + 50;
    });
    this.animateCanvas();
  }

  // ends the instance
  destroy() {
    window.removeEventListener('resize', this.onResize);
    this.running = false;
    if (this.animationId) {
      cancelAnimationFrame(this.animationId);
      this.animationId = null;
    }
  }

  // Paints the still field off-screen; only needed when the field changes, on a resize.
  renderStarField() {
    this.fieldCtx.setTransform(1, 0, 0, 1, 0, 0);
    this.fieldCtx.scale(this.dpr, this.dpr);
    this.fieldCtx.globalAlpha = 1;
    this.fieldCtx.fillStyle = BG_COLOR;
    this.fieldCtx.fillRect(0, 0, window.innerWidth, window.innerHeight);
    this.fieldCtx.fillStyle = '#fff';
    this.backgroundStars.forEach(star => star.draw(this.fieldCtx));
    this.fieldCtx.globalAlpha = 1;
  }

  // one still frame of the field and its stars
  drawStarField() {
    this.ctx.save();
    this.ctx.setTransform(1, 0, 0, 1, 0, 0);
    this.ctx.globalAlpha = 1;
    this.ctx.drawImage(this.fieldCanvas, 0, 0);
    this.ctx.restore();
    this.ctx.globalAlpha = 1;
  }

  // brings both canvases to the window's size and re-cuts the field
  resizeCanvas() {
    const dpr = Math.min(window.devicePixelRatio || 1, MAX_PIXEL_RATIO);
    this.dpr = dpr;
    this.canvas.width = window.innerWidth * dpr;
    this.canvas.height = window.innerHeight * dpr;
    this.canvas.style.width = `${window.innerWidth}px`;
    this.canvas.style.height = `${window.innerHeight}px`;
    this.ctx.scale(dpr, dpr);
    this.fieldCanvas.width = this.canvas.width;
    this.fieldCanvas.height = this.canvas.height;
    this.initBackgroundStars();
    this.renderStarField();
    // setting the size wipes the canvas, and a stopped loop will not repaint it
    if (!this.running) {
      this.drawStarField();
    }
  }

  // Sizes the field to the window and lays all of it out again, keeping the stars it has
  // so a resize does not reshuffle them.
  initBackgroundStars() {
    const screenArea = window.innerWidth * window.innerHeight;
    const numStars = ~~(screenArea / BG_STAR_DENSITY);
    if (this.backgroundStars.length > numStars) {
      this.backgroundStars.length = numStars;
    }
    while (this.backgroundStars.length < numStars) {
      this.backgroundStars.push(new BackgroundStar());
    }
    this.layoutBackgroundStars();
    while (this.shootingStars.length < MAX_SH_STARS) {
      this.shootingStars.push(new ShootingStar(this.ctx));
    }
  }

  // Spreads the field over a window-shaped grid of roughly square cells, one star per
  // cell, with any spare cells spread through the grid.
  layoutBackgroundStars() {
    const total = this.backgroundStars.length;
    if (total === 0) {
      return;
    }
    const width = Math.max(1, window.innerWidth);
    const height = Math.max(1, window.innerHeight);
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

  // one frame: the field, the shooting stars, and the next frame's request
  animateCanvas() {
    this.drawStarField();
    this.shootingStars.forEach(star => star.update());
    this.animationId = requestAnimationFrame(this.animateCanvas);
  }
}
