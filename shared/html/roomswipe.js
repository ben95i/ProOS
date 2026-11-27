// Automate - the top bar's room selector drawn as a cube with a room name on each face.
// Swiping the name left turns the cube to the next room, right to the previous one, wrapping
// round; a tap still reaches the page's click listener, which opens the room menu.
//   roomCubeTemplate(['Living Room', 'Bedroom', 'Bathroom'])  the cube's markup, put inside
//     #room_anchor; its front face is #current-room-label, which selectRoom() names
//   new RoomSwipe('room_anchor', { neighbourName, onTurn, onRest }) then .activate() to bind,
//     .cancelRest() to drop a rest still to come, .destroy() to unbind:
//     neighbourName(step)  the name of the room step places from the current one (1 or -1)
//     onTurn(step)         a turn landed: step 1 for the next room, -1 for the previous
//     onRest()             REST_MS passed after the last turn with no new swipe: the user
//                          has stopped on a room

import { removeDropdowns } from './dropdowns.js';

const DRAG_THRESHOLD_PX = 3;
// a release turns the cube once the drag is this share of a face's width
const TURN_SHARE = 0.3;
// or once the drag was this fast, whatever its length
const FLICK_PX_PER_MS = 0.5;
// the turn's settle, also handed to roomswipe.css as --cube-settle
const SETTLE_MS = 220;
// how long the cube stays on a room before onRest() reads it
const REST_MS = 600;
const CLICK_BLOCK_MS = 300; // a swipe swallows the click that follows it
const QUARTER_TURN_DEG = 90;

// The cube: the front face (the room on show) and the side face (the room turning in), over
// an invisible copy of every room's name in the same grid cell, so the cube is always the
// widest name wide and its faces turn about one depth.
export function roomCubeTemplate(roomNames) {
  const sizers = roomNames.map(name =>
    `<span class="room-cube__sizer" aria-hidden="true">${name}</span>`).join('');
  return `
      <span id="current-room-label" class="room-cube__face room-cube__front"></span>
      <span class="room-cube__face room-cube__side" aria-hidden="true"></span>
      ${sizers}`;
}

export default class RoomSwipe {

  // finds the cube's faces; see the top of this file
  constructor(anchorId, callbacks) {
    this.anchor = document.getElementById(anchorId);
    this.front = this.anchor.querySelector('.room-cube__front');
    this.side = this.anchor.querySelector('.room-cube__side');
    this.neighbourName = callbacks.neighbourName;
    this.onTurn = callbacks.onTurn;
    this.onRest = callbacks.onRest;
    this.drag = null;
    this.faceWidth = 1;
    // the room the side face shows: -1 the previous, 1 the next
    this.sideStep = 0;
    this.frameId = null;
    this.settleTimer = null;
    this.restTimer = null;
    this.turnPending = false;
    this.blockNextClick = false;
    this.clickBlockTimeout = null;
    this.anchor.style.setProperty('--cube-settle', `${SETTLE_MS}ms`);
    this.placeFaces(0);
  }

  // binds the cube's events
  activate() {
    this.anchor.addEventListener('pointerdown', this);
    this.anchor.addEventListener('pointermove', this);
    this.anchor.addEventListener('pointerup', this);
    this.anchor.addEventListener('pointercancel', this);
    // before the page's listener on document, so a swipe's click can be stopped
    this.anchor.addEventListener('click', this, true);
  }

  // unbinds the events and drops every timer
  destroy() {
    this.anchor.removeEventListener('pointerdown', this);
    this.anchor.removeEventListener('pointermove', this);
    this.anchor.removeEventListener('pointerup', this);
    this.anchor.removeEventListener('pointercancel', this);
    this.anchor.removeEventListener('click', this, true);
    if (this.frameId) cancelAnimationFrame(this.frameId);
    clearTimeout(this.settleTimer);
    clearTimeout(this.restTimer);
    clearTimeout(this.clickBlockTimeout);
    this.drag = null;
  }

  // routes the bound events
  handleEvent(event) {
    switch (event.type) {
      case 'pointerdown': this.onPointerDown(event); break;
      case 'pointermove': this.onPointerMove(event); break;
      case 'pointerup': this.onPointerUp(event); break;
      case 'pointercancel': this.onPointerCancel(event); break;
      case 'click': this.onClick(event); break;
    }
  }

  // A press: follows the pointer. A press while the cube settles is ignored, so a turn
  // always lands before the next starts.
  onPointerDown(event) {
    if (event.button !== 0 && event.pointerType === 'mouse') return;
    if (this.settleTimer) return;
    // the user is still on the selector, so the room is not settled on yet
    clearTimeout(this.restTimer);
    this.blockNextClick = false;
    clearTimeout(this.clickBlockTimeout);
    this.drag = {
      pointerId: event.pointerId,
      startX: event.clientX,
      startTime: performance.now(),
      deltaX: 0,
      progress: 0,
      moved: false
    };
    this.anchor.setPointerCapture(event.pointerId);
  }

  // A drag: turns the cube with the pointer, once per frame. A drag of a face's width to
  // the left is progress -1, to the right 1.
  onPointerMove(event) {
    if (!this.drag || event.pointerId !== this.drag.pointerId) return;
    const deltaX = event.clientX - this.drag.startX;
    if (!this.drag.moved && Math.abs(deltaX) <= DRAG_THRESHOLD_PX) return;
    if (!this.drag.moved) {
      this.drag.moved = true;
      removeDropdowns();
      this.measureFace();
      // the room may have changed since the side face was named (a turn, the room menu)
      this.sideStep = 0;
    }
    const share = deltaX / this.faceWidth;
    this.drag.deltaX = deltaX;
    this.drag.progress = Math.max(-1, Math.min(1, share));
    if (this.frameId) return;
    this.frameId = requestAnimationFrame(() => {
      this.frameId = null;
      if (this.drag) {
        this.placeFaces(this.drag.progress);
      }
    });
  }

  // A release: a tap is left to the click that follows; a swipe far or fast enough turns
  // the cube, a shorter one springs back, and either swallows its click.
  onPointerUp(event) {
    if (!this.drag || event.pointerId !== this.drag.pointerId) return;
    const ended = this.endDrag();
    if (!ended.moved) {
      this.armRest();
      return;
    }
    this.blockNextClick = true;
    this.clickBlockTimeout = setTimeout(() => {
      this.blockNextClick = false;
    }, CLICK_BLOCK_MS);
    const elapsedMs = Math.max(1, performance.now() - ended.startTime);
    const speed = Math.abs(ended.deltaX) / elapsedMs;
    const farEnough = Math.abs(ended.progress) >= TURN_SHARE;
    const flicked = speed >= FLICK_PX_PER_MS && ended.progress !== 0;
    let target = 0;
    if (farEnough || flicked) {
      target = Math.sign(ended.progress);
    }
    this.settle(target);
  }

  // the browser took the pointer (an upright scroll): springs back
  onPointerCancel(event) {
    if (!this.drag || event.pointerId !== this.drag.pointerId) return;
    this.endDrag();
    this.settle(0);
  }

  // a click: stopped when a swipe just ended, otherwise left for the room menu
  onClick(event) {
    if (!this.blockNextClick) return;
    event.preventDefault();
    event.stopPropagation();
    this.blockNextClick = false;
    clearTimeout(this.clickBlockTimeout);
  }

  // ends the drag and hands back what it was
  endDrag() {
    const ended = this.drag;
    this.drag = null;
    if (this.frameId) {
      cancelAnimationFrame(this.frameId);
      this.frameId = null;
    }
    return ended;
  }

  // Animates the cube to target (0 back, -1 / 1 a quarter turn) and, on a turn, hands it
  // on: a drag to the left (-1) brings in the next room (step 1). The faces then go back
  // to rest as onTurn() names the front face, in the same frame, so nothing jumps.
  settle(target) {
    this.anchor.classList.add('room-cube--settling');
    this.placeFaces(target);
    this.settleTimer = setTimeout(() => {
      this.settleTimer = null;
      this.anchor.classList.remove('room-cube--settling');
      this.placeFaces(0);
      if (target !== 0) {
        this.turnPending = true;
        const step = -target;
        this.onTurn(step);
      }
      this.armRest();
    }, SETTLE_MS);
  }

  // books onRest() for a turn not yet rested on
  armRest() {
    if (!this.turnPending) return;
    clearTimeout(this.restTimer);
    this.restTimer = setTimeout(() => {
      this.restTimer = null;
      this.turnPending = false;
      this.onRest();
    }, REST_MS);
  }

  // drops a rest still to come: the room was picked another way and read already
  cancelRest() {
    clearTimeout(this.restTimer);
    this.restTimer = null;
    this.turnPending = false;
  }

  // The face's width, and half of it as the depth the faces turn about. Measured as a
  // drag starts, so a new page width or font is taken up.
  measureFace() {
    const faceWidth = this.front.offsetWidth;
    this.faceWidth = Math.max(1, faceWidth);
    const depth = this.faceWidth / 2;
    this.anchor.style.setProperty('--cube-depth', `${depth}px`);
  }

  // Turns both faces to progress: the front face by progress quarter turns, and the side
  // face, carrying the room coming in, a quarter turn behind it on the side it comes from.
  // progress 0 shows the front face alone.
  placeFaces(progress) {
    if (progress !== 0) {
      const incomingStep = -Math.sign(progress);
      this.showSideRoom(incomingStep);
    }
    let sideOffset = 1;
    if (this.sideStep === -1) {
      sideOffset = -1;
    }
    const frontAngle = progress * QUARTER_TURN_DEG;
    const sideTurns = progress + sideOffset;
    const sideAngle = sideTurns * QUARTER_TURN_DEG;
    this.placeFace(this.front, frontAngle);
    this.placeFace(this.side, sideAngle);
  }

  // names the side face for the room step places away, when it shows another
  showSideRoom(step) {
    if (this.sideStep === step) return;
    this.sideStep = step;
    this.side.textContent = this.neighbourName(step);
  }

  // turns one face to angle degrees, darkening it as it turns away: 0 faces the user
  placeFace(face, angleDeg) {
    const angleRad = angleDeg * Math.PI / 180;
    const facing = Math.max(0, Math.cos(angleRad));
    face.style.setProperty('--face-angle', `${angleDeg}deg`);
    face.style.setProperty('--face-light', facing.toFixed(3));
  }
}
