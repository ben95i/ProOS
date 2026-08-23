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
  constructor(canvasId = 'star-canvas') {
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

    // Bind methods to preserve 'this' context in event listeners and requestAnimationFrame
    this.resizeCanvas = this.resizeCanvas.bind(this);
    this.animateCanvas = this.animateCanvas.bind(this);
  }

  activate() {
    window.addEventListener('resize', this.resizeCanvas);
    this.running = true;
    this.resizeCanvas();
    this.animateCanvas();
  }

  pause() {
    // 1. Stop the continuous animation loop
    this.running = false;
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

  // the background and its stars, one still frame -- what stays on screen
  // while the animation is stopped
  drawStarField() {
    if (!this.ctx) {
      return;
    }
    this.ctx.globalAlpha = 1; 
    this.ctx.fillStyle = BG_COLOR;
    this.ctx.fillRect(0, 0, window.innerWidth, window.innerHeight);
    this.ctx.fillStyle = '#fff';
    for (let i = 0; i < this.backgroundStars.length; i++) {
      this.backgroundStars[i].draw(this.ctx);
    }
    this.ctx.globalAlpha = 1; 
  }

  resume() {
    // Only resume if it's actually paused to prevent multiple loops running
    if (!this.animationId) {
      this.running = true;
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
    window.removeEventListener('resize', this.resizeCanvas);
    this.running = false;
    if (this.animationId) {
      cancelAnimationFrame(this.animationId);
      this.animationId = null;
    }
  }

  toggleShootingStars(isRunning) {
    this.shootingStarsRunning = isRunning;
  }

  resizeCanvas() {
    const dpr = Math.min(window.devicePixelRatio || 1, 1.5);
    this.canvas.width = window.innerWidth * dpr;
    this.canvas.height = window.innerHeight * dpr;
    this.canvas.style.width = `${window.innerWidth}px`;
    this.canvas.style.height = `${window.innerHeight}px`;
    this.ctx.scale(dpr, dpr);
    this.initBackgroundStars();
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
    this.drawStarField();
    
    if (this.shootingStarsRunning) {
      for (let i = 0; i < this.shootingStars.length; i++) {
        this.shootingStars[i].update();
      }
    }
    
    this.animationId = requestAnimationFrame(this.animateCanvas);
  }
}