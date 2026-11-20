# Bathroom API Reference

Nov 15, 2026 · @Ben

The bathroom is its own room in `controller.js`, with one device, `bthr-light1` (the BathroomLight Pico W at `bathroomlight.home`, port 4000), which drives two lights:

- **accent**: the Pico's LED0, dimmable (0–255).
- **main**: the Pico's LED1, on or off only.

Each light has its own mode, trigger distance and off delay; presence hold is shared. Every command below goes through the existing `GET /api` route and comes back as JSON. The firmware's own commands are in `BathroomLight API Command Reference.md` alongside.

## Calling the API

Room commands go in `arg`; device commands go in `var`, with `arg=bthrxmit`, the same way `lrxmit` and `brxmit` work.

| Kind | Request | Example |
| --- | --- | --- |
| Room command | `/api?action=main&arg=<command>` | `/api?action=main&arg=bthrstatus` |
| Device command | `/api?action=main&arg=bthrxmit&var=<command>` | `/api?action=main&arg=bthrxmit&var=bthr-light1_mainon` |
| Device command with a value | `/api?action=main&arg=bthrxmit&var=<command>_<value>` | `/api?action=main&arg=bthrxmit&var=bthr-light1_accent_set_128` |

- Commands ending in `_set` take a value, appended after one more underscore. `parseInputData()` splits `bthr-light1_accent_set_128` into `bthr-light1_accent_set` + `128`. No command name holds `_set` anywhere else, which is why the settings are called `range` and `delay`.
- A value must be a whole number from 0 to 9999; leading zeros are dropped (`0128` sends `128`). Anything else returns `bthrXmit: non-numeric value` or `bthrXmit: out-of-range value` without contacting the light.
- Every reply is a JSON string (the response body is `"128"`), except `bthrstatus`, which returns an object. Numbers arrive as strings, so compare against `"0"` or convert with `Number()`.
- The light can take up to 3 s to answer before the API gives up (`PROCESS_TIMEOUT`); most replies are immediate, and `bthr-light1_sensor` takes about 1 s.

## Room commands

These go in `arg`, like `lrstatus`, `lron` and `lroff` for the living room. The bathroom is not part of `lightson`; `lightsoff` turns both its lights off as `bthroff` does, and `allon` and `alloff` switch it as `bthron` and `bthroff` do.

| Command | What it does | Reply |
| --- | --- | --- |
| `bthrstatus` | Reads both lights' on / off states in one request and builds the room status | Room status object (below) |
| `bthron` | Sends both lights their on command (`bthr-light1on`); each obeys its own mode | `"OK"` straight away, without waiting for the light |
| `bthroff` | Sends both lights their off command (`bthr-light1off`); a light in Stay On ignores it | `"OK"` straight away, without waiting for the light |

`bthrstatus` returns the same `{ roomState, devices }` shape as `lrstatus` and `brstatus`, with two devices:

```json
{
  "roomState": true,
  "devices": [
    { "name": "Ambient Light", "value": "0", "status": false },
    { "name": "Accent Lights", "value": "1", "status": true }
  ]
}
```

| Field | Values | Meaning |
| --- | --- | --- |
| `roomState` | `true` / `false` | `true` while either light is on, whether motion or the website turned it on |
| `devices[].value` | `"1"` / `"0"`, or an error string | On (or fading on) / off (or fading off) |
| `devices[].status` | `true` / `false` / `null` | On, off, or unknown: the light is offline and both rows hold the error |

## Light commands

Send these as `arg=bthrxmit&var=<command>`. Accent levels run from 0 (off) to 255 (full) and are perceptual: 128 looks about half as bright as 255.

| Command | Value | What it does | Reply |
| --- | --- | --- | --- |
| `bthr-light1on` | — | Both lights on, each when its mode allows; the accent fades to 255 | `"accent~main"` states after, e.g. `"1~0"` |
| `bthr-light1off` | — | Both lights off; a light in Stay On ignores it | `"accent~main"` states after |
| `bthr-light1_states` | — | Reads both lights | `"accent~main"`, 1 on, 0 off |
| `bthr-light1_accent` | — | Reads the accent's brightness: where it is (or is fading to) while on, where it will come on while off | `"1"`–`"255"` |
| `bthr-light1_accent_menu` | — | Reads the brightness and the mode in one request, for the Accent menu | `"level~mode"`, e.g. `"180~1"` |
| `bthr-light1_accenton` | — | Fades the accent on to 255 when its mode allows | The level it ends at |
| `bthr-light1_accentoff` | — | Fades the accent off; ignored in Stay On | The level it ends at |
| `bthr-light1_accent_state` | — | Reads whether the accent is on | `"1"` / `"0"` |
| `bthr-light1_accent_set` | 0–255 | Sets the accent's level at once, no fade. A non-zero level always becomes the on level, even when the mode keeps the light off | The level set, or the kept on level when the light stays off |
| `bthr-light1_mainon` | — | Turns the main light on when its mode allows | `"1"` / `"0"`, its state after |
| `bthr-light1_mainoff` | — | Turns the main light off; ignored in Stay On | `"1"` / `"0"` |
| `bthr-light1_main_state` | — | Reads whether the main light is on | `"1"` / `"0"` |
| `bthr-light1_motion` | — | Reads what the motion sensor last reported | `"0"` nobody, `"1"` still, `"2"` motion |

## Light modes

Each light has its own mode, saved on the light, so it survives a power cut.

| Command | Value | What it does | Reply |
| --- | --- | --- | --- |
| `bthr-light1_accent_mode` · `bthr-light1_main_mode` | — | Reads the mode | `"0"`, `"1"` or `"2"` |
| `bthr-light1_accent_mode_set` · `bthr-light1_main_mode_set` | 0, 1 or 2 | Switches the mode; the light goes straight to what the new mode calls for | The new mode |

| Mode | Website label | Light | What on / off commands can still do |
| --- | --- | --- | --- |
| `2` | Stay On | On and stays on | Off is ignored; the accent's level still changes |
| `1` | Automatic | On when someone comes within its trigger distance, off after its off delay with nobody there | On works only while someone is within its distance (or it is already on); off works: the accent fades off, the main light switches off, and its off delay countdown is cleared, so the next movement within its distance turns it on again |
| `0` | Keep Off | Off and stays off | On is ignored; an accent level is kept for later |

So in Automatic, `bthron` from across the room lights the accent (5 ft reach by default) but leaves the main light (12 in) off.

## Motion sensor settings

| Setting | Read | Set | Range | Default | Effect |
| --- | --- | --- | --- | --- | --- |
| Accent trigger distance (cm) | `bthr-light1_accent_range` | `bthr-light1_accent_range_set_<N>` | 1–490 | 150 | Motion this close turns the accent on |
| Accent off delay (s) | `bthr-light1_accent_delay` | `bthr-light1_accent_delay_set_<N>` | 1–9999 | 30 | Time with nobody within the accent's distance before it turns off |
| Main trigger distance (cm) | `bthr-light1_main_range` | `bthr-light1_main_range_set_<N>` | 1–490 | 30 | Motion this close turns the main light on |
| Main off delay (s) | `bthr-light1_main_delay` | `bthr-light1_main_delay_set_<N>` | 1–9999 | 15 | Time with nobody within the main light's distance before it turns off |
| Presence hold | `bthr-light1_presence` | `bthr-light1_presence_set_<N>` | 0 or 1 | 1 | Both lights. 1: anyone within the farther of the two distances, moving or still, keeps every light that is on lit (walking back from the sink keeps the main light on until you pass the accent's distance); 0: each light is kept on only by movement within its own distance |

| Command | What it does | Reply |
| --- | --- | --- |
| `bthr-light1_config` | Reads every setting and both modes | `"presence~accent range~accent delay~accent mode~main range~main delay~main mode"`, e.g. `"1~150~30~1~30~15~1"` |
| `bthr-light1_reset` | Puts every setting back to its default and both modes to Automatic | As `bthr-light1_config` |

- Fill a settings form from the one `bthr-light1_config` read: `const [presence, accentRange, accentDelay, accentMode, mainRange, mainDelay, mainMode] = reply.split('~');`.
- A value outside its range is refused by the light and returns `commonAPIRequest: invalid response`; nothing changes. The two distances are independent.
- `bthr-light1_reset` needs no confirmation from the light, so ask the user before sending it.

## Diagnostics

These return several lines of plain text with their line breaks kept (`\n` in the JSON string).

| Command | Value | What it returns |
| --- | --- | --- |
| `bthr-light1_log` | — | The whole rolling log, up to 60 lines, oldest first |
| `bthr-light1_log_set_<N>` | 1–60 (0 = all) | The newest N log lines |
| `bthr-light1_status` | — | Live sensor, zones, both lights and settings |
| `bthr-light1_sensor` | — | As `bthr-light1_status`, plus what the sensor itself holds; takes about 1 s |

## Errors

Errors come back as ordinary JSON strings with HTTP 200, so check the reply text rather than the status code.

| Reply | Meaning |
| --- | --- |
| `bthrXmit: non-numeric value` · `out-of-range value` | The `_set` value was missing, not a whole number, or above 9999; nothing was sent |
| `bthrXmit: invalid command` | The command name is not in the table (the old `bthr-light1_rangemin`, `_timeout` and the like are gone) |
| `commonAPIRequest: invalid response` | The light refused the request: a value out of range; nothing changed |
| `commonAPIRequest exception: <reason>` | The light did not answer within 3 s: off Wi-Fi, powered off or rebooting |
| `executeMain: invalid command` | The `arg` value is wrong |

## Building the UI

- **The lights change by themselves.** Read their state (`bthrstatus`, `bthr-light1_states`) rather than remembering what was sent.
- **The mode can refuse a command,** and the reply still comes back normally, so read the state back.
- **One request at a time.** The light serves a single client at a time, so send settings one after another (await each).
- **Settings save to the light's flash after a delay** of up to 5 minutes; a power cut before then reverts them. `bthr-light1_status` shows `EEPROM: pending, writes in Ns` until they are stored.
