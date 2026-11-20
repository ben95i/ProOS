# BathroomLight API Command Reference

Nov 15, 2026 · @Ben

The Pico W light takes one command per HTTP request on port 4000 and replies with plain text. New in this version: a second LED. **LED0** is the dimmable PWM light on GP19; **LED1** is an on/off light on GP18 (physical pin 24). Each LED has its own light mode, its own trigger distance and its own off delay, so LED0 can light the room from a few feet away while LED1 only comes on when someone is right at the sensor. LED0 keeps its command numbers; the nearest-distance setting (command 4) is gone, as detection now always starts at 0 cm.

## Request format

Every request is an HTTP GET whose URL carries one command framed as `?<command>,<unused>,<value>~`.

- The node API's `callPico(target, arg1, arg2)` sends `http://<target>:4000/api?<arg1>,,<arg2>~` and strips the newlines from the reply. The firmware ignores the path and the middle field, so `/api?13,,2~` and `/?13,0,2~` do the same thing; this sheet uses the short form.
- Leaving out `arg2` sends an empty value, which counts as 0: `callPico('BathroomLight', 8)` reads all settings, but `callPico('BathroomLight', 2)` fades LED0 off.
- Multi-line replies (commands 9 and 10) come back from `callPico()` with their lines run together, so read those with curl, a browser or `callPicoText()`.
- Each field is a whole number of up to 4 digits; extra digits are cut off. A negative value is read as a very large number: settings reject it with 400 ERR and command 3 treats it as 0.
- Quote the URL in the shell, because zsh and bash can treat `?` as a wildcard. One client is served at a time; a client that stops sending for 1 s is dropped without a reply.

| Status and body | When |
| --- | --- |
| 200 OK + a number or text | The command ran; the body is its result |
| 200 OK + `0` | Unknown command number (such as 4 or 11), or an unknown light action for command 2 |
| 400 Bad Request + `ERR` | No complete `?…~` frame, fewer than 2 commas, an unknown action for command 14 or 18, or a value outside the allowed range; nothing changes |
| No reply | The request headers didn't arrive within 1 s |

## Zones: how each LED reacts to the sensor

Every 0.1 s the C4002 reports an overall state (none, static presence or motion) and two targets it tracks side by side: the strongest **motion target** and the strongest **presence target**, each with a distance and an energy (0 when there is none). It reports the motion target even while its overall state reads static presence, as when someone stands still at the sink and moves a hand. Each LED's **zone** reaches from the sensor out to the LED's trigger distance, and is occupied while something has been detected inside it within the LED's off delay.

- **Arriving:** the motion target inside a zone occupies it, whatever the overall state says, so a hand moved near the sensor counts while someone is standing in the room. With the defaults, walking in at 4 ft occupies LED0's zone (0–150 cm) only; stepping up to the sink at 12 in occupies LED1's zone (0–30 cm) too, as anything inside LED1's zone is also inside LED0's.
- **Staying:** an occupied zone stays occupied while a target is within its **hold distance**:
  - presenceHoldsLight on: the farther of the two trigger distances (150 cm by default), counting the motion target and the presence target (sitting or standing still). Anyone there keeps every light that is on lit, whichever light they first turned on.
  - presenceHoldsLight off: the LED's own trigger distance, counting the motion target only.

  Still presence never occupies an empty zone: turning a light on always takes motion within its own distance.
- **Leaving:** a zone goes vacant once nothing has been detected within its hold distance for the LED's off delay. With presenceHoldsLight on, stepping back from the sink keeps LED1 on until you are beyond LED0's distance, and then each LED counts its own off delay. With it off, stepping back from the sink starts LED1's off delay at once while LED0 stays on. Leaving the sensor's view altogether adds the sensor's own 5 s hold, because it keeps reporting the target that long. An off command in motion activated mode empties the LED's zone at once (see *On and off commands by mode*).

In short, checked for each LED on every report:

```text
arrive:  zone empty    and motion target within the LED's trigger distance     -> occupied
hold:    zone occupied and (presenceHoldsLight on:  motion or presence target within the farther trigger distance
                            presenceHoldsLight off: motion target within the LED's trigger distance)
                                                                                -> off delay restarts
leave:   zone occupied and nothing held it for the LED's off delay             -> vacant (motion activated: LED off)
off:     motion activated, zone occupied, and an off command (?2,0,0~ ?14,0,0~ ?18,0,0~) -> vacant at once
```
- **Sensor range:** the sensor is told to detect from 0 cm out to the farther of the two trigger distances (150 cm by default), so movement beyond both zones, such as someone passing the doorway, is ignored. It is sent again when a change moves the farther distance.
- **Nobody reported:** while the overall state is none, the targets are ignored, even if their fields still hold old values.
- **Right at the sensor:** a target at 0 cm (energy above 0) is real and counts for every zone.
- **Unreadable report:** a report whose layout the firmware doesn't recognise (the status page then says `unknown report layout`) falls back to the overall state, placed at the edge of the sensor's range, so it can only reach the LED with the farthest trigger distance.
- **Resolution:** the firmware sets the sensor to 20 cm gates at boot, so it places targets to within about 20 cm out to 4.9 m. In its default 80 cm gates it can't place anything closer than about 80 cm, so a short trigger distance never fires; 4.9 m is also why trigger distances stop at 490 cm.
- Zones are tracked in every light mode, so the status page always shows them.

## Light modes

Each LED has its own mode, saved to EEPROM, so each comes back in the same mode after a power cut. The default for both is motion activated.

| Mode | LED0 / LED1 set with | What the LED does | Its zone | Commands that can't override it |
| --- | --- | --- | --- | --- |
| 0 · always off | `?13,0,0~` / `?17,0,0~` | Turns off (LED0 fades, about 2 s) and stays off | Still tracked; never switches the LED | On commands and set level never turn it on; a new LED0 level is kept for later |
| 1 · motion activated | `?13,0,1~` / `?17,0,1~` | Turns on when someone arrives in its zone, off after its off delay | Switches the LED | While its zone is empty and the LED is off, on commands are ignored and set level only keeps the level |
| 2 · always on | `?13,0,2~` / `?17,0,2~` | Turns on (LED0 fades in to its on level, about 2 s) and stays on | Still tracked; never switches the LED | Off commands and `?3,0,0~` are ignored; set level still changes LED0's brightness |

- On switching, the LED goes straight to what the new mode calls for. Switching to motion activated lights it only if its zone is occupied right then; otherwise it turns off.
- Sending the mode that is already active changes nothing.
- The **on level** is the brightness LED0 comes on at, by motion or in always on mode. It is the last non-zero level sent with command 3, held in RAM only, and 255 after a reboot. LED1 has no level.

## Commands

Every command, in short form; anything not listed replies 0. The on and off commands depend on the LED's mode, detailed in the next section.

| Request | Value range | What it does | Reply |
| --- | --- | --- | --- |
| `?2,0,0~` | — | Fades LED0 off (about 2 s from full); ignored in always on mode | the level LED0 ends at |
| `?2,0,1~` | — | Fades LED0 on to 255 (about 2 s) when its mode allows | the level LED0 ends at (0 when it stays off) |
| `?2,0,2~` | — | Reads LED0's brightness: the level it is on at (or fading to), and while it is off the on level it will come on at | 1–255, never 0 |
| `?2,0,3~` | — | Reads what the sensor last reported | 0 none, 1 static presence, 2 motion |
| `?2,0,4~` | — | Reads whether LED0 is on (on or fading on) or off (off or fading off) | 1 on, 0 off |
| `?3,0,N~` | 0–255 (higher = 255) | Sets LED0's brightness; a non-zero N becomes the on level | the level set, or the kept on level when LED0 stays off |
| `?5,0,N~` | 1–490 cm | Sets LED0's trigger distance | the new value |
| `?6,0,N~` | 0 or 1 | Sets presenceHoldsLight for both LEDs: whether anyone within the farther trigger distance, moving or still, keeps a lit LED on | the new value |
| `?7,0,N~` | 1–9999 s | Sets LED0's off delay | the new value |
| `?8,0,0~` | — | Reads all settings | `presence~LED0 distance~LED0 delay~LED0 mode~LED1 distance~LED1 delay~LED1 mode`, e.g. `1~150~30~1~30~15~1` |
| `?8,0,N~` | 5, 6, 7, 13, 15, 16 or 17 | Reads the setting that command N sets | the value |
| `?9,0,N~` | 0 = all, 1–60 = newest N | Reads the rolling log | log lines |
| `?10,0,N~` | 0 or 1 | Shows sensor, zone and LED status; 1 also reads the sensor's configuration back | status text |
| `?12,0,1~` | 1 only | Resets all settings, both modes included, to the defaults | `1~150~30~1~30~15~1` |
| `?13,0,N~` | 0, 1 or 2 | Sets LED0's light mode: 0 always off, 1 motion activated, 2 always on | the new mode |
| `?14,0,0~` | — | **New.** Turns LED1 off; ignored in always on mode | LED1's state: 1 on, 0 off |
| `?14,0,1~` | — | **New.** Turns LED1 on when its mode allows | LED1's state |
| `?14,0,2~` | — | **New.** Reads whether LED1 is on | 1 on, 0 off |
| `?15,0,N~` | 1–490 cm | **New.** Sets LED1's trigger distance | the new value |
| `?16,0,N~` | 1–9999 s | **New.** Sets LED1's off delay | the new value |
| `?17,0,N~` | 0, 1 or 2 | **New.** Sets LED1's light mode | the new mode |
| `?18,0,0~` | — | **New.** All off: sends each LED its own off command | `LED0~LED1` states, e.g. `0~0` |
| `?18,0,1~` | — | **New.** All on: sends each LED its own on command (LED0 to 255) | `LED0~LED1` states |
| `?18,0,2~` | — | **New.** Reads both LEDs' states | `LED0~LED1`, e.g. `1~0`: LED0 on, LED1 off |

Changed from the single-LED firmware, for clients to update:

- **Command 4 is gone** (it replies 0 and changes nothing), and `?8,0,4~` replies 400 ERR.
- **Command 5** was the farthest detection distance; it is now LED0's trigger distance, 1–490 cm.
- **Command 7** was motionTimeout, 6–9999 s including the sensor's 5 s hold; it is now LED0's off delay, 1–9999 s, counted from the last detection inside LED0's zone.
- **`?8,0,0~` and `?12,0,1~`** reply with seven fields in a new order (above) instead of `min~max~presence~timeout~mode`.

## On and off commands by mode

The mode decides whether a command may turn an LED on or off; a brightness sent with command 3 is never lost. "Zone empty" means nothing has been detected within the LED's trigger distance for its off delay.

| Command | Always off | Motion activated, zone empty and LED off | Motion activated, zone occupied or LED on | Always on |
| --- | --- | --- | --- | --- |
| `?3,0,N~` (N > 0) | LED0 stays off; N becomes the on level; replies N | LED0 stays off; N becomes the on level for the next detection; replies N | LED0 jumps to N; N becomes the on level; replies N | LED0 jumps to N; N becomes the on level; replies N |
| `?3,0,0~` | LED0 off at once; replies 0 | LED0 off at once; replies 0 | LED0 off at once; replies 0 (see below) | Ignored; replies the level it stays at |
| `?2,0,1~` | Ignored; replies 0 | Ignored; replies 0 | Fades on to 255; replies 255 | Fades on to 255; replies 255 |
| `?2,0,0~` | Fades off; replies 0 | Fades off; replies 0 | Fades off and empties LED0's zone; replies 0 (see below) | Ignored; replies the level it stays at |
| `?14,0,1~` | Ignored; replies 0 | Ignored; replies 0 | LED1 on; replies 1 | LED1 on; replies 1 |
| `?14,0,0~` | LED1 off; replies 0 | LED1 off; replies 0 | LED1 off and empties LED1's zone; replies 0 (see below) | Ignored; replies 1 |

- **All on and all off** (`?18,0,1~`, `?18,0,0~`) apply these rules to each LED on its own: with both in motion activated mode and someone standing 4 ft away, all on lights LED0 but leaves LED1 off, since nobody is in LED1's zone. Put LED1 in always on mode to keep it lit.
- **Command 3 jumps, command 2 fades.** `?3,0,N~` sets LED0's level at once and cancels any fade; `?2,0,0~` and `?2,0,1~` fade at 8 ms per level. LED1 always switches at once.
- **Turned off while someone is in the zone** (motion activated): an off command (`?2,0,0~`, `?14,0,0~`, or `?18,0,0~` for both) turns the LED off (LED0 fades, LED1 switches) and empties its zone at once, ending its off delay countdown, so the next motion within its trigger distance turns it on again; someone still moving there brings it straight back on. `?3,0,0~` turns LED0 off but leaves its zone as it is, so LED0 stays off while the zone is occupied, because motion only turns it on when someone arrives in an empty zone; after the zone has been empty for the off delay, the next arrival turns it on again.
- **Zone empty, LED0 on** in motion activated mode only happens briefly, while LED0 fades off after its off delay; a level sent then is kept for next time and the fade carries on.
- `?2,0,1~` fades LED0 to full brightness but doesn't change the on level.
- **Reading the LEDs:** `?2,0,2~` always returns LED0's brightness, never 0. `?2,0,4~`, `?14,0,2~` and `?18,0,2~` return 1 or 0 for on or off; LED0 counts as on from the moment a fade on starts and as off from the moment a fade off starts. The live level during a fade is on the status page (`?10,0,0~`).
- Every change and every ignored request is logged, for example `http: LED0 on level set to 100, stays off (motion activated, zone empty)` or `http: LED1 stays on (always on mode)`.

## Settings

Seven settings are stored in EEPROM and survive a reboot; each takes effect as soon as it is set.

| Setting | Set | Read | Range | Default | Effect |
| --- | --- | --- | --- | --- | --- |
| presenceHoldsLight | `?6,0,N~` | `?8,0,6~` | 0 or 1 | 1 | Both LEDs. 1: anyone within the farther trigger distance, moving or still (breathing), keeps every lit LED on; 0: only motion within an LED's own distance keeps it on |
| LED0 trigger distance (cm) | `?5,0,N~` | `?8,0,5~` | 1–490 | 150 (about 5 ft) | Motion up to this distance turns LED0 on |
| LED0 off delay (s) | `?7,0,N~` | `?8,0,7~` | 1–9999 | 30 | Time from the last detection inside LED0's zone to LED0 turning off |
| LED0 light mode | `?13,0,N~` | `?8,0,13~` | 0, 1 or 2 | 1 | 0 always off, 1 motion activated, 2 always on |
| LED1 trigger distance (cm) | `?15,0,N~` | `?8,0,15~` | 1–490 | 30 (about 12 in) | Motion up to this distance turns LED1 on |
| LED1 off delay (s) | `?16,0,N~` | `?8,0,16~` | 1–9999 | 15 | Time from the last detection inside LED1's zone to LED1 turning off |
| LED1 light mode | `?17,0,N~` | `?8,0,17~` | 0, 1 or 2 | 1 | 0 always off, 1 motion activated, 2 always on |

- A set replies with the new value; an out-of-range value replies 400 ERR and changes nothing.
- The two trigger distances are independent: either may be the larger. The sensor detects out to the larger one.
- The sensor places targets in 20 cm gates (`resolution` on `?10,0,1~` shows what it holds), so a trigger distance is only as fine as that.
- `?12,0,1~` resets all seven to the defaults, sends the default range (0–150 cm) to the sensor, switches both LEDs to motion activated and replies `1~150~30~1~30~15~1`. Any other value replies 400 ERR, so a mistyped request can't wipe the settings.
- **Saving:** changes are written at most once every 5 minutes, to limit flash wear. Boot counts as a write, so the first save is at least 5 minutes after power-up. Several changes in one window cost one write, and nothing is written if a setting was changed back. A reboot before the write loses the pending change; the status page shows `EEPROM: pending, writes in Ns` meanwhile.
- **Upgrading:** settings are never carried over to new firmware. Each build stamps its settings with an ID from its compile date and time, so on the first boot after an upload the EEPROM is wiped and the defaults above are stored; the log shows `no settings from this firmware: EEPROM wiped, using defaults`. Missing or damaged settings are wiped the same way. Re-uploading a sketch the IDE didn't recompile keeps the old stamp, and so keeps the settings.

## Status and log replies

`?10,0,0~` returns live status instantly; `?10,0,1~` also reads back what the C4002 holds (about 1 s, reports paused meanwhile). This sample has someone at the sink:

```text
sensor:              configured
reports:             812 ok, 0 bad checksum, last 0.1s ago
target:              motion (2)
presence target:     28 cm, energy 41
motion target:       25 cm, energy 63, speed -40 cm/s
LED0 zone:           occupied (held within 150 cm), last detection 0s ago, vacant in 29s
LED0:                level 255, target 255, on level 255
LED1 zone:           occupied (held within 150 cm), last detection 0s ago, vacant in 14s
LED1:                on
settings:
  presenceHoldsLight:1
  LED0:              mode 1 (motion activated), within 150 cm, off delay 30 s
  LED1:              mode 1 (motion activated), within 30 cm, off delay 15 s
  detect range:      0-150 cm (sent to the sensor)
  EEPROM:            saved
fixed configuration:
  report period:     0.1 s
  resolution:        20 cm gates
  disappear delay:   5 s
  motion sens.:      high
  presence sens.:    medium
```

| Line | Meaning |
| --- | --- |
| sensor | `configured`, or `not answering yet, retrying` while the sensor is starting or missing |
| reports | Valid frames, frames with a bad checksum (a rising count means UART noise), and the age of the last detection report |
| target | The sensor's last state: none (0), static presence (1) or motion (2) |
| presence / motion target | Each target the zones are checked against: its distance, its energy (0 = none), and the motion target's speed |
| LED0 zone · LED1 zone | `vacant` with the LED's trigger distance, or `occupied` with its hold distance, the time since the last detection within it and the countdown to vacant; tracked in every mode |
| LED0 | Current level, the level a fade is heading to, and the on level |
| LED1 | `on` or `off` |
| settings | The stored settings, each LED's on one line, the range sent to the sensor, and `EEPROM: saved` or `pending, writes in Ns` |

`?9,0,0~` returns the rolling log (newest 60 entries, oldest first; `?9,0,N~` for the newest N), stamped with uptime. Walking up to the sink and back out looks like this:

```text
+0d 00:10:02  motion at 140cm: LED0 fading in to 255
+0d 00:10:06  motion at 30cm: LED1 on
+0d 00:11:05  nothing within 150cm for 15s: LED1 off
+0d 00:11:20  nothing within 150cm for 30s: LED0 fading off
+0d 00:11:35  always on: LED1 on
+0d 00:11:35  http: LED1 lightMode set to 2
```

## Examples

Ready to paste; replace the host if yours differs.

```bash
# LED1
curl "http://BathroomLight.home:4000/?14,0,1~"   # on (when its mode allows)
curl "http://BathroomLight.home:4000/?14,0,0~"   # off
curl "http://BathroomLight.home:4000/?14,0,2~"   # 1 = on, 0 = off

# both LEDs
curl "http://BathroomLight.home:4000/?18,0,1~"   # all on
curl "http://BathroomLight.home:4000/?18,0,0~"   # all off
curl "http://BathroomLight.home:4000/?18,0,2~"   # LED0~LED1, e.g. 1~0

# zones: LED0 from 5 ft, LED1 from 12 in
curl "http://BathroomLight.home:4000/?5,0,150~"
curl "http://BathroomLight.home:4000/?15,0,30~"
curl "http://BathroomLight.home:4000/?7,0,60~"   # LED0 off delay 1 min
curl "http://BathroomLight.home:4000/?16,0,10~"  # LED1 off delay 10 s

# light modes
curl "http://BathroomLight.home:4000/?13,0,1~"   # LED0 motion activated
curl "http://BathroomLight.home:4000/?17,0,2~"   # LED1 always on

# LED0 brightness: in motion mode with nobody in its zone this only sets the next on level
curl "http://BathroomLight.home:4000/?3,0,80~"
curl "http://BathroomLight.home:4000/?2,0,2~"    # brightness, also while off

# settings and diagnostics
curl "http://BathroomLight.home:4000/?8,0,0~"    # presence~LED0 distance~delay~mode~LED1 distance~delay~mode
curl "http://BathroomLight.home:4000/?10,0,0~"   # status
curl "http://BathroomLight.home:4000/?9,0,20~"   # newest 20 log lines
```

From the node API: `callPico('BathroomLight', 18, 2)` returns `1~0`; `callPico('BathroomLight', 8, 17)` returns LED1's mode.

## Timing and hardware notes

| Item | Value |
| --- | --- |
| LED0 motion fade-in | 20 ms per level, about 5 s from off to full |
| LED0 command fades, mode-change fades, off-delay fade-off | 8 ms per level, about 2 s full range |
| LED1 | Switches at once in every case |
| Sensor report period | 100 ms; an LED reacts within about 0.1 s of motion in its zone |
| Sensor resolution | 20 cm gates, out to 4.9 m |
| Off delays | Counted from the last detection inside the LED's zone; plus the sensor's 5 s hold when you leave its view altogether |
| LED0 PWM | 12-bit (0–4095) at 15 262 Hz on GP19 (physical pin 25), driving a TC4420 and MOSFET |
| LED1 output | On/off on GP18 (physical pin 24); GP18 shares GP19's PWM slice but stays a plain GPIO |
| Brightness curve | CIE 1931 lightness: level 128 looks about half as bright; level 1 is just visible |
| On-board LED | Lit during start-up, then a 100 ms blink for every command |
| EEPROM writes | At most one every 5 minutes |

The Pico W's 3.3 V regulator is held in PWM mode at boot to keep its ripple low. Wi-Fi network, password and hostname live in `creds.h`.
