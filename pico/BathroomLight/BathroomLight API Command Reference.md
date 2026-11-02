# BathroomLight API Command Reference

Oct 10, 2026 · @Ben

The Pico W light takes one command per HTTP request on port 4000 and replies with plain text. New in this version: command 13 switches between three light modes, and in motion activated mode a new brightness is kept for the next detection instead of lighting an empty room.

## Request format

Every request is an HTTP GET whose URL carries one command framed as `?<command>,<unused>,<value>~`.

- The node API's `callPico(target, arg1, arg2)` sends `http://<target>:4000/api?<arg1>,,<arg2>~` and strips the newlines from the reply. The firmware ignores the path and the middle field, so `/api?13,,2~` and `/?13,0,2~` do the same thing; this sheet uses the short form.
- Leaving out `arg2` sends an empty value, which counts as 0: `callPico('BathroomLight', 8)` reads all settings, but `callPico('BathroomLight', 2)` fades the light off.
- Multi-line replies (commands 9 and 10) come back from `callPico()` with their lines run together, so read those with curl or a browser.
- Each field is a whole number of up to 4 digits; extra digits are cut off. A negative value is read as a very large number: settings reject it with 400 ERR and command 3 treats it as 0.
- Quote the URL in the shell, because zsh and bash can treat `?` as a wildcard. One client is served at a time; a client that stops sending for 1 s is dropped without a reply.

| Status and body | When |
| --- | --- |
| 200 OK + a number or text | The command ran; the body is its result |
| 200 OK + `0` | Unknown command number (such as 11), or an unknown light action for command 2 |
| 400 Bad Request + `ERR` | No complete `?…~` frame, fewer than 2 commas, or a value outside the allowed range; nothing changes |
| No reply | The request headers didn't arrive within 1 s |

## Light modes

Command 13 picks one of three modes; the mode is saved to EEPROM, so the light comes back in the same mode after a power cut. The default is motion activated.

| Mode | Set with | What the light does | Motion and the timeout | Commands that can't override it |
| --- | --- | --- | --- | --- |
| 0 · always off | `?13,0,0~` | Fades off (about 2 s) and stays off | Still tracked for the status page; never switch the light | Fade on and set level never turn it on; a new level is kept for later |
| 1 · motion activated | `?13,0,1~` | Fades in when someone arrives, fades off after motionTimeout | Switch the light, as in earlier versions | While the room is empty and the light is off, set level only keeps the level and fade on is ignored |
| 2 · always on | `?13,0,2~` | Fades in to the on level (about 2 s) and stays on | Still tracked; never switch the light | Fade off and `?3,0,0~` are ignored; set level still changes the brightness |

- On switching, the light goes straight to what the new mode calls for. Switching to motion activated lights the room only if it is occupied right then; otherwise the light fades off.
- Sending the mode that is already active changes nothing.
- The **on level** is the brightness the light comes on at, by motion or in always on mode. It is the last non-zero level sent with command 3, held in RAM only, and 255 after a reboot. After a power cut in always on mode the light therefore comes back at full brightness.
- `?8,0,13~` reads the mode; `?12,0,1~` resets it to motion activated with the other settings.

## Commands

Every command, in short form; anything not listed replies 0. Commands 2 and 3 depend on the light mode, detailed in the next section.

| Request | Value range | What it does | Reply |
| --- | --- | --- | --- |
| `?2,0,0~` | — | Fades the light off (about 2 s from full); ignored in always on mode | the level the light ends at |
| `?2,0,1~` | — | Fades the light on to 255 (about 2 s) when the mode allows | the level the light ends at (0 when it stays off) |
| `?2,0,2~` | — | Reads the brightness: the level the light is on at (or fading to), and while it is off the on level it will come on at | 1–255, never 0 |
| `?2,0,3~` | — | Reads what the sensor last reported | 0 none, 1 static presence, 2 motion |
| `?2,0,4~` | — | New. Reads whether the light is on (on or fading on) or off (off or fading off) | 1 on, 0 off |
| `?3,0,N~` | 0–255 (higher = 255) | Sets the brightness; a non-zero N becomes the on level | the level set, or the kept on level when the light stays off |
| `?4,0,N~` | 0 to max − 1 cm | Sets radarRangeMin, the nearest detection distance | the new value |
| `?5,0,N~` | min + 1 to 1100 cm | Sets radarRangeMax, the farthest detection distance | the new value |
| `?6,0,N~` | 0 or 1 | Sets presenceHoldsLight: whether sitting still keeps the room occupied | the new value |
| `?7,0,N~` | 6–9999 s | Sets motionTimeout: time with no detection before the room is vacant | the new value |
| `?8,0,0~` | — | Reads all settings | `min~max~presence~timeout~mode`, e.g. `0~300~1~30~1` |
| `?8,0,N~` | 4, 5, 6, 7 or 13 | Reads the setting that command N sets | the value |
| `?9,0,N~` | 0 = all, 1–60 = newest N | Reads the rolling log | log lines |
| `?10,0,N~` | 0 or 1 | Shows radar and light status; 1 also reads the sensor's configuration back | status text |
| `?12,0,1~` | 1 only | Resets all settings, the light mode included, to the defaults | `0~300~1~30~1` |
| `?13,0,N~` | 0, 1 or 2 | **New.** Sets the light mode: 0 always off, 1 motion activated, 2 always on | the new mode |

`?8,0,0~` and `?12,0,1~` now reply with five fields instead of four; a client that splits on `~` and reads the first four keeps working.

## Light commands by mode

The mode decides whether a command may turn the light on or off; a brightness sent with command 3 is never lost.

| Command | Always off | Motion activated, room empty and light off | Motion activated, room occupied or light on | Always on |
| --- | --- | --- | --- | --- |
| `?3,0,N~` (N > 0) | Light stays off; N becomes the on level; replies N | Light stays off; N becomes the on level for the next detection; replies N | Light jumps to N; N becomes the on level; replies N | Light jumps to N; N becomes the on level; replies N |
| `?3,0,0~` | Light off at once; replies 0 | Light off at once; replies 0 | Light off at once; replies 0 (see below) | Ignored; replies the level it stays at |
| `?2,0,1~` | Ignored; replies 0 | Ignored; replies 0 | Fades on to 255; replies 255 | Fades on to 255; replies 255 |
| `?2,0,0~` | Fades off; replies 0 | Fades off; replies 0 | Fades off; replies 0 (see below) | Ignored; replies the level it stays at |

- **Command 3 jumps, command 2 fades.** `?3,0,N~` sets the level at once and cancels any fade; `?2,0,0~` and `?2,0,1~` fade at 8 ms per level.
- **Turned off while someone is in the room** (motion activated): the light stays off while the room is occupied, because motion only turns it on when someone arrives in an empty room. After the room has been vacant for motionTimeout, the next arrival fades it in again.
- **Room empty, light on** in motion activated mode only happens briefly, while the light fades off after the timeout; a level sent then is kept for next time and the fade carries on.
- `?2,0,1~` fades to full brightness but doesn't change the on level.
- **Reading the light:** `?2,0,2~` always returns a brightness, never 0. While the light is on it is the level the light is at or fading to; while it is off it is the on level the next motion or command will use. `?2,0,4~` returns 1 or 0 for on or off. The live level during a fade is on the status page (`?10,0,0~`).
- Every change and every ignored request is logged, for example `http: on level set to 100, light stays off (motion activated, room empty)` or `http: light stays on (always on mode)`.

## Settings

Five settings are stored in EEPROM and survive a reboot; each takes effect as soon as it is set.

| Setting | Set | Read | Range | Default | Effect |
| --- | --- | --- | --- | --- | --- |
| radarRangeMin (cm) | `?4,0,N~` | `?8,0,4~` | 0 to max − 1 | 0 | Nearest distance the sensor detects; sent to the sensor at once |
| radarRangeMax (cm) | `?5,0,N~` | `?8,0,5~` | min + 1 to 1100 | 300 | Farthest distance detected; set it to the far wall so a passing doorway is ignored |
| presenceHoldsLight | `?6,0,N~` | `?8,0,6~` | 0 or 1 | 1 | 1: sitting still (breathing) keeps the room occupied; 0: only motion does |
| motionTimeout (s) | `?7,0,N~` | `?8,0,7~` | 6–9999 | 30 | Time from the last detection to the room going vacant; includes the sensor's own 5 s hold |
| lightMode | `?13,0,N~` | `?8,0,13~` | 0, 1 or 2 | 1 | 0 always off, 1 motion activated, 2 always on |

- A set replies with the new value; an out-of-range value replies 400 ERR and changes nothing.
- Min must stay below max: to move the window further out, set max first; to bring it closer, set min first.
- `?12,0,1~` resets all five to the defaults, sends the default range to the sensor, switches the light to motion activated and replies `0~300~1~30~1`. Any other value replies 400 ERR, so a mistyped request can't wipe the settings.
- **Saving:** changes are written at most once every 5 minutes, to limit flash wear. Boot counts as a write, so the first save is at least 5 minutes after power-up. Several changes in one window cost one write, and nothing is written if a setting was changed back. A reboot before the write loses the pending change; the status page shows `EEPROM: pending, writes in Ns` meanwhile.
- **Upgrading:** settings are never carried over to new firmware. Each build stamps its settings with an ID from its compile date and time, so on the first boot after an upload the EEPROM is wiped and the defaults above are stored; the log shows `no settings from this firmware: EEPROM wiped, using defaults`. Missing or damaged settings are wiped the same way. Re-uploading a sketch the IDE didn't recompile keeps the old stamp, and so keeps the settings.

## Status and log replies

`?10,0,0~` returns live status instantly; `?10,0,1~` also reads back what the C4002 holds (about 1 s, reports paused meanwhile). This sample is from always on mode with the room vacant:

```text
sensor:              configured
reports:             1 ok, 0 bad checksum, last 37.0s ago
target:              motion (2)
presence target:     145 cm, energy 37
motion target:       210 cm, energy 55
room:                vacant
pwmLED0:             level 120, target 120, on level 120
settings:
  detect range:      0-300 cm
  presenceHoldsLight:1
  motionTimeout:     30 s
  lightMode:         2 (always on)
  EEPROM:            pending, writes in 260s
fixed configuration:
  report period:     0.2 s
  disappear delay:   5 s
  motion sens.:      high
  presence sens.:    medium
```

| Line | Meaning |
| --- | --- |
| sensor | `configured`, or `not answering yet, retrying` while the sensor is starting or missing |
| reports | Valid frames, frames with a bad checksum (a rising count means UART noise), and the age of the last detection report |
| target | The sensor's last state: none (0), static presence (1) or motion (2) |
| room | `vacant`, or `occupied` with the time since the last detection and the countdown to vacant; tracked in every mode |
| pwmLED0 | Current level, the level a fade is heading to, and the on level (was "motion level") |
| settings | The five stored settings, `lightMode` with its name, and `EEPROM: saved` or `pending, writes in Ns` |

`?9,0,0~` returns the rolling log (newest 60 entries, oldest first; `?9,0,N~` for the newest N), stamped with uptime. A typical mode change looks like this:

```text
+0d 00:00:40  always off: light fading off
+0d 00:00:40  http: lightMode set to 0
+0d 00:00:43  motion detected, light mode always off
+0d 00:00:43  http: on level set to 200, light stays off (always off mode)
+0d 00:00:43  motion activated: light fading in to 200
+0d 00:00:43  http: lightMode set to 1
+0d 00:01:08  no detection for 30s, light fading off
```

## Examples

Ready to paste; replace the host if yours differs.

```bash
# light modes
curl "http://BathroomLight.home:4000/?13,0,0~"   # always off
curl "http://BathroomLight.home:4000/?13,0,1~"   # motion activated
curl "http://BathroomLight.home:4000/?13,0,2~"   # always on
curl "http://BathroomLight.home:4000/?8,0,13~"   # read the mode

# brightness: in motion mode with nobody there this only sets the next on level
curl "http://BathroomLight.home:4000/?3,0,80~"
curl "http://BathroomLight.home:4000/?2,0,2~"    # brightness, also while off
curl "http://BathroomLight.home:4000/?2,0,4~"    # 1 = on, 0 = off

# settings and diagnostics
curl "http://BathroomLight.home:4000/?8,0,0~"    # min~max~presence~timeout~mode
curl "http://BathroomLight.home:4000/?10,0,0~"   # status
curl "http://BathroomLight.home:4000/?9,0,20~"   # newest 20 log lines
```

From the node API: `callPico('BathroomLight', 13, 2)` switches to always on and returns `2`; `callPico('BathroomLight', 8, 13)` returns the mode.

## Timing and hardware notes

| Item | Value |
| --- | --- |
| Motion fade-in | 20 ms per level, about 5 s from off to full |
| Command fades, mode-change fades, timeout fade-off | 8 ms per level, about 2 s full range |
| Sensor report period | 200 ms; the fade starts within about 0.2 s of motion |
| motionTimeout | 5 s sensor hold + the rest on the Pico (30 s default = 5 + 25) |
| PWM | 12-bit (0–4095) at 15 262 Hz on GP19 (physical pin 25), driving a TC4420 and MOSFET |
| Brightness curve | CIE 1931 lightness: level 128 looks about half as bright; level 1 is just visible |
| On-board LED | Lit during start-up, then a 100 ms blink for every command |
| EEPROM writes | At most one every 5 minutes |

The Pico W's 3.3 V regulator is held in PWM mode at boot to keep its ripple low. Wi-Fi network, password and hostname live in `creds.h`.
