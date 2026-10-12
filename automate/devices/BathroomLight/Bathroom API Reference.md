# Bathroom API Reference

Oct 10, 2026 · @Ben

The bathroom is its own room in `controller.js`, with one device, `bthr-light1` (the BathroomLight Pico W at `bathroomlight.home`, port 4000). Every command below goes through the existing `GET /api` route and comes back as JSON.

## Calling the API

Room commands go in `arg`; device commands go in `var`, with `arg=bthrxmit`, the same way `lrxmit` and `brxmit` work.

| Kind | Request | Example |
| --- | --- | --- |
| Room command | `/api?action=main&arg=<command>` | `/api?action=main&arg=bthrstatus` |
| Device command | `/api?action=main&arg=bthrxmit&var=<command>` | `/api?action=main&arg=bthrxmit&var=bthr-light1on` |
| Device command with a value | `/api?action=main&arg=bthrxmit&var=<command>_<value>` | `/api?action=main&arg=bthrxmit&var=bthr-light1_set_128` |

- Commands ending in `_set` take a value, appended after one more underscore. `parseInputData()` splits `bthr-light1_set_128` into `bthr-light1_set` + `128`.
- A value must be a whole number from 0 to 9999; leading zeros are dropped (`0128` sends `128`). Anything else returns `bthrXmit: invalid value` without contacting the light.
- Every reply is a JSON string (the response body is `"128"`), except `bthrstatus`, which returns an object. Numbers arrive as strings, so compare against `"0"` or convert with `Number()`.
- The light can take up to 3 s to answer before the API gives up (`PROCESS_TIMEOUT`); most replies are immediate, and `bthr-light1_sensor` takes about 1 s.

## Room commands

These go in `arg`, like `lrstatus`, `lron` and `lroff` for the living room. The bathroom is not part of `lightson`, `lightsoff`, `allon` or `alloff`.

| Command | What it does | Reply |
| --- | --- | --- |
| `bthrstatus` | Reads the light level and builds the room status | Room status object (below) |
| `bthron` | Fades the light on to full (about 2 s) when the light mode allows: not in always off, and not in motion activated with the room empty | `"OK"` straight away, without waiting for the light |
| `bthroff` | Fades the light off (about 2 s); ignored in always on mode | `"OK"` straight away, without waiting for the light |

Because `bthron` and `bthroff` reply before the light acts and may be ignored by the mode, refresh with `bthrstatus` about 2 s later to show what actually happened.

`bthrstatus` returns the same `{ roomState, devices }` shape as `lrstatus` and `brstatus`, with one device:

```json
{
  "roomState": true,
  "devices": [
    { "name": "Bathroom Light", "value": "128", "status": true }
  ]
}
```

| Field | Values | Meaning |
| --- | --- | --- |
| `roomState` | `true` / `false` | `true` while the light is above 0, whether motion or the website turned it on |
| `devices[0].name` | `"Bathroom Light"` | Display name |
| `devices[0].value` | `"0"`–`"255"`, or an error string | Current brightness level |
| `devices[0].status` | `true` / `false` / `null` | On (level above 0), off (level 0), or unknown (`value` holds an error, e.g. the light is offline) |

When the light is offline, `roomState` is `false` and `status` is `null`, so show the room as unreachable rather than off when `status` is `null`.

## Light commands

Send these as `arg=bthrxmit&var=<command>`. Brightness levels run from 0 (off) to 255 (full) and are perceptual: 128 looks about half as bright as 255.

| Command | Value | What it does | Reply |
| --- | --- | --- | --- |
| `bthr-light1` | — | Reads the current level; mid-fade it returns a value in between | `"0"`–`"255"` |
| `bthr-light1on` | — | Fades on to full, about 2 s, when the light mode allows (see *Light modes*) | The level it ends at: `"255"`, or `"0"` when the mode keeps it off |
| `bthr-light1off` | — | Fades off, about 2 s; ignored in always on mode | The level it ends at: `"0"`, or the current level in always on mode |
| `bthr-light1_set` | 0–255; higher becomes 255 | Sets the level at once, no fade, cancelling any running fade. A non-zero level always becomes the on level, even when the mode keeps the light off | The level set, or the kept on level when the light stays off |
| `bthr-light1_motion` | — | Reads what the motion sensor last reported | `"0"` nobody, `"1"` someone still (static presence), `"2"` motion |

- `bthr-light1_set_0` switches off instantly (except in always on mode); use `bthr-light1off` for a fade. It does not change the on level.
- The on level is the brightness the light comes on at, by motion or in always on mode. It is kept only until the light reboots, then returns to 255. `bthr-light1on` always fades to 255 and does not change it.
- A dimmer slider maps directly to `bthr-light1_set_<0–255>`; send on release, not on every movement, because the Pico serves one request at a time. With the light off in motion activated mode, the slider sets the brightness for the next arrival.

## Light modes

The light mode decides who may switch the light: always off, motion activated (the default) or always on. It is saved on the light, so it survives a power cut.

| Command | Value | What it does | Reply |
| --- | --- | --- | --- |
| `bthr-light1_mode` | — | Reads the mode | `"0"`, `"1"` or `"2"` |
| `bthr-light1_mode_set` | 0, 1 or 2 | Switches the mode; the light goes straight to what the new mode calls for | The new mode, e.g. `"2"` |

| Mode | Light | Motion | What the website commands can still do |
| --- | --- | --- | --- |
| `0` Always off | Fades off and stays off | Still tracked, never switches the light | Fade off and set to 0 work; fade on and set level never turn it on, but a level is kept as the on level |
| `1` Motion activated | Fades in when someone arrives, off after the off delay | Switches the light | With the room empty and the light off, fade on is ignored and set level only keeps the on level; otherwise everything works |
| `2` Always on | Fades in to the on level and stays on | Still tracked, never switches the light | Set level changes the brightness; fade off and set to 0 are ignored |

- Switching to motion activated lights the room only if someone is in it at that moment; otherwise the light fades off. Sending the mode already active changes nothing.
- `bthr-light1_reset` puts the mode back to motion activated along with the other settings.
- An out-of-range mode (such as `bthr-light1_mode_set_3`) is refused by the light and returns `commonAPIRequest: invalid response`.
- Suggested control: a three-way selector (Off / Auto / On) that sends `bthr-light1_mode_set_<0|1|2>` and is filled from `bthr-light1_mode` or the fifth field of `bthr-light1_config`.

## Motion sensor settings

Four settings control when motion turns the light on and how long it stays on. Each takes effect at once and survives a reboot (see the save delay under *Building the UI*).

| Setting | Read | Set | Range | Default | Effect |
| --- | --- | --- | --- | --- | --- |
| Nearest detection (cm) | `bthr-light1_rangemin` | `bthr-light1_rangemin_set_<N>` | 0 to farthest − 1 | 0 | Ignores anything closer than this |
| Farthest detection (cm) | `bthr-light1_rangemax` | `bthr-light1_rangemax_set_<N>` | nearest + 1 to 1100 | 300 | Ignores anything further away; set it to the far wall so the doorway is ignored |
| Presence holds light | `bthr-light1_presence` | `bthr-light1_presence_set_<N>` | 0 or 1 | 1 | 1: sitting still keeps the light on; 0: only movement does |
| Off delay (s) | `bthr-light1_timeout` | `bthr-light1_timeout_set_<N>` | 6–9999 | 30 | Time with no detection before the light fades off; includes the sensor's own 5 s hold |

| Command | What it does | Reply |
| --- | --- | --- |
| `bthr-light1_config` | Reads all four settings and the light mode | `"min~max~presence~timeout~mode"`, e.g. `"0~300~1~30~1"` |
| `bthr-light1_rangemin`, `_rangemax`, `_presence`, `_timeout` | Reads one setting | The value, e.g. `"300"` |
| `bthr-light1_<setting>_set_<N>` | Sets one setting | The new value, e.g. `"250"` |
| `bthr-light1_reset` | Puts all four settings back to their defaults and the mode to motion activated | `"0~300~1~30~1"` |

- Split the `bthr-light1_config` reply on `~` to fill a settings form in one request: `const [min, max, presence, timeout, mode] = reply.split('~');`.
- A value outside its range, including a nearest distance not below the farthest, is refused by the light and returns `commonAPIRequest: invalid response`; nothing changes. To move the window further out, set the farthest first; to bring it closer, set the nearest first.
- `bthr-light1_reset` needs no confirmation from the light, so ask the user before sending it.

## Diagnostics

These three commands return several lines of plain text with their line breaks kept (`\n` in the JSON string), so show them in a `<pre>` block or a monospace text area.

| Command | Value | What it returns |
| --- | --- | --- |
| `bthr-light1_log` | — | The whole rolling log, up to 60 lines, oldest first |
| `bthr-light1_log_set_<N>` | 1–60 (0 = all) | The newest N log lines; `_set` here only marks the value |
| `bthr-light1_status` | — | Live sensor, room, light and settings status |
| `bthr-light1_sensor` | — | As `bthr-light1_status`, plus what the sensor itself holds; takes about 1 s |

Log lines are `+<days>d HH:MM:SS  message`, timed from the light's last boot (it has no clock), and the log is cleared by a reboot:

```text
+0d 00:00:00  BathroomLight started
+0d 00:00:03  Wi-Fi connected, IP 192.168.1.57, RSSI -61 dBm
+0d 00:02:10  motion detected, light fading in to 255
+0d 00:03:05  no detection for 30s, light fading off
```

The status text is labelled lines; the ones worth highlighting in a UI:

| Line | Shows |
| --- | --- |
| `sensor:` | `configured`, or `not answering yet, retrying` when the sensor is missing |
| `target:` | The sensor's last state and its number, e.g. `motion (2)` |
| `room:` | `vacant`, or `occupied` with the countdown to vacant |
| `pwmLED0:` | Current level, fade target and on level |
| `EEPROM:` | `saved`, or `pending, writes in Ns` while a settings change waits to be stored |
| `read from C4002:` | `bthr-light1_sensor` only: the sensor's stored range, delay and sensitivities |

## Errors

Errors come back as ordinary JSON strings with HTTP 200, so check the reply text rather than the status code: treat any reply starting with `bthrXmit:`, `commonAPIRequest` or `executeMain:` as an error.

| Reply | Meaning | What to show |
| --- | --- | --- |
| `bthrXmit: invalid value` | The `_set` value was missing, not a whole number, negative or above 9999; nothing was sent | Highlight the input |
| `bthrXmit: invalid command` | The command name is not in the table | A bug in the UI |
| `commonAPIRequest: invalid response` | The light refused the request: a value out of range (or nearest not below farthest); nothing changed | "Value out of range", then re-read the setting |
| `commonAPIRequest exception: <reason>` | The light did not answer within 3 s: off Wi-Fi, powered off or rebooting | "Bathroom light offline" |
| `executeMain: invalid command` | The `arg` value is wrong, e.g. a typo in `bthrxmit` | A bug in the UI |

## Building the UI

The light is driven by the motion sensor as much as by the website, so read its state rather than remembering what the UI last sent.

- **The level changes by itself.** In motion activated mode, motion fades the light in (about 5 s); after the off delay with nobody detected it fades off (about 2 s). Poll `bthrstatus` or `bthr-light1` for the switch and slider, as the other rooms are polled, and after any command.
- **The mode can refuse a command.** A refused switch still replies normally (`"OK"` from `bthron` / `bthroff`, or the level it stays at), so read the level back rather than assuming it changed. Show the current mode next to the switch so a refused tap makes sense.
- **Switched off while someone is there, it stays off** (motion activated) until the room has been empty and motion is seen again. **Switched on while empty** is ignored in motion activated mode; use always on to light an empty room.
- **Read after a fade.** `bthr-light1on` and `bthr-light1off` reply with the target before the fade finishes; read the level about 2 s later if the UI shows the exact value.
- **Settings save to memory after a delay.** A change, the mode included, takes effect at once but is written to the light's flash at most once every 5 minutes; a power cut before then reverts it. `bthr-light1_status` shows `EEPROM: pending, writes in Ns` until it is stored.
- **One request at a time.** The light serves a single client at a time, so send settings one after another (await each), and fill a settings form from the one `bthr-light1_config` read rather than five separate reads.
- **Suggested controls:** an on/off switch (`bthron` / `bthroff`, state from `bthrstatus`), a mode selector (`bthr-light1_mode_set_<0|1|2>`), a 0–255 dimmer (`bthr-light1_set_<N>`), an occupancy indicator (`bthr-light1_motion`), a settings form (`bthr-light1_config` plus the four `_set` commands and reset), and a log/status viewer (`bthr-light1_log`, `bthr-light1_status`, `bthr-light1_sensor`).
