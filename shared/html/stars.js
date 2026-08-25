// animated stars background canvas //

const BG_COLOR = '#000000';
const BG_STAR_DENSITY = 2500;
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

class BackgroundStar {
  constructor() {
    this.scatter();
    this.size = Math.random() * BG_STAR_SIZE;
    this.alpha = Math.random(); 
  }

  scatter() {
    this.x = Math.random() * window.innerWidth;
    this.y = Math.random() * window.innerHeight;
  }

  // after a resize, only the stars left off-screen are moved
  keepOnScreen() {
    if (this.x > window.innerWidth || this.y > window.innerHeight) {
      this.scatter();
    }
  }

  draw(ctx) {
    ctx.globalAlpha = this.alpha;
    ctx.fillRect(this.x, this.y, this.size, this.size);
  }
}

class ShootingStar {
  constructor(ctx) {
    this.ctx = ctx;
    this.reset();
    this.delay = Math.random() * 300; 
  }

  reset() {
    this.x = Math.random() * window.innerWidth * 0.5 - (window.innerWidth * 0.2); 
    this.y = Math.random() * SH_STAR_START_Y_OFFSET; 
    const speed = (Math.random() * SH_STAR_SPEED_VARIANCE) + SH_STAR_MIN_SPEED; 
    this.length = (Math.random() * SH_STAR_LENGTH_VARIANCE) + SH_STAR_MIN_LENGTH;
    this.thickness = (Math.random() * SH_STAR_THICKNESS_VARIANCE) + SH_STAR_MIN_THICKNESS;
    this.angle = SH_STAR_BASE_ANGLE + (Math.random() * (SH_STAR_ANGLE_VARIANCE * 2) - SH_STAR_ANGLE_VARIANCE); 
    this.vx = speed * Math.cos(this.angle);
    this.vy = speed * Math.sin(this.angle);
    
    // Gradient requires ctx
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
    
    if (this.x > window.innerWidth + this.length || 
        this.y > window.innerHeight + this.length) {
      this.reset();
    } else {
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
}

export default class AnimatedBackground {
  // enabled is the user's own setting (About window), separate from pause() /
  // resume(), which the activity timeout drives. Off means the star field
  // stays on screen but nothing moves, and activity no longer starts it.
  constructor(canvasId = 'star-canvas', enabled = true) {
    this.enabled = enabled !== false;
    this.canvas = document.getElementById(canvasId);
    if (!this.canvas) {
      console.error(`AnimatedBackground: Canvas with id '${canvasId}' not found.`);
      return;
    }
    
    this.ctx = this.canvas.getContext('2d');
    this.backgroundStars = [];
    this.shootingStars = [];
    this.animationId = null;
    // whether the loop is running; the frozen frame is redrawn without it
    this.running = false;
    
    // Replaces the global App.starsRunning
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

    // Bind methods to preserve 'this' context in event listeners and requestAnimationFrame
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

  // the About window's switch; also applied on startup from the saved setting
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

  pause() {
    // 1. Stop the continuous animation loop
    this.running = false;
    this.staticDrawn = false;
    if (this.animationId) {
      cancelAnimationFrame(this.animationId);
      this.animationId = null;
    }
    // 2. Reset all shooting stars so they don't resume mid-flight later
    for (let i = 0; i < this.shootingStars.length; i++) {
      this.shootingStars[i].reset();
    }
    // 3. Leave the star field on screen, just no longer moving
    this.drawStarField();
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
    for (let i = 0; i < this.backgroundStars.length; i++) {
      this.backgroundStars[i].draw(this.fieldCtx);
    }
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

  resume() {
    // switched off by the user: activity does not bring the animation back
    if (!this.enabled) {
      return;
    }
    // Only resume if it's actually paused to prevent multiple loops running
    if (!this.animationId) {
      this.running = true;
      this.staticDrawn = false;
      // Randomize their start delays again so they don't all shoot at the exact same time
      for (let i = 0; i < this.shootingStars.length; i++) {
        this.shootingStars[i].delay = Math.random() * 200 + 50;
      }
      // Restart the loop
      this.animateCanvas();
    }
  }

  destroy() {
    // Clean up event listeners and animation frames if the component is ever removed
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

  resizeCanvas() {
    const dpr = Math.min(window.devicePixelRatio || 1, 1.5);
    this.dpr = dpr;
    this.canvas.width = window.innerWidth * dpr;
    this.canvas.height = window.innerHeight * dpr;
    this.canvas.style.width = `${window.innerWidth}px`;
    this.canvas.style.height = `${window.innerHeight}px`;
    this.ctx.scale(dpr, dpr);
    this.fieldCanvas.width = this.canvas.width;
    this.fieldCanvas.height = this.canvas.height;
    this.initBackgroundStars();
    // the star field moved or changed size, so its cached frame is re-cut
    this.renderStarField();
    this.staticDrawn = false;
    // setting the canvas size wipes it, so a field that is not being redrawn
    // by the loop has to be painted again here
    if (!this.running) {
      this.drawStarField();
    }
  }

  // Brings the field to the size of the window. Stars already on screen are
  // kept, so a resize -- a phone hiding its address bar, say -- does not
  // reshuffle a field that is sitting still.
  initBackgroundStars() {
    const numStars = ~~((window.innerWidth * window.innerHeight) / BG_STAR_DENSITY);
    if (this.backgroundStars.length > numStars) {
      this.backgroundStars.length = numStars;
    }
    while (this.backgroundStars.length < numStars) {
      this.backgroundStars.push(new BackgroundStar());
    }
    for (let i = 0; i < this.backgroundStars.length; i++) {
      this.backgroundStars[i].keepOnScreen();
    }
    while (this.shootingStars.length < MAX_SH_STARS) {
      this.shootingStars.push(new ShootingStar(this.ctx));
    }
  }

  animateCanvas() {
    if (this.shootingStarsRunning) {
      this.drawStarField();
      for (let i = 0; i < this.shootingStars.length; i++) {
        this.shootingStars[i].update();
      }
      this.staticDrawn = false;
    } else if (!this.staticDrawn) {
      // nothing is moving: the field goes down once and the canvas is then
      // left holding those exact pixels rather than being repainted with
      // them sixty times a second
      this.drawStarField();
      this.staticDrawn = true;
    }
    
    this.animationId = requestAnimationFrame(this.animateCanvas);
  }
}