# Automate Home Automation Website

Oct 11, 2026 · @Ben

## Overview

Automate is a one-page home automation site that runs the apartment's lights, audio, PCs, LEDs and doorway camera from a phone or browser. It is plain JavaScript ES modules with no framework: `index.html` loads `main.js`, which draws the whole page from template literals and imports one module per feature.

One codebase serves three device models. `main.js` reads the model from the page's `<title>` at load and draws only that model's markup; markup for the other models never reaches the page.

| Model | Detected by | What it draws |
| --- | --- | --- |
| Server | title holds neither "pi" nor "led" | Full page: top nav (main menu, bookmarks, room status, room selector, weather, stats), Pixels, APT and room groups, server options |
| LED Pi | title holds "led" | Pixels group, LED text scroller, Pi main menu, Pi options, log window |
| Pi | title holds "pi" | Pi main menu, Pi options, log window |

lighttpd serves the static files and reverse-proxies `/api` to the node API on 127.0.0.1:3000 (and `/cam1` to the camera host). Every button press becomes a `GET /api?action=…&arg=…&var=…`, and menu and list files are read and written through `/api/read` and `/api/write`.

## Architecture

The browser never talks to a device: every press is a request to the node API, and `executeMain()` in `controller.js` dispatches it to the room or device it names.

&#91;embedded content: request path · browser to devices\]

Room transmitters answer the Automate API on port 80 (`callApi`), Pico controllers answer `?<cmd>,,<value>~` on port 4000 (`callPico`), and every call gives up after 3 s. A room status (`lrstatus`, `brstatus`, `bthrstatus`) reads all of a room's devices at once and answers `{ roomState, devices }`.

## How the page is built

The page is drawn once from template functions in `main.js`, and every button reaches its handler through one click listener on `document`.

1. **Load.** On `DOMContentLoaded`, `loadPage()` reads the model and `renderPage()` injects the markup from `buildPageHTML()`. Every injected node carries `data-app-injected`, so `removeInjectedPage()` can take the whole page down before a re-render.
2. **Features.** `loadFeatures()` starts the server-only parts (the last room from `localStorage` key `ctls-mode`, `RoomStateManager`, `WeatherWidget`), then the main menu and theme colour, the stars background and the activity timer.
3. **Buttons.** Each template is followed by `registerListeners({ id: handler })`. One delegated listener walks up from the clicked element to the first id in that map, so markup drawn later (room grids, server panels) needs no binding.
4. **Every click.** `handleClicks()` restarts the 50 s activity timer (on expiry the stars and the room status refresh pause) and closes open menus unless the click was on a `.button`, a bookmark or a `.nohide` element.
5. **Teardown.** `removeDropdowns()` closes menus and destroys their toggle switches; `App.destroyAll()` on `beforeunload` and `destroyPageServices()` end every running service.

Shared state lives on `window.App`: the service instances (room status, stars, lists, bookmarks, camera, colour prompt, bathroom settings), the drawn menu rows, the open toggle switches and the current room.

### Dynamic menus

Most dropdowns are filled from text rows, one per line, as `HOST|TYPE|NAME`; a row that sends a command carries `cmd~<host>~<command>` in its first field. `drawMenu(data, menuId, { TwoColumns, MenuScroll })` turns the rows into links. Each request stamps a token (`beginMenuRequest()`), and a reply that lands after another menu was opened is dropped.

`openDeviceMenu(config)` is the common path for a device button: open the dropdown, show the button spinner, send one command, build rows from the reply, draw.

| Row type | Draws | Sends |
| --- | --- | --- |
| `grnind` · `blkind` · `redind` · `ylwind` · `noind` | Label with a green, black, red or yellow dot, or none | The row's command on click, for `cmd~` rows |
| `toggle` | `ToggleSwitch` from the device's 1 / 0 | `<command>on` / `<command>off` |
| `slider` | 0–255 slider | `<command>_set_<n>` while sliding, 50 ms throttle |
| `slidercust` | 1–255 slider | As `slider` |
| `sliderthr` | 0–255 slider for one-request-at-a-time devices | While sliding, 250 ms throttle; the release value always sent last |
| `on` · `off` · `pause` | Label with an icon | The row's command |
| `chk` | Switch for a browser-side setting (stars animation) | Nothing; saved in `localStorage` |
| `room` | A room name | Switches room and restarts the room status |
| `subs` | Subwoofer mode, red when on | Turns the volume buttons into subwoofer controls |
| `link` | A URL | Opens it |
| `ledeffectmenu` · `relaxmenu` · `relaxactive` · `relaxstop` | LED effects, relax sounds | The effect or sound named |

## The page

The server page is a top bar over three outlined button groups: Pixels and APT in the left column, the current room on the right. In portrait the room group sits under the others; under 700 px of height it stands beside them, both columns ending level.

### Top bar

| Item | What it does |
| --- | --- |
| Main menu (list icon) | About window, Colors (theme colour picker), stars animation switch, then the URL rows of the `mainmenu` file. The Pi models get About, Colors, stars, System (log window) and Home. |
| Bookmarks (star) | Bookmarks menu with search, add, edit and drag re-order, kept in a file under its own edit lock |
| Room status light | Polls `<room>status` every 10 s with a countdown ring; hatched once the readings go stale after 50 s idle, red on an error. Tapped, it lists each device with a green, black or red dot. |
| Room selector (centre) | The current room's icon and name; opens a menu of the other rooms |
| Weather | The weather widget and its menu |
| Stats (network icon) | Runs the `status` command and shows its rows |

### Pixels

- **Colors**: colour prompt for the LED pixels (`leds hex …`).
- **Control**: Off and Pause, plus a brightness slider for the Fadecandy LEDs.
- **Animated**: two-column menu of LED effects from the `ledeffectmenu` file; its TextScroll entry opens the LED text scroller (colour, font, speed).

### APT

| Row | Buttons |
| --- | --- |
| 1 | **Room** on / off (`<room>on`, `<room>off` for the room on screen) · **Lights** on / off (`lightson`, `lightsoff`) · **Power** all on / off (`allon`, `alloff`) |
| 2 | **YouTube** (link) · **Temperature / humidity** (thermometers from `roomth`, refresh) · **Media** (link) |
| 3 | **Doorway** (live camera window, full screen) · **Server** (log window + server options) · **Lists** (lists window) |

The Server log window arms one command at a time from three option panels (main, backup, regions) and sends it with the send button; password commands go through a password prompt first. The panels cover AFP, CIFS, Xana, UniFi, the router and network, backups and snapshots, archive and RAM disk regions, uploads and clearing the log.

### Room group

The third group's legend shows the current room's icon and name, and its grid is the room's own template (next section). Switching rooms closes any open menu, swaps the grid, saves the choice and restarts the room status.

## Rooms and devices

Three rooms are defined in `ROOM_CONFIGS` in `main.js`, each with a key, a name, an icon, a volume target and a grid template. The key names the room's API commands: `<key>status`, `<key>on`, `<key>off`, and device commands go through `<key>xmit`.

| Room | Key | Volume target | Room status reads |
| --- | --- | --- | --- |
| Living Room | `lr` | `lrxmit`, 0–100 | Main Lamp, Backlight, Desktop PC, Stereo System |
| Bedroom | `br` | `brpi`, 0–192 shown as % | Desk Lamp, Bed Lights, RetroPi TV, Vintage Macs, Bedroom TV, Bedroom PC, Audio System |
| Bathroom | `bthr` | none | Bathroom Light |

### Living Room

| Button | Opens | Device / command |
| --- | --- | --- |
| Lamp | On / off switch + brightness slider | Pico window lamp `wl1_dual` (192.168.1.18:4000) |
| Desktop | On / off switch | `wkststate`: ping, then the PC power relay |
| Ceiling | On / off switch | `clr1` |
| Volume − · mute · + | Volume popup | `voldwn`, `volmute`, `volup` on the stereo amp |
| −5% · +5% | Volume popup | `voldwnc`, `volupc` |
| Stereo | On / off switch | `hifistate` |
| Volume | Volume popup | `voldisplay` |
| Inputs | Input list, live one green | `curinput`: Auto, USB, Phono, Optical, Coaxial, Aux |
| More… | Subs Gain, Low Cut, vLimiter | `curhpfvollim`, `togglehpf`, `vlimit`; Subs Gain turns the volume buttons red and sends `sub…` |

### Bedroom

| Button | Opens | Device / command |
| --- | --- | --- |
| Bed | Adjust (up / down window), Colors, Cycle, Sweep, Rainbow, brightness | `bed_leds…` and `bed_adj…` codes on the bedroom transmitter |
| Relax | Relax sounds, the playing one marked | `relaxmenu`, `relax`, `relaxstop` on the Bedroom Pi |
| Dresser | On / off switch | `brlamp1` |
| Volume − · mute · + | Volume popup | `vol…` on the Bedroom Pi amp |
| TV | On / off switch | `brtv` |
| Audio | On / off switch | `ampstate` on the Bedroom Pi |
| PC | On / off switch | `brpc`: ping, wake-on-LAN, sleep |
| Macs | On / off switch | `brmacs` |
| Inputs | Input list, live one green | `inputstate`: USB, Coaxial, Optical, Aux |
| RetroPi | On / off switch | `retropi` |

### Bathroom

| Button | Opens | Device / command |
| --- | --- | --- |
| Brightness | Slider, 0–255 | `bthr-light1`, `bthr-light1_set_<n>` |
| Mode | Always On / Auto / Off, active one green | `bthr-light1_mode`, `bthr-light1_mode_set_<0–2>` |
| Settings | Motion sensor settings window | See *Bathroom light* |

The bathroom is left out of `lightson`, `lightsoff`, `allon` and `alloff`; APT's Room on / off still reaches it as `bthron` / `bthroff` while it is the room on screen.

## Bathroom light

The bathroom's one device is the BathroomLight, a Pico W on `bathroomlight.home:4000` driving a PWM LED and a C4002 mmWave motion sensor. Motion fades it in, and it fades off after the off delay with nobody detected. All of its controls live in `main.js` beside the other rooms.

- **Brightness** reads `bthr-light1`, 1–255: the level the light is at (or fading to) while it is on, and the level it will come on at while it is off. The slider is a `sliderthr` row, so the light gets at most one level every 250 ms and the release value always lands last.
- **Mode** reads `bthr-light1_mode` and marks Always On (2), Auto (1) or Off (0) green; a row sends `bthr-light1_mode_set_<n>`.
- **Room status** reads `bthr-light1_state`, 1 or 0, which reports where a fade is heading from the moment it starts.

### Settings window

`bathroomSettingsPrompt()` opens a centred window filled from one `bthr-light1_config` read (`min~max~presence~timeout~mode`). Each change is sent on release and confirmed in the title for 5 s ("Off delay: 6 min 8 s"); errors show there in red, and "Light offline" stays while the settings are greyed out.

| Setting | Control | Range | Command |
| --- | --- | --- | --- |
| Nearest detection | Slider, held below the farthest | 0–1100 cm | `bthr-light1_rangemin_set_<n>` |
| Farthest detection | Slider, held above the nearest | 0–1100 cm | `bthr-light1_rangemax_set_<n>` |
| Presence keeps light on | Switch | 0 / 1 | `bthr-light1_presence_set_<n>` |
| Off delay | Slider on a cube curve (about 1 s steps at the short end) | 6 s – 1 h | `bthr-light1_timeout_set_<n>` |

The Log and Sensor buttons turn the window into a log box twice as wide (680 px), filled from `bthr-light1_log` or `bthr-light1_sensor`, with Back and Close under it. Every request from the window waits for the one before it (`bthrRequest()`), because the Pico serves one request at a time.

### API commands

`bthrXmit()` in `controller.js` maps each command to a Pico call `http://bathroomlight.home:4000/api?<cmd>,,<value>~`. The log and status replies keep their line breaks; the rest are single values.

| Website command | Pico call | Reply |
| --- | --- | --- |
| `bthr-light1` | 2, 2 | Brightness 1–255 |
| `bthr-light1on` · `off` | 2, 1 · 2, 0 | Level the fade ends at |
| `bthr-light1_state` | 2, 4 | 1 on, 0 off |
| `bthr-light1_motion` | 2, 3 | 0 nobody, 1 still, 2 moving |
| `bthr-light1_set_<n>` | 3, n | Level set, or the kept on level |
| `bthr-light1_config` | 8, 0 | `min~max~presence~timeout~mode` |
| `bthr-light1_rangemin` · `rangemax` · `presence` · `timeout` · `mode` | 8, 4 · 5 · 6 · 7 · 13 | One setting |
| …`_set_<n>` of those | 4 · 5 · 6 · 7 · 13, n | The new value |
| `bthr-light1_reset` | 12, 1 | Defaults, mode back to Auto |
| `bthr-light1_log` · `_log_set_<n>` | 9, 0 · 9, n | Rolling log, all or newest n lines |
| `bthr-light1_status` · `_sensor` | 10, 0 · 10, 1 | Status text, plus the sensor's stored settings |

Errors come back as text with HTTP 200: a reply starting `commonAPIRequest exception` means the light is offline, `commonAPIRequest: invalid response` means it refused the value, and `bthrXmit:` or `executeMain:` means a bad command.

## Shared utilities and components

Each feature larger than a room button is a class in its own module, and anything two modules need lives in `utils.js`.

| Module | Class | What it does |
| --- | --- | --- |
| `mainmenu.js` | `MainMenu` | Main menus, theme colour (live while picking, kept on close), light-theme switch, About window |
| `roomstate.js` | `RoomStateManager` | Room status light, 10 s refresh, device dropdown |
| `colorprompt.js` | `ColorPrompt` | Colour window: saturation / value map, hue slider, palette swatches with names, hex box |
| `switch.js` | `ToggleSwitch` | Draggable on / off switch used in menus and windows |
| `stars.js` | `AnimatedBackground` | Star canvas behind the page, paused when idle |
| `weather.js` | `WeatherWidget` | Weather in the top bar and its menu |
| `bookmarks.js` | `Bookmarks` | Bookmarks menu, add / edit window, file and lock |
| `lists.js` | `Lists`, `ListStore` | Lists window: lists and items, drag re-order, multi-select delete, completed toggles, per-list encryption, edit lock, connection status |
| `upload.js` | `Uploader` | Photo and video upload in 1 MB chunks to `api/upload.js`, rebuilt under `/mnt/upload` |
| `camera.js` | `CameraView` | Doorway live stream relayed by `api/camstream.js`, full screen |
| `wifi.js` | `SSIDpwd()` | Guest Wi-Fi password for the QR window |

### utils.js

| Area | Exports |
| --- | --- |
| Timing | `refreshInterval()` 10 s, `timeoutInterval()` 50 s, `sleep`, `requestThrottle` (with `cancel()`), `trackPromise`, `singleFlight`, `awaitQuiet` |
| API | `sendCmd(action, arg, var)` → parsed JSON or null; `loadMenu(url)` |
| Files | `fileURL`, `readFile`, `writeFile` (retried after 250 and 750 ms on a dropped connection or 502–504), `savePOST`, `sendFileBeacon` |
| Text | `splitMenuData`, `cleanString`, `splitByDelimiter`, `isNumeric`, `parsePalette`, `menuRow`, `indicator` |
| DOM | `html` tag, `makeElement`, `svgElement`, `promptWindow`, `onButtons`, `setSpinner`, `classDisplay`, `setDisplayById`, `showPopup` |
| Colour | `hexToRgb`, `rgbToHex`, `hsvToRgb`, `rgbToHsv`, `mapNumber` |
| Search and drag | `searchRowElement`, `searchTerm`, `searchMatches`, `DragSort` |
| Edit lock | `FileLock`: a lock file beside the data holding \`OWNER |
| Encryption | `FileCrypt` and helpers: PBKDF2 (310,000 rounds) splits into an AES-256-GCM key kept in the tab and a verifier hashed into a \`V1 |

Files are pipe-delimited text, one row per line. Lists rows are `LIST_ID|LIST_NAME|ITEM_ID|ITEM_NAME|CHECK_STATE` with row order as the order; stored names are printable ASCII without `|`, and an item's line breaks are stored as `~`.

## Styling

CSS is one sheet per component under `styles/`, and the order of the `<link>` tags in `index.html` matters: several component rules beat a `main.css` rule only by coming after it.

1. `main.css`: fonts (JosefinSans), the palette, buttons, menus, prompts, spinners, room prompts (bed, temperature, bathroom settings). Must load first.
2. `groups.css`: the outlined Pixels, APT and room groups, and the landscape layout.
3. `stars.css`, `roomstate.css`, `weather.css`, `switch.css`, `upload.css`, `camera.css`, `colorprompt.css`, `bookmarks.css`.
4. `lists.css`: last, since it holds the drag-handle and search-row rules the bookmarks menu also uses.

**Colour.** Everything derives from one `--main-color` set by the theme picker; `:root` mixes it with white, black and transparency into the rest of the palette (`--main-trans-35` for button faces, `--main-black-30` to `-50` for the walls). When the colour's BT.601 brightness passes 135 (`MainMenu.LIGHT_LUMA`), the page takes `.theme-light` and button text turns black.

**Groups.** A group sizes itself around its buttons, so button size is set only on `.button` (150 × 105 px). `groups.css` exposes its spacing as variables: 3 columns, 7 px clear, gap and spacing, a 24 px legend band, a 20% fill.

**Rules to keep.**

- No `+` inside `calc()`: yui-compressor strips the spaces around it. Write sums as `a - -1 * b`.
- Wrap every `:hover` rule in `@media (hover: hover) and (pointer: fine)`, or iOS keeps the last tapped row lit.
- Put translucency in the background colour, not `opacity`, on menus; iOS Safari repaints opacity layers poorly.
- The palette lives once on `:root`, never on `*`.

## Deployment and conventions

A shell script deploys the site and minifies it in place: yui-compressor for `styles/*.css`, `uglifyjs --compress` for the root `*.js`. The node API (`controller.js` and the modules it imports, plus `api/upload.js` and `api/camstream.js`) runs separately behind lighttpd.

Conventions for new work:

- Add only the code a feature needs; leave existing features as they are, and reuse the site's own helpers (`sendCmd`, `openDeviceMenu`, `drawMenu`, `promptWindow`, `onButtons`, `readFile` / `writeFile`).
- Rooms, their templates, menus and room windows live in `main.js`; a feature that stands on its own is a class in its own module with its own sheet.
- Register a template's buttons with `registerListeners()` directly under it.
- Read and write state files through the site's file API, never a separate store.
- Pico firmware answers the `callPico` form: `http://<host>:4000/api?<cmd>,,<value>~`, with errors as text under HTTP 200.
- Comments explain why, in full sentences; code is spelled out rather than compressed into one-line expressions.
