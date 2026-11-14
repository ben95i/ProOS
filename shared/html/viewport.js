// Automate - page scale on phones and tablets. A mobile browser lays the page out at the
// width its viewport tag names, then scales that width to the screen, so the width
// chosen here is the page's scale. A desktop browser ignores the tag and shows the page
// at its own size.
// by Ben Provenzano III

// The narrowest the page is laid out, in CSS px: the three-button groups (465px) and a
// margin. A screen with fewer points across (a phone) shows this width edge to edge when
// held upright. The viewport tag in index.html names the same width, so an upright phone
// starts at its scale.
const MOBILE_PAGE_WIDTH = 490;
// A screen with at least that many points across (a tablet): one CSS px per point.
const NATIVE_VIEWPORT = 'width=device-width, initial-scale=1, shrink-to-fit=no';

// True while the device is held sideways. iOS keeps screen.width and screen.height as
// they are upright, so the screen's own orientation is asked.
function deviceIsLandscape() {
  const screenOrientation = window.screen.orientation;
  if (screenOrientation && typeof screenOrientation.type === 'string') {
    return screenOrientation.type.startsWith('landscape');
  }
  return window.matchMedia('(orientation: landscape)').matches;
}

// The viewport tag's content for this screen, as it is held now. The scale is the
// screen's short side over MOBILE_PAGE_WIDTH, and stays the same when the device turns.
// A 390 x 844 phone: 'width=490, …' upright, 'width=1060, …' sideways.
function viewportContent() {
  const shortSide = Math.min(window.screen.width, window.screen.height);
  const longSide = Math.max(window.screen.width, window.screen.height);
  if (shortSide >= MOBILE_PAGE_WIDTH) {
    return NATIVE_VIEWPORT;
  }
  const pageScale = shortSide / MOBILE_PAGE_WIDTH;
  let screenWidth = shortSide;
  if (deviceIsLandscape()) {
    screenWidth = longSide;
  }
  const layoutWidth = Math.round(screenWidth / pageScale);
  return `width=${layoutWidth}, shrink-to-fit=no`;
}

// Writes the viewport tag when its content has to change. The content comes from the
// screen alone, never from the page's own size, so the resize it causes changes nothing.
function applyViewport() {
  const viewportTag = document.querySelector('meta[name="viewport"]');
  if (!viewportTag) {
    return;
  }
  const content = viewportContent();
  if (viewportTag.getAttribute('content') !== content) {
    viewportTag.setAttribute('content', content);
  }
}

// Sets the page's scale for this device and keeps it as the device is turned. Called
// once, before the page is drawn.
export function installViewportScale() {
  applyViewport();
  window.addEventListener('orientationchange', applyViewport);
  window.addEventListener('resize', applyViewport);
}
