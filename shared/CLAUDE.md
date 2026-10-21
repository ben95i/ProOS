# Automate Home Automation Website

Project reference imported from the Claude chat "Automate" project doc (Oct 11, 2026) and checked against the code.

## Where things are

| Path (this repo) | What | Deployed to |
| --- | --- | --- |
| `html/` | The UI: `index.html`, one ES module per feature, `styles/`, `menus/`, `img/`, `fonts/` | `/var/www/html` (staged via `/opt/rpi/config/html/`) |
| `nodeapi/` | The node API (Express 5, CommonJS): `server.js` entry, `controller.js` dispatch, one module per service (see *Node API*) | `/opt/nodeapi`, run by `nodeapi.service` |
| `nodeapi/www-minify.sh` | Minifies the deployed site in place, run by the installer | `/opt/nodeapi/www-minify.sh` |
| `../automate/config/` | Host config: `installer.sh`, `lighttpd.conf`, `nodeapi.service`, `rc.local` | the Automate server |
| `../login.sh` | Deploys from the repo root: rsyncs `shared/html/` and `shared/nodeapi/` to a host | |

There is no build step, test suite or linter in the repo; `html/` is served as written and minified only on the host. On the host, menu files are read from `/var/www/html/menus/<name>.txt` and saved state (any `*.save` file, plus `sysout`) is read and written under `/var/www/html/ram/`.

## Overview

Automate is a one-page home automation site that runs the apartment's lights, audio, PCs, LEDs and doorway camera from a phone or browser. It is plain JavaScript ES modules with no framework: `index.html` loads `main.js`, which draws the whole page from template literals and imports one module per feature and one per room.

The modules import in one direction only, so none of them can call back up: `error.js` (the message popup and error reports) at the bottom, `utils.js` above it, `volctrl.js` (the amp rooms' volume controls) and `dropdowns.js` above it, the room modules (`livingrm.js`, `bedrm.js`, `bathrm.js`) and the Server window (`server.js`) above that, and `main.js` on top. What `dropdowns.js` needs from the page (the rooms, the LED text scroller, the browser settings, the bookmarks and weather menus) is handed to it once by `configureMenus()` in `main.js`.

One codebase serves three device models. `main.js` reads the model from the page's `<title>` at load and draws only that model's markup; markup for the other models never reaches the page.

| Model | Detected by | What it draws |
| --- | --- | --- |
| Server | title is exactly `Automate` (`SERVER_MODEL`), as `index.html` is served on the automate host | Full page: top nav (main menu, bookmarks, room status, room selector, weather, stats), Pixels, APT and room groups, server options |
| LED Pi | any other title holding "led" | Pixels group, LED text scroller, Pi main menu, Pi options, log window |
| Pi | any other title (a Pi's installer puts its `brand.txt` name there: "Bedroom", "AudioCPU", "Doorway") | Pi main menu, Pi options, log window |

lighttpd serves the static files and reverse-proxies `/api` to the node API on 127.0.0.1:3000. Every button press becomes a `GET /api?action=…&arg=…&var=…`, menu and list files are read and written through `/api/read` and `/api/write`, uploads go to `/api/upload` and the doorway camera streams from `/api/camera/doorway`.

## Architecture

The browser never talks to a device: every press is a request to the node API, and `executeMain()` in `controller.js` dispatches it to the room or device it names.

`browser → lighttpd → /api → node API (127.0.0.1:3000) → executeMain() → room transmitter / Pico / Pi`

Room transmitters answer the Automate API on port 80 (`callApi`), Pico controllers answer `?<cmd>,,<value>~` on port 4000 (`callPico`), and every call gives up after 3 s. A room status (`lrstatus`, `brstatus`, `bthrstatus`) reads all of a room's devices at once and answers `{ roomState, devices }`. The routes, the command table and the API's modules are under *Node API*.

## How the page is built

The page is drawn once from template functions in `main.js`, and every button reaches its handler through one click listener on `document`.

1. **Load.** On `DOMContentLoaded`, `loadPage()` reads the model and `renderPage()` injects the markup from `buildPageHTML()`. Every injected node carries `data-app-injected`, so `removeInjectedPage()` can take the whole page down before a re-render.
2. **Features.** `loadFeatures()` starts the server-only parts (the last room from `localStorage` key `ctls-mode`, `RoomStateManager`, `WeatherWidget`), then the main menu and theme colour, the stars background and the activity timer.
3. **Buttons.** Each template is followed by `registerListeners({ id: handler })`, in `main.js`, in each room module and in `server.js` alike. The map and `dispatchRegisteredListener()` live in `utils.js`; one delegated listener walks up from the clicked element to the first id in that map, so markup drawn later (room grids, server panels) needs no binding.
4. **Every click.** `handleClicks()` restarts the 50 s activity timer (on expiry the stars and the room status refresh pause) and closes open menus unless the click was on a `.button`, a bookmark or a `.nohide` element.
5. **Teardown.** `removeDropdowns()` closes menus and destroys their toggle switches; `App.destroyAll()` on `beforeunload` and `destroyPageServices()` end every running service.

Shared state lives on `window.App`: the service instances (room status, stars, lists, bookmarks, camera, colour prompt, bathroom settings), the drawn menu rows, the open toggle switches and the current room.

### Dynamic menus

All of the menu code is in `dropdowns.js` (styles in `styles/dropdowns.css`). Most dropdowns are filled from text rows, one per line, as `HOST|TYPE|NAME`; a row that sends a command carries `cmd~<host>~<command>` in its first field. `drawMenu(data, menuId, { TwoColumns, MenuScroll })` turns the rows into links, and `createListItem()` builds each one through the `ROW_BUILDERS` table, one function per row type. Each request stamps a token (`beginMenuRequest()`), and a reply that lands after another menu was opened is dropped.

`openDeviceMenu(config)` is the common path for a device button: open the dropdown, show the button spinner, send one command, build rows from the reply, draw. `showPowerMenu(target, menu)` is the on / off and slider menu, and `showInputMenu(config)` an amp's input list.

| Row type | Draws | Sends |
| --- | --- | --- |
| `grnind` · `blkind` · `redind` · `ylwind` · `noind` | Label with a green, black, red or yellow dot, or none | The row's command on click, for `cmd~` rows |
| `toggle` | `ToggleSwitch` from the device's 1 / 0 | `<command>on` / `<command>off` |
| `slider` | 0–255 slider | `<command>_set_<n>` while sliding, 75 ms throttle; the release value always sent last |
| `slidercust` | 1–255 slider | As `slider` |
| `on` · `off` · `pause` | Label with an icon | The row's command |
| `chk` | Switch for a browser-side setting (stars animation) | Nothing; saved in `localStorage` |
| `room` | A room name | Switches room and restarts the room status |
| `subs` | Subwoofer mode, red when on | Turns the volume buttons into subwoofer controls |
| `link` | A URL | Opens it |
| `ledeffectmenu` · `relaxmenu` · `relaxactive` · `relaxstop` | LED effects, relax sounds | The effect or sound named |
| `pixelsmore` | More... with a gear, under the Pixels Control slider (server only) | Nothing; opens the Pixels window through the `openPixels` hook |

## The page

The server page is a top bar over three outlined button groups: Pixels and APT in the left column, the current room on the right. In portrait the room group sits under the others; under 700 px of height it stands beside them, both columns ending level.

### Top bar

| Item | What it does |
| --- | --- |
| Main menu (list icon) | About window, Colors (theme colour picker), stars animation switch. The Pi models get About, Colors, stars, System (log window) and Home. |
| Bookmarks (star) | Bookmarks menu with search, add, edit and drag re-order, kept in a file under its own edit lock |
| Room status light | Polls `<room>status` every 10 s with a countdown ring; hatched once the readings go stale after 50 s idle, red on an error. Tapped, it lists each device with a green, black or red dot. |
| Room selector (centre) | The current room's icon and name; opens a menu of the other rooms |
| Weather | The weather widget and its menu; reads the `weather` command once a minute |
| Stats (network icon) | Runs the `status` command, which probes every host in the `statsmenu` file, and shows a green or black dot per host |

### Pixels

- **Colors**: colour prompt for the LED pixels (`leds hex …`).
- **Control**: Off and Pause, a brightness slider for the Fadecandy LEDs, and (server only) **More...**, which opens the Pixels window: one box per LED host in the `mainmenu` file with its name, a status line under it, a link to its page and its own brightness slider. Every host is asked for its status at once (`fc_host_status`), so a host that is down shows "Not available" in red without holding up the rest. The status line reads "Maze is active" or "Maze is paused" (the effect's name from the `ledeffectmenu` file), a dot in the colour then "is set" for a solid colour, or "Not active" when stopped or black; a slider sends `fc_host_set_<n>` to its host only, throttled while dragged and once more on release. Close and Refresh span the boxes' width. Refresh reads the host list and every status again, its icon turning until all have answered; while the list is unchanged the boxes stay, each slider gliding to its new level (one being held stays put) and a state line that changed switching to its new text at once (`pixels.js`, `styles/pixels.css`).
- **Animated**: two-column menu of LED effects from the `ledeffectmenu` file; its TextScroll entry opens the LED text scroller (colour, font, speed). Shuffle is picked on the server from the `ledshuffle` file, so every host plays the same effect; a Pi asked to shuffle on its own (the LED Pi page, the LEDwall's front button) picks from its own copy of the same file.

Effects, Off, Pause and the text scroller send through `sendLeds()` in `dropdowns.js`, which shows a popup naming any LED host that did not answer.

### APT

| Row | Buttons |
| --- | --- |
| 1 | **Room** on / off (`<room>on`, `<room>off` for the room on screen) · **Lights** on / off (`lightson`, `lightsoff`) · **Power** all on / off (`allon`, `alloff`) |
| 2 | **YouTube** (link) · **Temperature / humidity** (thermometers from `roomth`, refresh) · **Media** (link) |
| 3 | **Doorway** (live camera window, full screen) · **Server** (log window + server options) · **Lists** (lists window) |

The Server log window arms one command at a time from three option panels (main, backup, regions) and sends it with the send button; password commands go through a password prompt first. The panels cover AFP, CIFS, Xana, UniFi, the router and network, backups and snapshots, archive and RAM disk regions, uploads and clearing the log. All of it lives in `server.js` and `styles/server.css`: the log window and panel templates, the armed command, the password prompt, the guest Wi-Fi window, the Pi models' options panel and Wi-Fi setup, and the upload button. `main.js` draws its two templates and opens and closes the log window through `showLogWindow()` and `closeLogWindow()`, because closing it also closes the camera window.

### Room group

The third group's legend shows the current room's icon and name, and its grid is the room's own template (next section). Switching rooms closes any open menu, swaps the grid, saves the choice and restarts the room status.

## Rooms and devices

Each room is a module of its own that exports its config (`LIVING_ROOM_CONFIG` in `livingrm.js`, `BEDROOM_CONFIG` in `bedrm.js`, `BATHROOM_CONFIG` in `bathrm.js`): a key, a name, an icon, a volume target and a grid template. The module also holds the room's button listeners, its menus and its windows, and `main.js` only collects the three configs into `ROOM_CONFIGS`. The key names the room's API commands: `<key>status`, `<key>on`, `<key>off`, and device commands go through `<key>xmit`.

The grids are built from `dropdownButtonTemplate()` and `gridButtonTemplate()` in `utils.js` and `powerButtonTemplate()` in `dropdowns.js`. The Pixels and APT groups and the text scroller in `main.js`, and the Server window's option panels in `server.js`, are built from the same two; both take an optional `buttonClass` (`svropt_button`, `button__sml`), and a button may have an icon, a label or both. The windows' own buttons (log, camera, bathroom settings, Pixels, bed, colour prompt, About, upload, lists, password and Wi-Fi prompts) are drawn with `gridButtonTemplate()` too. Three kinds stay written out: the temperature / humidity grid button (two icons), and the temperature window's and bookmark edit window's buttons, whose icon class sits on the button itself; every one of them has an id. The volume buttons, their handlers, the subwoofer mode and the volume popup are shared by the two rooms with an amp and live in `volctrl.js` (`volumeButtonsTemplate`, `volumeButtonListeners`, `sendVolume`, `showVolumePopup`).

| Room | Key | Volume target | Room status reads |
| --- | --- | --- | --- |
| Living Room | `lr` | LEDwall (`lrxmit`), 0–100 | Main Lamp, Backlight, Desktop PC, Stereo System |
| Bedroom | `br` | `brpi`, 0–192 shown as % | Desk Lamp, Bed Lights, RetroPi TV, Vintage Macs, Bedroom TV, Bedroom PC, Audio System |
| Bathroom | `bthr` | none | Bathroom Light |

### Living Room

| Button | Opens | Device / command |
| --- | --- | --- |
| Lamp | On / off switch + brightness slider | Pico living room lamp `wl1_dual` (`lrmlamp1`, 192.168.1.18:4000) |
| Desktop | On / off switch | `wkststate`: ping, then the PC power relay |
| Ceiling | On / off switch | `clr1` |
| Volume − · mute · + | Volume popup | `voldwn`, `volmute`, `volup` on the stereo amp |
| −5% · +5% | Volume popup | `voldwnc`, `volupc`; in subwoofer mode `subdwn_fast`, `subup_fast` |
| Stereo | On / off switch | `hifistate` |
| Volume | Volume popup | `voldisplay`; sends nothing in subwoofer mode |
| Inputs | Input list, live one green | `curinput`: Auto, USB, Phono, Optical, Coaxial, Aux |
| More… | Subs Gain, Low Cut, vLimiter | `curhpfvollim`, `togglehpf`, `vlimit`; Subs Gain turns the volume and ±5% buttons red and sends `sub…` |

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
| Mode | Stay On / Automatic / Always Off, active one green | `bthr-light1_mode`, `bthr-light1_mode_set_<0–2>` |
| Settings | Motion sensor settings window | See *Bathroom light* |

The bathroom is left out of `lightson`, `lightsoff`, `allon` and `alloff`; APT's Room on / off still reaches it as `bthron` / `bthroff` while it is the room on screen.

## Bathroom light

The bathroom's one device is the BathroomLight, a Pico W on 192.168.1.21:4000 (`bthrlight1` in the API's address table) driving a PWM LED and a C4002 mmWave motion sensor. Motion fades it in, and it fades off after the off delay with nobody detected. All of its controls live in `bathrm.js`, styled by `styles/bathrm.css`.

- **Brightness** reads `bthr-light1`, 1–255: the level the light is at (or fading to) while it is on, and the level it will come on at while it is off. The slider is an ordinary `slider` row, so the light gets at most one level every 75 ms while it is dragged and the release value always lands last.
- **Mode** reads `bthr-light1_mode` and marks Stay On (2), Automatic (1) or Always Off (0) green; a row sends `bthr-light1_mode_set_<n>`.
- **Room status** reads `bthr-light1_state`, 1 or 0, which reports where a fade is heading from the moment it starts.

### Settings window

`bathroomSettingsPrompt()` opens a centred window, 302 px wide so its setting boxes end level with its row of three buttons, filled from one `bthr-light1_config` read (`min~max~presence~timeout~mode`). Each change is sent on release and confirmed in the title for 5 s ("Off delay: 6 min 8 s"); errors show there in red, and "Light offline" stays while the settings are greyed out.

| Setting | Control | Range | Command |
| --- | --- | --- | --- |
| Nearest detection | Slider, held below the farthest | 0–1000 cm, shown in feet | `bthr-light1_rangemin_set_<n>` |
| Farthest detection | Slider, held above the nearest | 0–1000 cm, shown in feet | `bthr-light1_rangemax_set_<n>` |
| Presence hold (presence keeps the light on) | Switch | 0 / 1 | `bthr-light1_presence_set_<n>` |
| Off delay | Slider on a cube curve (about 1 s steps at the short end) | 6 s – 1 h | `bthr-light1_timeout_set_<n>` |

The Log and Sensor buttons turn the window into a log box twice as wide (680 px), filled from `bthr-light1_log` or `bthr-light1_sensor`, with Back, Refresh (reads the same read-out again) and Close under it. Every request from the window waits for the one before it (`bthrRequest()`), because the Pico serves one request at a time.

### API commands

`bthrXmit()` in `controller.js` maps each command to a Pico call `http://192.168.1.21:4000/api?<cmd>,,<value>~`. The log and status replies keep their line breaks; the rest are single values. A `_set` value has to be a whole number from 0 to 9999, checked inside `bthrXmit()` itself, or the answer is `bthrXmit: invalid value` and nothing is sent; ranges within that are the Pico's to check.

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

## Node API

The API is one Express 5 app (`nodeapi/server.js`) listening on 127.0.0.1:3000 behind lighttpd, run as `nodeapi.service` from `/opt/nodeapi`. Its dependencies are `express`, `express-validator`, `jpeg-js` and `pngjs`; it also needs `ping`, `wakeonlan`, `nmap` (through `sudo`) and `ffmpeg` on the host.

### Routes

| Route | What it does | Answers |
| --- | --- | --- |
| `GET /api?action=&arg=&var=` | A command. `action=main` goes to `executeMain(arg, var)`; `action=leds`, or `main` with `arg=leds`, goes to the LED hosts | JSON: a string, or an object for a room status, the weather and a site lookup |
| `GET /api/read?file=` | Reads `menus/<file>.txt`, or `ram/<file>.txt` for `sysout` and any name holding `.save` | The text as a JSON string, the last 64 KB of it |
| `POST /api/write?file=` | Writes the body (text, up to 5 MB) to `ram/<file>.txt`, replacing the file | 200 with no body |
| `POST /api/upload?action=` | Chunked upload: `start`, `chunk`, `finish`, `abort` (`upload.js`) | JSON per step |
| `GET /api/camera/<name>` | Live fragmented MP4 of a camera (`camstream.js`) | A stream, with `X-Camera-Codec` and `X-Camera-Rotation` headers |

Every query value has to match `[A-Za-z0-9_.#%@&-]*`, or the route answers 400 with the validation errors; this is why the UI sends free text (LED messages, URLs, Wi-Fi settings) as URL-safe base64 with `@` for `=`. A file name holding `..` is refused. A command that fails answers its error as text under HTTP 200; only an exception in the route itself answers 500.

### Modules

| Module | Exports | What it does |
| --- | --- | --- |
| `server.js` | (entry) | The app, the three core routes, query validation, `safePath()` |
| `controller.js` | `executeMain` | Command dispatch, the room transmitters and Picos, room scenes and room status, server commands |
| `ledparser.js` | `OPCLEDparser` | Sends an LED command to every host in `menus/mainmenu.txt` (one `URL|NAME` per line; an older `URL|link|NAME` line still reads) at once through one queue per host: one request in flight per host, and a newer brightness (`fc_set`) or scene command replaces the waiting one of its kind, so the last value always lands. `fc_set` and `hex` answer `OK` at once; other commands wait up to 1.5 s and answer `OK` or `No answer from <names>`. `fc_leds` answers with the first host's level. `fc_host_status` and `fc_host_set_<n>`, with the host's NAME as `var`, are the Pixels window's: the first passes on the host's own `fc_status` once checked (`{ effect, state, color, brightness }`, state `off` · `active` · `paused` · `color`), the second sets its brightness and answers `OK`; both answer `No answer from <name>` when it does not. Server-only: the Pis install `rpi/ledparser.js` over it |
| `statusupd.js` | `runStatusUpdate` | Probes each `HOST\|NAME\|ACTION` row of `menus/statsmenu.txt` (`ping`, or `ssh` as a TCP connect to port 22), 12 at a time, and answers `HOST\|grnind or blkind\|NAME\|ACTION` rows |
| `weather.js` | `runWeatherWidget` | Open-Meteo reading for the place in `weather.json`, cached 5 minutes: `{ location, temperature, condition, humidity, timeOfDay, lastUpdated }` |
| `sitelookup.js` · `favicon.js` | `runSiteLookup` · `runFaviconLookup` | For a new bookmark: a page's title and its favicon as a 24 × 24, 32-colour PNG in URL-safe base64; `{ title: "", icon: "" }` on any failure |
| `netscan.js` | `runScanNetwork` | `nmap -sn` over 192.168.1.0/24, as a table of address, hostname, vendor and MAC |
| `upload.js` | `registerUploadRoutes` | Appends 1 MB chunks to a `.part` file under `/mnt/upload/.part` and renames it into `/mnt/upload` on finish; 1 GB per file, 8 uploads at once, executable types refused, abandoned uploads swept after 30 minutes |
| `camstream.js` · `fmp4.js` | `registerCameraRoutes` | One `ffmpeg -c copy` per camera in `CAMERAS` (`doorway`: `rtsp://doorway.home:8554/cam`), started by the first viewer, shared by the rest and stopped 5 s after the last leaves; `fmp4.js` cuts the stream into boxes and reads the codec, keyframes and rotation |

### Commands

Device addresses are in one table at the top of `controller.js`, `IP_ADRS`: `lrxmit` 192.168.1.12 (the LEDwall), `desk` .14, `brpi` .15, `brpc` .17 (with its MAC for wake-on-LAN), `lrmlamp1` .18 and `bthrlight1` .21.

`executeMain(baseWWW, arg, var)` switches on `arg`. For the three `…xmit` commands `var` is split at `_set`, so `wl1_dual_set_120` reaches the room as `wl1_dual_set` and `120`.

| `arg` | `var` | What it does |
| --- | --- | --- |
| `lrxmit` · `brxmit` · `bthrxmit` | a device command | `lrXmit()`, `brXmit()`, `bthrXmit()`: one device of that room |
| `lrstatus` · `brstatus` · `bthrstatus` | | Reads the room's devices together: `{ roomState, devices: [{ name, value, status }] }`; the room is on when any device is |
| `lron` · `lroff` · `bron` · `broff` · `bthron` · `bthroff` | | The room's scene, fired without waiting; answers `OK` |
| `allon` · `alloff` | | The living room and bedroom scenes together |
| `lightson` · `lightsoff` | | Both lamps, the dresser lamp and the bed lights; off also stops the LED pixels |
| `lrlightson` · `lrlightsoff` | | The living room lamp's first channel |
| `brpi` | a command | Passed straight to the Bedroom Pi (`ampstate`, `inputstate`, `vol…`, the input names) |
| `relax` | a sound | Plays it on the Bedroom Pi, turns the TV off and sleeps the bedroom PC |
| `relaxmenu` · `relaxstop` | | The sound list as menu rows, or stop |
| `status` | | `runStatusUpdate()` |
| `weather` | | `runWeatherWidget()` |
| `sitelookup` | a URL, base64 | `runSiteLookup()` |
| `server` | a command name | Appends "`<name>` sent." to `ram/sysout.txt` and drops an empty `ram/<name>.txt` trigger file for the host's shell side to act on; `netscan` runs here instead and writes its table into the log |

**Living room (`lrXmit`).** The stereo commands are codes sent to the LEDwall at 192.168.1.12 (`extcom`): power (`hifistate`, `…on`, `…off`), inputs (`usb`, `aux`, `phono`, `coax`, `opt`, `autodac`, `curinput`), volume (`volup`, `voldwn`, `volupc`, `voldwnc`, `volmute`, `voldisplay`), `vlimit`, `togglehpf`, `curhpfvollim` and the subwoofers (`subup`, `subdwn`, `submute`, `subup_fast`, `subdwn_fast`). The living room lamp is a Pico at 192.168.1.18 (`lrmlamp1`; `lrmLamp1Status()`, `lrmLamp1On()`, `lrmLamp1Off()`): `wl1_dual` reads both channels as `lamp~backlight`, `wl1_dualon` / `off` and `wl1_led1…` / `wl1_led2…` switch them, `wl1_dual_set_<n>` dims. `wkststate` pings the desktop (192.168.1.14), and its `on` / `off` press the PC power relay only when the ping says the state would change.

**Bedroom (`brXmit`).** Codes go to the Bedroom Pi at 192.168.1.15 (`hwcom`). A status code answers three digits, one per device, where 9 means on; each command names the digit it reads (`brlamp1` 0, `brmacs` 1, `retropi` 2). Also `brtv`, the bed (`bed_adj_up` / `dwn` / `stop`, `bed_leds`, `bed_leds_rb` / `cy` / `sw` / `off` / `pause`, `bed_leds_set_<n>`, `bed_leds_hex_set_<hex>`) and `roomth` (temperature~humidity). `brpc` pings 192.168.1.17, `brpcon` sends wake-on-LAN and `brpcoff` asks the PC to sleep.

**Bathroom (`bthrXmit`).** See *Bathroom light* above.

## Shared utilities and components

Each feature larger than a room button is a class in its own module, and anything two modules need lives in `utils.js`.

| Module | Class | What it does |
| --- | --- | --- |
| `dropdowns.js` | (functions) | Dropdowns, `drawMenu` / `createListItem`, the menu request token, power, input, file and API menus |
| `volctrl.js` | (functions) | The amp rooms' volume buttons: `volumeButtonsTemplate`, `volumeButtonListeners`, `registerVolumeButtons`, `sendVolume`, the subwoofer mode (`subwooferVolumeEnabled`, `subwooferVolumeMode`), the volume popup (`showVolumePopup`, `clearVolumePopupTimer`) |
| `livingrm.js` · `bedrm.js` · `bathrm.js` | (functions) | One room each: config, grid template, listeners, menus and windows |
| `server.js` | (functions) | Server log window, option panels, armed command and send, password prompt, Wi-Fi windows, upload button |
| `pixels.js` | (functions) | Pixels window: per-host brightness sliders and links for the LED hosts in the `mainmenu` file |
| `mainmenu.js` | `MainMenu` | Main menus, theme colour (live while picking, kept on close), light-theme switch, About window |
| `roomstate.js` | `RoomStateManager` | Room status light, 10 s refresh, device dropdown |
| `colorprompt.js` | `ColorPrompt` | Colour window: saturation / value map, hue slider, palette swatches with names, hex box; `openColorPrompt()` opens it for a device |
| `switch.js` | `ToggleSwitch` | Draggable on / off switch used in menus and windows |
| `stars.js` | `AnimatedBackground` | Star canvas behind the page, paused when idle |
| `weather.js` | `WeatherWidget` | Weather in the top bar and its menu |
| `bookmarks.js` | `Bookmarks` | Bookmarks menu, add / edit window, file and lock |
| `lists.js` | `Lists`, `ListStore` | Lists window: lists and items, drag re-order, multi-select delete, completed toggles, per-list encryption, edit lock, connection status |
| `encrypt.js` | `FileCrypt` | Browser-side encryption of a file, used by `lists.js` (with its key, blob and password record helpers): PBKDF2 (310,000 rounds) splits into an AES-256-GCM key kept in the tab and a verifier hashed into a `V1\|SALT\|ITERATIONS\|CHECK` record file; the key is never stored or sent to the server |
| `upload.js` | `Uploader` | Photo and video upload in 1 MB chunks to `nodeapi/upload.js`, rebuilt under `/mnt/upload` |
| `camera.js` | `CameraView` | Doorway live stream relayed by `nodeapi/camstream.js` (`/api/camera/<name>`), full screen |
| `wifi.js` | `SSIDpwd()` | Guest Wi-Fi password for the QR window |
| `error.js` | `HttpError` | The message popup and every failure report (see *Errors* below) |

### utils.js

| Area | Exports |
| --- | --- |
| Timing | `refreshInterval()` 10 s, `timeoutInterval()` 50 s, `sleep`, `requestThrottle` (with `cancel()`), `trackPromise`, `singleFlight`, `awaitQuiet` |
| API | `sendCmd(action, arg, var, options)` → parsed JSON, or null with the failure reported (`{ quiet: true }` logs it to the console only); `loadMenu(url)` |
| Files | `fileURL`, `readFile`, `writeFile` (retried after 250 and 750 ms on a dropped connection or 502–504), `savePOST`, `sendFileBeacon`, `reportFileFailure(what)` (reports why the last file request failed) |
| Text | `splitMenuData`, `cleanString`, `splitByDelimiter`, `isNumeric`, `parsePalette`, `menuRow`, `indicator` |
| DOM | `html` tag, `makeElement`, `svgElement`, `promptWindow`, `onButtons`, `setSpinner`, `classDisplay`, `setDisplayById` |
| Buttons | `registerListeners`, `dispatchRegisteredListener`, `gridButtonTemplate`, `dropdownButtonTemplate` |
| Colour | `hexToRgb`, `rgbToHex`, `hsvToRgb`, `rgbToHsv`, `mapNumber` |
| Search and drag | `searchRowElement`, `searchTerm`, `searchMatches`, `DragSort` |
| Edit lock | `FileLock`: a lock file beside the data holding one row, `OWNER_TOKEN\|TIMESTAMP`, kept alive by a heartbeat; `onLost()` fires when another window takes it |

### Errors

`error.js` holds the message popup and every report of a failure, styled by `styles/error.css`. It imports nothing, so `utils.js` and every other module can use it.

| Export | What it does |
| --- | --- |
| `showPopup(message, options)` | The popup at the top of the page, 4 s; a plain notice ("[lists] password changed.") is markup, an error passes a node, a class and a duration |
| `reportError(what, error)` | Logs a caught failure and shows it as one of the two kinds below, whichever the error is |
| `reportNotConnected(what, reason)` · `reportException(what, reason)` | One kind directly, with a sentence of its own |
| `HttpError(status, body)` | Thrown for an error answer, so its status and text reach the popup |
| `logError(what, error)` | Console only, no popup |
| `installErrorHandlers()` | Reports uncaught errors and rejections; called once from `main.js` |

**Not connected** (wifi-slash icon) is a request that got no answer (Safari "Load failed", Chrome "Failed to fetch"), the device being offline, a 502–504 from lighttpd while the API is down, or LED hosts that did not answer. **Exception has occurred** (alert icon) is anything else: an error status, an answer that is not JSON, an error thrown in the page. Both show a sentence, then rows for what the page was doing (`what`, a gerund phrase: "sending lrxmit wl1_dual"), the error, the server's answer and where it was thrown, for 7 s, and log it all to the console under `[Automate]`. The newest error replaces the one on screen, so the popup always names the latest failure. The room status refresh shows no popup at all (`sendCmd(…, { quiet: true })`): its light turns red instead, and the failure is logged. `drawMenu()` given no data only logs, since the request that came back empty has already reported why.

Files are pipe-delimited text, one row per line. Lists rows are `LIST_ID|LIST_NAME|ITEM_ID|ITEM_NAME|CHECK_STATE` with row order as the order; stored names are printable ASCII without `|`, and an item's line breaks are stored as `~`.

## Styling

CSS is one sheet per component under `styles/`, and the order of the `<link>` tags in `index.html` matters: several component rules beat a `main.css` rule only by coming after it.

1. `main.css`: fonts (JosefinSans), the palette, buttons, prompts, popups, spinners. Must load first. `error.css` (the message popup and its error forms) comes straight after it.
2. `dropdowns.css`, then `livingrm.css`, `bedrm.css`, `bathrm.css`, then `server.css`: the dropdown menus and their rows, each room's own rules (subwoofer mode, bed window, bathroom settings), and the Server window (log size, options panels, password and Wi-Fi prompts). Cut out of `main.css` and kept directly after it, in this order, so they meet every later sheet as they did before.
3. `groups.css`: the outlined Pixels, APT and room groups, and the landscape layout.
4. `stars.css`, `roomstate.css`, `weather.css`, `switch.css`, `upload.css`, `camera.css`, `colorprompt.css`, `pixels.css`, `bookmarks.css`.
5. `lists.css`: last, since it holds the drag-handle and search-row rules the bookmarks menu also uses.

**Colour.** Everything derives from one `--main-color` set by the theme picker; `:root` mixes it with white, black and transparency into the rest of the palette (`--main-trans-35` for button faces, `--main-black-30` to `-50` for the walls). When the colour's BT.601 brightness passes 135 (`MainMenu.LIGHT_LUMA`), the page takes `.theme-light` and button text turns black.

**Groups.** A group sizes itself around its buttons, so button size is set only on `.button` (150 × 105 px). `groups.css` exposes its spacing as variables: 3 columns, 7 px clear, gap and spacing, a 12 px label with a 7 px gap above and below its capitals, a 20% fill.

The CSS rules to keep (`calc()`, `:hover`, translucency, the palette) are in `../CODESTYLE.md`.

## Deployment and conventions

`../login.sh` copies the site and API to the host, and `nodeapi/www-minify.sh` minifies the deployed site in place: yui-compressor for `styles/*.css`, `uglifyjs --compress` for the root `*.js`, with a cache under `/opt/www-mincache` so unchanged files are not recompressed. `installer.sh` on the host copies the staged site into `/var/www/html` (leaving `ram/` alone), installs the API and its service, and runs the minifier. The node API runs separately behind lighttpd and is not minified. lighttpd (`automate/config/lighttpd.conf`, `rpi/config/lighttpd.conf`) sends the page, `.js` and `.css` with `max-age=0`, so browsers revalidate them on every load: one stale cached ES module (iOS Safari keeps them otherwise) can fail to link against new ones and leave the page black.

Where new work goes (code style rules, including buttons, listeners and helpers, are in `../CODESTYLE.md`):

- A room's template, listeners, menus and windows live in its own module and sheet; code two rooms share goes in `utils.js` (the volume controls in `volctrl.js`), and anything that draws or opens a menu in `dropdowns.js`. A new room exports its config and is added to `ROOM_CONFIGS` in `main.js`. A feature that stands on its own is a class in its own module with its own sheet.
- Imports go downwards only (`main.js` → rooms and `server.js` → `dropdowns.js` → `volctrl.js` → `utils.js` → `error.js`); a lower module that needs something from the page takes it as a parameter or through `configureMenus()`.
- Read and write state files through the site's file API, never a separate store.
- Report failures through `error.js` (`reportError` around a request, `reportFileFailure` after a failed `readFile` / `writeFile`), never a hand-written popup; `showPopup` is for notices.
- A new API command is a case in `executeMain()`, or an entry in a room's code table; a service with its own route registers it from its own module, as `upload.js` and `camstream.js` do. Query values must fit the API's character set, so free text goes as URL-safe base64.
- Pico firmware answers the `callPico` form: `http://<host>:4000/api?<cmd>,,<value>~`, with errors as text under HTTP 200.
