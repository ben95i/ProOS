// Automate Website - camera window
//
// Plays a camera's live stream in the camera popup. The API relays the
// camera's RTSP feed as fragmented MP4 without re-encoding it (api/
// camstream.js), and this class hands it straight to the browser's own
// video decoder through Media Source Extensions -- no plugin, and no
// transcoding anywhere between the camera and the screen.
//
// While it loads, the first frame of the live stream is shown as a still
// under the "Loading..." message, and the window is sized to fit the visible
// part of the screen (on an iPhone, between Safari's bars).
//
// The API reports the rotation the camera asks for (X-Camera-Rotation) and
// the picture is turned to match with a CSS transform, which the browser's
// compositor does at no cost. A quarter turn also swaps the window's shape,
// so a portrait camera gets a portrait window.
//
// Kept light on purpose: the stream stops while the tab is hidden and only
// a few seconds of video are held in memory.
//
// Smoothness over delay: the video arrives a second at a time (the API
// sends whole groups of pictures) and over Wi-Fi it arrives unevenly, so
// playback is held TARGET_DELAY_S behind the newest frame as a cushion.
//   - It only starts, and only restarts after running dry, once the full
//     cushion is in: one longer pause instead of many small stutters.
//   - Drift is corrected by playing a few percent faster or slower, judged
//     on the lag averaged over several seconds so the once-a-second arrival
//     of new video does not keep changing the speed.
//   - A seek, which shows as a hitch, is only used when far behind.
//
// new CameraView(name) then .open() / .close() / .toggleFullscreen();
// .destroy() on page teardown. CameraView.POPUP_HTML is the popup's markup,
// placed by main.js with the rest of the page.

import { gridButtonTemplate } from './utils.js';

export default class CameraView {

  static STREAM_PATH = 'api/camera';

  static POPUP_ID = 'cam-popup';
  static STAGE_ID = 'cam-stage';
  static VIDEO_ID = 'cam-video';
  static STATUS_ID = 'cam-status';

  // the stage takes this while the picture is turned a quarter, which swaps
  // the video's width and height (camera.css)
  static QUARTER_CLASS = 'cam-stage--quarter';
  // the whole-window stand-in for browsers without the Fullscreen API
  // (iPhone Safari), put on the popup
  static MAXIMIZED_CLASS = 'cam-popup--max';

  static STATUS_LOADING = 'Loading...';
  static STATUS_RECONNECTING = 'Reconnecting...';
  static STATUS_UNSUPPORTED = 'This browser cannot play the camera stream';

  static RETRY_MS = 3000;          // pause before reconnecting
  static TICK_MS = 500;            // how often the delay is checked
  // How far behind the newest frame playback sits. Raise it if the picture
  // still stutters on a poor connection, lower it (not below 2) for less
  // delay -- the API delivers a second of video at a time.
  static TARGET_DELAY_S = 3;
  static DELAY_TOLERANCE_S = 1;    // averaged drift allowed before the speed changes
  static LAG_SMOOTHING = 0.1;      // weight of each new lag reading in the average
  static CATCH_UP_RATE = 1.05;     // speed while too far behind
  static SLOW_DOWN_RATE = 0.95;    // speed while too close to running dry
  static NORMAL_RATE = 1;
  static MAX_LAG_S = 10;           // further behind than this: seek forward
  static KEEP_BUFFER_S = 4;        // video kept behind the playhead
  static TRIM_STEP_S = 4;          // trim once this much more has built up
  static MAX_QUEUE_BYTES = 4 * 1024 * 1024; // unplayed data before giving up

  // clear space kept above and below the popup; the bottom one matches
  // .popup-win's bottom: 10px in main.css
  static VIEWPORT_MARGIN_PX = 10;
  static MIN_STAGE_HEIGHT_PX = 120;

  // the shape shown until the first frame says otherwise
  static DEFAULT_WIDTH = 16;
  static DEFAULT_HEIGHT = 9;

  static DEGREES_PER_TURN = 360;
  static DEGREES_PER_QUARTER = 90;

  static POPUP_HTML = `
<div class="popup-win" id="cam-popup" data-app-injected>
  <div class="popup-ctn">
    <div id="cam-stage">
      <video id="cam-video" muted autoplay playsinline disablepictureinpicture></video>
      <div id="cam-status"></div>
    </div>
    <div class="popup-actions">
      ${gridButtonTemplate({
        buttonId: 'btn-cam-close',
        icon: 'fad fa-times-octagon',
        buttonClass: 'button_left_txtwin'
      })}
      ${gridButtonTemplate({
        buttonId: 'btn-cam-fullscreen',
        label: 'Fullscreen',
        buttonClass: 'button_right_txtwin'
      })}
    </div>
  </div>
</div>`;

  constructor(cameraName) {
    this.cameraName = cameraName;
    this.isOpen = false;
    // bumped every time a stream starts or stops, so a reply, a chunk or a
    // buffer event from a stream that has since been stopped is ignored
    this.session = 0;
    this.abortController = null;
    this.mediaSource = null;
    this.sourceBuffer = null;
    this.objectURL = null;
    this.queue = [];
    this.queuedBytes = 0;
    this.hasStarted = false;
    this.isBuffering = false;  // paused until the cushion has filled
    this.averageLag = null;
    this.rotation = 0;
    this.retryTimer = null;
    this.tickTimer = null;
    this.onVisibility = () => this.visibilityChanged();
    this.onMetadata = () => this.fitToVideo();
    this.onPlaying = () => this.startedPlaying();
    this.onViewportResize = () => this.fitToViewport();
    this.onWaiting = () => this.ranDry();
    this.onStageClick = () => this.restoreFromMaximized();
  }

  //// elements ////

  popup() {
    return document.getElementById(CameraView.POPUP_ID);
  }

  stage() {
    return document.getElementById(CameraView.STAGE_ID);
  }

  video() {
    return document.getElementById(CameraView.VIDEO_ID);
  }

  setStatus(text) {
    const status = document.getElementById(CameraView.STATUS_ID);
    if (status) {
      status.textContent = text;
    }
  }

  streamURL() {
    const name = encodeURIComponent(this.cameraName);
    return `${location.protocol}//${location.hostname}/${CameraView.STREAM_PATH}/${name}`;
  }

  //// the window ////

  open() {
    const popup = this.popup();
    const video = this.video();
    if (this.isOpen || !popup || !video) {
      return;
    }
    this.isOpen = true;
    popup.style.display = 'block';
    this.fitToViewport();
    window.addEventListener('resize', this.onViewportResize);
    if (window.visualViewport) {
      window.visualViewport.addEventListener('resize', this.onViewportResize);
    }
    video.muted = true; // autoplay is only allowed muted
    video.addEventListener('loadedmetadata', this.onMetadata);
    video.addEventListener('playing', this.onPlaying);
    video.addEventListener('waiting', this.onWaiting);
    this.stage().addEventListener('click', this.onStageClick);
    document.addEventListener('visibilitychange', this.onVisibility);
    this.start();
  }

  close() {
    if (!this.isOpen) {
      return;
    }
    this.isOpen = false;
    this.leaveFullscreen();
    this.stop();
    this.setStatus('');
    const video = this.video();
    if (video) {
      video.removeEventListener('loadedmetadata', this.onMetadata);
      video.removeEventListener('playing', this.onPlaying);
      video.removeEventListener('waiting', this.onWaiting);
    }
    const stage = this.stage();
    if (stage) {
      stage.removeEventListener('click', this.onStageClick);
    }
    document.removeEventListener('visibilitychange', this.onVisibility);
    window.removeEventListener('resize', this.onViewportResize);
    if (window.visualViewport) {
      window.visualViewport.removeEventListener('resize', this.onViewportResize);
    }
    const popup = this.popup();
    if (popup) {
      popup.style.display = 'none';
    }
  }

  destroy() {
    this.close();
  }

  // Nothing is decoded or downloaded while the tab is out of sight; the
  // stream picks up again, at live, when it comes back.
  visibilityChanged() {
    if (!this.isOpen) {
      return;
    }
    if (document.hidden) {
      this.stop();
      return;
    }
    this.start();
  }

  //// the stream ////

  async start() {
    this.stop();
    const session = ++this.session;
    this.setStatus(CameraView.STATUS_LOADING);
    // Safari on iPhone only has ManagedMediaSource
    const MediaSourceType = window.ManagedMediaSource || window.MediaSource;
    if (!MediaSourceType) {
      this.setStatus(CameraView.STATUS_UNSUPPORTED);
      return;
    }
    this.abortController = new AbortController();
    try {
      const response = await fetch(this.streamURL(), {
        cache: 'no-store',
        signal: this.abortController.signal
      });
      if (session !== this.session) {
        return;
      }
      if (!response.ok || !response.body) {
        throw new Error(`status ${response.status}`);
      }
      const codec = response.headers.get('X-Camera-Codec');
      const mimeType = `video/mp4; codecs="${codec}"`;
      if (!codec || !MediaSourceType.isTypeSupported(mimeType)) {
        // retrying would only get the same answer
        this.stop();
        this.setStatus(`${CameraView.STATUS_UNSUPPORTED} (${codec})`);
        return;
      }
      this.applyRotation(response.headers.get('X-Camera-Rotation'));
      await this.openMediaSource(MediaSourceType, mimeType, session);
      if (session !== this.session) {
        return;
      }
      this.tickTimer = setInterval(() => this.keepLive(), CameraView.TICK_MS);
      await this.pump(response.body.getReader(), session);
    } catch (err) {
      if (session === this.session) {
        this.retryLater(err);
      }
    }
  }

  // Tears the stream down completely: the download, the MediaSource and
  // the video's decoder, so a closed window holds no memory at all.
  stop() {
    this.session += 1;
    clearTimeout(this.retryTimer);
    this.retryTimer = null;
    clearInterval(this.tickTimer);
    this.tickTimer = null;
    if (this.abortController) {
      this.abortController.abort();
      this.abortController = null;
    }
    this.queue = [];
    this.queuedBytes = 0;
    this.hasStarted = false;
    this.isBuffering = false;
    this.averageLag = null;
    if (this.mediaSource && this.mediaSource.readyState === 'open') {
      try {
        this.mediaSource.endOfStream();
      } catch (err) {
        // a buffer still updating refuses; the source goes with the video
      }
    }
    this.mediaSource = null;
    this.sourceBuffer = null;
    const video = this.video();
    if (video) {
      video.pause();
      video.removeAttribute('src');
      video.load();
    }
    if (this.objectURL) {
      URL.revokeObjectURL(this.objectURL);
      this.objectURL = null;
    }
  }

  // The stream broke or ended (the camera restarted, the API restarted,
  // the network dropped): start again shortly while the window is open.
  retryLater(err) {
    const reason = (err && err.message) ? err.message : 'stream stopped';
    console.log(`[CameraView] ${this.cameraName}: ${reason}, reconnecting`);
    this.stop();
    if (!this.isOpen || document.hidden) {
      return;
    }
    this.setStatus(CameraView.STATUS_RECONNECTING);
    this.retryTimer = setTimeout(() => this.start(), CameraView.RETRY_MS);
  }

  openMediaSource(MediaSourceType, mimeType, session) {
    return new Promise((resolve, reject) => {
      const mediaSource = new MediaSourceType();
      this.mediaSource = mediaSource;
      mediaSource.addEventListener('sourceopen', () => {
        if (session !== this.session) {
          resolve();
          return;
        }
        try {
          const sourceBuffer = mediaSource.addSourceBuffer(mimeType);
          sourceBuffer.mode = 'segments'; // place frames by their own timestamps
          sourceBuffer.addEventListener('updateend', () => this.bufferUpdated(session));
          this.sourceBuffer = sourceBuffer;
          resolve();
        } catch (err) {
          reject(err);
        }
      }, { once: true });
      const video = this.video();
      // ManagedMediaSource only plays with remote playback (AirPlay) off
      video.disableRemotePlayback = true;
      this.objectURL = URL.createObjectURL(mediaSource);
      video.src = this.objectURL;
    });
  }

  // Reads the response as it arrives and queues it for the SourceBuffer.
  // Only returns by throwing: an ended response is a dropped stream too.
  async pump(reader, session) {
    for (;;) {
      const { value, done } = await reader.read();
      if (session !== this.session) {
        return;
      }
      if (done) {
        throw new Error('stream ended');
      }
      this.queue.push(value);
      this.queuedBytes += value.byteLength;
      if (this.queuedBytes > CameraView.MAX_QUEUE_BYTES) {
        throw new Error('decoder fell behind');
      }
      this.appendQueued();
    }
  }

  // Hands everything queued to the SourceBuffer in one append. Chunks that
  // arrive while it is busy wait for its updateend.
  appendQueued() {
    const sourceBuffer = this.sourceBuffer;
    if (!sourceBuffer || sourceBuffer.updating || this.queue.length === 0) {
      return;
    }
    let data = this.queue[0];
    if (this.queue.length > 1) {
      data = new Uint8Array(this.queuedBytes);
      let offset = 0;
      for (const chunk of this.queue) {
        data.set(chunk, offset);
        offset += chunk.byteLength;
      }
    }
    try {
      sourceBuffer.appendBuffer(data);
      this.queue = [];
      this.queuedBytes = 0;
    } catch (err) {
      if (err.name === 'QuotaExceededError') {
        // the browser's buffer is full: keep the data, free some, try again
        this.queue = [data];
        this.trimBuffer(0);
        return;
      }
      this.retryLater(err);
    }
  }

  // After every append or trim: on the first video, put its first frame on
  // screen as a still and wait for the cushion to fill (keepLive starts
  // playback); then trim what has been watched and append whatever came in
  // meanwhile.
  bufferUpdated(session) {
    if (session !== this.session || !this.sourceBuffer) {
      return;
    }
    if (!this.hasStarted && this.sourceBuffer.buffered.length > 0) {
      this.hasStarted = true;
      this.isBuffering = true;
      this.jumpToLive();
      // A paused video does not always paint the frame it was moved to
      // (Safari keeps it blank), so it is set playing and stopped again by
      // startedPlaying() the moment the frame is up.
      this.playVideo();
    }
    if (this.trimBuffer(CameraView.KEEP_BUFFER_S)) {
      return; // a remove is running; its updateend comes back here
    }
    this.appendQueued();
  }

  // Drops video more than keepSeconds behind the playhead. Answers true
  // when a remove was started.
  trimBuffer(keepSeconds) {
    const sourceBuffer = this.sourceBuffer;
    const video = this.video();
    if (!sourceBuffer || sourceBuffer.updating || sourceBuffer.buffered.length === 0) {
      return false;
    }
    const oldestBuffered = sourceBuffer.buffered.start(0);
    const keepFrom = video.currentTime - keepSeconds;
    const trimThreshold = keepFrom - CameraView.TRIM_STEP_S;
    if (keepSeconds > 0 && oldestBuffered > trimThreshold) {
      return false;
    }
    if (keepFrom <= oldestBuffered) {
      return false;
    }
    sourceBuffer.remove(oldestBuffered, keepFrom);
    return true;
  }

  // Puts the playhead the target delay behind the newest frame, or as far
  // back as there is video when there is less than that.
  jumpToLive() {
    const sourceBuffer = this.sourceBuffer;
    const video = this.video();
    const lastRange = sourceBuffer.buffered.length - 1;
    const rangeStart = sourceBuffer.buffered.start(lastRange);
    const liveEdge = sourceBuffer.buffered.end(lastRange);
    const target = liveEdge - CameraView.TARGET_DELAY_S;
    video.currentTime = Math.max(rangeStart, target);
    this.averageLag = null;
    this.setPlaybackRate(CameraView.NORMAL_RATE);
  }

  // The video began to play. While still filling the cushion this was only
  // to paint the first frame: it is stopped there, a still under the
  // loading message. Otherwise it is the real start, and the message goes.
  startedPlaying() {
    if (this.isBuffering) {
      this.video().pause();
      return;
    }
    this.setStatus('');
  }

  // Playback caught up with the newest frame and stopped. Rather than let
  // the browser resume on every frame that trickles in -- a stutter each
  // time -- it pauses until the whole cushion is back.
  ranDry() {
    if (!this.hasStarted || this.isBuffering) {
      return;
    }
    this.isBuffering = true;
    this.video().pause();
  }

  // Runs every TICK_MS: starts playback once the cushion is in, then keeps
  // it near TARGET_DELAY_S behind live with small speed changes. Seeks only
  // when far behind, or when stuck before a gap the server left by skipping
  // frames a slow connection could not take.
  keepLive() {
    const sourceBuffer = this.sourceBuffer;
    const video = this.video();
    if (!this.hasStarted || !sourceBuffer || sourceBuffer.buffered.length === 0) {
      return;
    }
    const lastRange = sourceBuffer.buffered.length - 1;
    const rangeStart = sourceBuffer.buffered.start(lastRange);
    const liveEdge = sourceBuffer.buffered.end(lastRange);
    const lag = liveEdge - video.currentTime;
    const isBeforeNewestRange = video.currentTime < rangeStart;
    if (lag > CameraView.MAX_LAG_S || isBeforeNewestRange) {
      this.jumpToLive();
      return;
    }
    if (this.isBuffering) {
      if (lag >= CameraView.TARGET_DELAY_S) {
        this.isBuffering = false;
        this.playVideo();
      }
      return;
    }
    this.averageLag = this.smoothLag(lag);
    this.setPlaybackRate(this.rateForLag(this.averageLag, video.playbackRate));
    if (video.paused) {
      this.playVideo();
    }
  }

  // Running average of the lag. Video lands a second at a time, so a single
  // reading swings by that much depending on when it is taken.
  smoothLag(lag) {
    if (this.averageLag === null) {
      return lag;
    }
    const change = lag - this.averageLag;
    return this.averageLag + change * CameraView.LAG_SMOOTHING;
  }

  // The speed to play at for this much lag. A change of speed holds until
  // the lag is back on target, not merely back inside the tolerance, so
  // the speed does not flick back and forth at the edge of it.
  rateForLag(lag, currentRate) {
    const isTooFarBehind = lag > CameraView.TARGET_DELAY_S + CameraView.DELAY_TOLERANCE_S;
    const isTooClose = lag < CameraView.TARGET_DELAY_S - CameraView.DELAY_TOLERANCE_S;
    if (isTooFarBehind) {
      return CameraView.CATCH_UP_RATE;
    }
    if (isTooClose) {
      return CameraView.SLOW_DOWN_RATE;
    }
    const isCatchingUp = currentRate > CameraView.NORMAL_RATE;
    const isSlowingDown = currentRate < CameraView.NORMAL_RATE;
    const hasCaughtUp = isCatchingUp && lag <= CameraView.TARGET_DELAY_S;
    const hasRefilled = isSlowingDown && lag >= CameraView.TARGET_DELAY_S;
    if (hasCaughtUp || hasRefilled) {
      return CameraView.NORMAL_RATE;
    }
    return currentRate;
  }

  setPlaybackRate(rate) {
    const video = this.video();
    if (video && video.playbackRate !== rate) {
      video.playbackRate = rate;
    }
  }

  playVideo() {
    const playing = this.video().play();
    if (playing && typeof playing.catch === 'function') {
      playing.catch(() => {
        // refused (no user gesture yet on some browsers): the next tick
        // tries again
      });
    }
  }

  //// rotation and shape ////

  // Turns the picture the way the camera asked: 0, 90, 180 or 270 degrees
  // clockwise, anything else rounded to the nearest quarter.
  applyRotation(headerValue) {
    const degrees = Number.parseInt(headerValue, 10);
    let rotation = 0;
    if (Number.isFinite(degrees)) {
      const quarterTurns = Math.round(degrees / CameraView.DEGREES_PER_QUARTER);
      const wholeDegrees = quarterTurns * CameraView.DEGREES_PER_QUARTER;
      const turnRemainder = wholeDegrees % CameraView.DEGREES_PER_TURN;
      rotation = (turnRemainder + CameraView.DEGREES_PER_TURN) % CameraView.DEGREES_PER_TURN;
    }
    this.rotation = rotation;
    const stage = this.stage();
    stage.style.setProperty('--cam-rotation', `${rotation}deg`);
    stage.classList.toggle(CameraView.QUARTER_CLASS, this.isQuarterTurn());
    this.fitToVideo();
  }

  isQuarterTurn() {
    const quarterTurns = this.rotation / CameraView.DEGREES_PER_QUARTER;
    return quarterTurns % 2 === 1;
  }

  // Shapes the window to the picture as it will be seen: the video's own
  // size once it is known, with width and height swapped for a quarter turn.
  fitToVideo() {
    const video = this.video();
    const stage = this.stage();
    if (!video || !stage) {
      return;
    }
    let width = video.videoWidth || CameraView.DEFAULT_WIDTH;
    let height = video.videoHeight || CameraView.DEFAULT_HEIGHT;
    if (this.isQuarterTurn()) {
      [width, height] = [height, width];
    }
    stage.style.setProperty('--cam-aspect', `${width} / ${height}`);
  }

  // Sizes the picture so the whole popup -- picture, frame and buttons --
  // fits in the part of the screen that can be seen. The visual viewport is
  // used where there is one: on a phone it is the area between the
  // browser's bars, where the CSS viewport units also count what is behind
  // them. Runs on open and whenever that area changes size (rotation, the
  // bars showing or hiding).
  fitToViewport() {
    const popup = this.popup();
    const stage = this.stage();
    if (!this.isOpen || !popup || !stage) {
      return;
    }
    const viewport = window.visualViewport;
    const viewportHeight = viewport ? viewport.height : window.innerHeight;
    // everything in the popup that is not the picture: its border, padding
    // and the button row
    const popupChrome = popup.offsetHeight - stage.offsetHeight;
    const margins = 2 * CameraView.VIEWPORT_MARGIN_PX;
    const availableHeight = viewportHeight - popupChrome - margins;
    const stageMaxHeight = Math.max(CameraView.MIN_STAGE_HEIGHT_PX, Math.floor(availableHeight));
    stage.style.setProperty('--cam-max-height', `${stageMaxHeight}px`);
  }

  //// fullscreen ////

  fullscreenElement() {
    return document.fullscreenElement || document.webkitFullscreenElement || null;
  }

  // Fills the screen with the picture (rotated as it is in the window), or
  // goes back. Where the Fullscreen API is missing the popup fills the
  // browser window instead; a tap on the picture brings it back.
  toggleFullscreen() {
    if (!this.isOpen) {
      return;
    }
    const popup = this.popup();
    if (this.fullscreenElement() || popup.classList.contains(CameraView.MAXIMIZED_CLASS)) {
      this.leaveFullscreen();
      return;
    }
    const stage = this.stage();
    const requestFullscreen = stage.requestFullscreen || stage.webkitRequestFullscreen;
    if (!requestFullscreen) {
      popup.classList.add(CameraView.MAXIMIZED_CLASS);
      return;
    }
    Promise.resolve(requestFullscreen.call(stage)).catch(() => {
      popup.classList.add(CameraView.MAXIMIZED_CLASS);
    });
  }

  leaveFullscreen() {
    const popup = this.popup();
    if (popup) {
      popup.classList.remove(CameraView.MAXIMIZED_CLASS);
    }
    if (!this.fullscreenElement()) {
      return;
    }
    const exitFullscreen = document.exitFullscreen || document.webkitExitFullscreen;
    if (exitFullscreen) {
      Promise.resolve(exitFullscreen.call(document)).catch(() => {});
    }
  }

  restoreFromMaximized() {
    const popup = this.popup();
    if (popup && popup.classList.contains(CameraView.MAXIMIZED_CLASS)) {
      popup.classList.remove(CameraView.MAXIMIZED_CLASS);
    }
  }
}
