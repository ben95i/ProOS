// Automate Website - animated stars background
//
// new AnimatedBackground('star-canvas', enabled) then .activate() to draw and
// start the loop. pause() / resume() follow the page activity timeout,
// setEnabled() is the user's own setting, destroy() ends the instance.

import { logError } from './error.js';

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
  constructor() {
    // Where the star sits inside its own cell, fixed for the life of the
    // star. A re-layout moves the cell, never the offset within it, so the
    // field keeps its shape across a resize instead of being reshuffled.
    this.jitterX = Math.random();
    this.jitterY = Math.random();
    this.x = 0;
    this.y = 0;
    this.size = Math.random() * BG_STAR_SIZE;
    this.alpha = Math.random();
  }

  // Lays the star in the cell it has been given, offset by its own jitter.
  // One star per cell is what spreads the field evenly: placing every star
  // freely across the window instead lets them clump.
  placeInCell(column, row, cellWidth, cellHeight) {
    this.x = (column + this.jitterX) * cellWidth;
    this.y = (row + this.jitterY) * cellHeight;
  }

  draw(ctx) {
    ctx.globalAlpha = this.alpha;
    ctx.fillRect(this.x, this.y, this.size, this.size);
  }
}

// one shooting star: falls across the canvas, then resets above it again
class ShootingStar {
  constructor(ctx) {
    this.ctx = ctx;
    this.reset();
    this.delay = Math.random() * 300;
  }

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

  // enabled is the user's own setting, separate from pause() / resume(),
  // which the activity timeout drives. Off means the star field stays on
  // screen but nothing moves, and activity no longer starts it.
  constructor(canvasId = 'star-canvas', enabled = true) {
    this.enabled = enabled !== false;
    this.canvas = document.getElementById(canvasId);
    if (!this.canvas) {
      logError('starting the stars background', `canvas '${canvasId}' not found.`);
      return;
    }
    this.ctx = this.canvas.getContext('2d');
    this.backgroundStars = [];
    this.shootingStars = [];
    this.animationId = null;
    this.running = false;
    this.shootingStarsRunning = true;
    // The background stars never move, so the field is rasterised once into
    // this off-screen canvas and blitted each frame instead of being redrawn
    // star by star at 60fps. Same pixels, a fraction of the work.
    this.fieldCanvas = document.createElement('canvas');
    this.fieldCtx = this.fieldCanvas.getContext('2d');
    this.dpr = 1;
    // a frame of nothing but the static field is worth drawing once, not
    // sixty times a second
    this.staticDrawn = false;
    // resize fires in bursts (a phone hiding its address bar); the work is
    // folded into the next frame instead of running per event
    this.resizePending = false;
    this.resizeCanvas = this.resizeCanvas.bind(this);
    this.onResize = this.onResize.bind(this);
    this.animateCanvas = this.animateCanvas.bind(this);
  }

  activate() {
    window.addEventListener('resize', this.onResize);
    // with the animation switched off the field is cut and painted once by
    // resizeCanvas() below, and no loop is started at all
    this.running = this.enabled;
    this.resizeCanvas();
    if (this.enabled) {
      this.animateCanvas();
    }
  }

  // the user's setting; also applied on startup from what was saved
  setEnabled(on) {
    this.enabled = on !== false;
    if (this.enabled) {
      this.resume();
    } else {
      this.pause();
    }
  }

  // coalesces a burst of resize events into one redraw
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

  // stops the loop and leaves the star field on screen, no longer moving
  pause() {
    this.running = false;
    this.staticDrawn = false;
    if (this.animationId) {
      cancelAnimationFrame(this.animationId);
      this.animationId = null;
    }
    // reset the shooting stars so they do not resume mid-flight later
    this.shootingStars.forEach(star => star.reset());
    this.drawStarField();
  }

  resume() {
    // switched off by the user: activity does not bring the animation back
    if (!this.enabled) {
      return;
    }
    // only if it is actually paused, so two loops cannot run at once
    if (this.animationId) {
      return;
    }
    this.running = true;
    this.staticDrawn = false;
    // fresh start delays, so they do not all shoot at the same moment
    this.shootingStars.forEach(star => {
      star.delay = Math.random() * 200 + 50;
    });
    this.animateCanvas();
  }

  destroy() {
    window.removeEventListener('resize', this.onResize);
    this.running = false;
    if (this.animationId) {
      cancelAnimationFrame(this.animationId);
      this.animationId = null;
    }
  }

  toggleShootingStars(isRunning) {
    this.shootingStarsRunning = isRunning;
    // the field has to go down once more to wipe any trail left mid-flight
    this.staticDrawn = false;
  }

  // Paints the still field into the off-screen canvas. Only called when the
  // field itself changes -- that is, on a resize.
  renderStarField() {
    if (!this.fieldCtx) {
      return;
    }
    this.fieldCtx.setTransform(1, 0, 0, 1, 0, 0);
    this.fieldCtx.scale(this.dpr, this.dpr);
    this.fieldCtx.globalAlpha = 1;
    this.fieldCtx.fillStyle = BG_COLOR;
    this.fieldCtx.fillRect(0, 0, window.innerWidth, window.innerHeight);
    this.fieldCtx.fillStyle = '#fff';
    this.backgroundStars.forEach(star => star.draw(this.fieldCtx));
    this.fieldCtx.globalAlpha = 1;
  }

  // the background and its stars, one still frame -- what stays on screen
  // while the animation is stopped
  drawStarField() {
    if (!this.ctx) {
      return;
    }
    // blitted 1:1 in device pixels, so the result is identical to painting
    // every star again through the scaled transform
    this.ctx.save();
    this.ctx.setTransform(1, 0, 0, 1, 0, 0);
    this.ctx.globalAlpha = 1;
    this.ctx.drawImage(this.fieldCanvas, 0, 0);
    this.ctx.restore();
    this.ctx.globalAlpha = 1;
  }

  // brings both canvases to the size of the window and re-cuts the field
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
    this.staticDrawn = false;
    // setting the canvas size wipes it, so a field that is not being redrawn
    // by the loop has to be painted again here
    if (!this.running) {
      this.drawStarField();
    }
  }

  // Brings the field to the size of the window and lays it out again. Stars
  // already on screen are kept, so a resize does not reshuffle a field that
  // is sitting still.
  //
  // The layout is done in full every time rather than only for the stars that
  // ended up off screen. A window that grew used to leave the stars it
  // already had inside the old rectangle while the new ones filled the whole
  // canvas, which left a seam at roughly double the density.
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

  // Spreads the field over a grid of one star to a cell. The grid is shaped
  // to the window, so the cells come out roughly square whatever the aspect
  // ratio, and there are always at least as many cells as stars: the spare
  // ones are spread through the grid rather than left in a bare strip.
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

  animateCanvas() {
    if (this.shootingStarsRunning) {
      this.drawStarField();
      this.shootingStars.forEach(star => star.update());
      this.staticDrawn = false;
    } else if (!this.staticDrawn) {
      // nothing is moving: the field goes down once and the canvas is then
      // left holding those exact pixels rather than being repainted with them
      // sixty times a second
      this.drawStarField();
      this.staticDrawn = true;
    }
    this.animationId = requestAnimationFrame(this.animateCanvas);
  }
}
