# SmartThermo API Command Reference

Oct 22, 2026 · @Ben

SmartThermo is a Pico W thermostat that replaces the Braeburn 2020NC on the single-stage gas furnace and central AC described in the *HVAC Thermostat Control Reference*. Three relays close R to W1, Y1 and G, and a DHT22 measures temperature and humidity. The Pico takes one command per HTTP request on port 4000 and replies with plain text, the same way BathroomLight does.

## Wiring

| Pico W pin | GPIO | Connects to | Contact |
| --- | --- | --- | --- |
| 21 | GP16 | G relay driver | R → G (indoor blower) |
| 22 | GP17 | Y1 relay driver | R → Y1 (compressor contactor) |
| 24 | GP18 | W1 relay driver | R → W1 (furnace heat call) |
| 23 | GND | Relay driver ground | |
| 29 | GP22 | DHT22 data, with a 10 kΩ pull-up to 3V3 | |
| 36 · 28 | 3V3(OUT) · GND | DHT22 power | |

- **The relays must be driven active high.** The firmware drives a pin HIGH to close a contact. Drive each coil through a 2N5551 NPN transistor (or a logic-level MOSFET), with a base resistor, a flyback diode across the coil and a 10 kΩ pull-down on the base or gate. The firmware sets the three relay pins to 12 mA drive strength (`gpio_set_drive_strength`, up from the 4 mA default) so each pin holds its HIGH level while it supplies the base current. An RP2040 pin is an input with its pull-down enabled from reset until the firmware runs, so with this wiring every relay stays open through power-up, a watchdog restart and a firmware upload. Do not use an "active low" relay board: its relays would close while the Pico is held in reset.
- **Why these pins.** The Pico W's Wi-Fi chip uses GP23–25 and GP29, GP26–28 are kept free for the ADC, and GP0–1 are the default debug UART. GP16, GP17 and GP18 sit next to a ground pin (23), and the RP2040 has no boot-strapping pins that could glitch at power-up.
- **Use mechanical relays with dry contacts** rated at least 1 A at 24 VAC, with each common wired to R and each normally-open contact to W1, Y1 or G. Never use a triac or SSR output, and never feed the Pico's own supply into an output. C is never switched. Power the Pico from R–C through an AC-input buck converter to 5 V.
- Wi-Fi network, password and hostname (`SmartThermo`) live in `creds.h`, which git ignores.

### Display

The display is the [Adafruit 1.44" 128x128 TFT](https://www.adafruit.com/product/2088) (ST7735R), on the RP2040's SPI1 bus. It runs on core 0, the core that runs the HTTP server.

| On the screen | What it shows | Updates |
| --- | --- | --- |
| Large number | Room temperature, rounded to whole °F, in the text colour. `--` with no usable reading: sensor starting, locked out or failed. A stale reading stays on screen | Every DHT22 sample (5 s) |
| Small number | Setpoint, in the text colour | At once |
| Fan icon | White fan on green while G is closed, dim otherwise. On heat in fan auto it stays dim, because the furnace runs its own blower | At once |
| Run icon | White play symbol while the furnace or AC runs: on orange heating, on blue cooling. A dim square otherwise, including while only the blower runs | At once |
| Mode icon | White flame on orange in heat, white snowflake on blue in cool, a dim power symbol while off | At once |

| Display pin | Pico W pin | GPIO | What it does |
| --- | --- | --- | --- |
| Vin | 36 | 3V3(OUT) | Power, shared with the DHT22 |
| GND | 18 | GND | Ground |
| RST | 15 | GP11 | Panel reset |
| D/C | 16 | GP12 | Data or command select |
| TCS | 17 | GP13 | Panel chip select |
| SCK | 19 | GP14 | SPI1 clock |
| SI | 20 | GP15 | SPI1 TX, data to the panel |
| Lite | 14 | GP10 | Backlight PWM |

- **Leave the other display pins unconnected.** SO and CCS are only for the microSD slot, which the firmware doesn't use, and 3.3Vout is an output.
- **Dimming.** The backlight rests at its default level, about 70% (180 of 255), with 3 kHz PWM on Lite. Levels are how bright it looks, not PWM duty: the firmware applies a gamma 2.8 curve, because the eye sees duty as far brighter than it is. Level 180 is about 38% duty, 128 about 15% and 26 about 0.17%. Only level 0 is off, and levels 1 to 13 all share the shortest pulse. An accepted change to the setpoint, hvac mode, fan mode or text colour, or a settings reset, fades it up to full (255, about 25 mA) in about 0.6 s. It holds for 30 s from the latest change, then fades back. `?6,0,N~` sets the default level from 0 (off) to 255, ends any hold and fades straight to it, and `?8,0,6~` reads it. The level isn't saved to EEPROM, so every start comes back at 180. `?12,0,1~` doesn't change it. A new level reaches the log only once it has stopped changing for 2 s, at most once a minute, so dragging a slider costs one entry; a level dragged back to the one last logged writes nothing.
- **Text colour.** `?7,0,N~` sets the temperatures' colour and shows it at once, and `?8,0,7~` reads it. It's saved to EEPROM with the other settings: 0 white (default), 1 silver, 2 red, 3 orange, 4 amber, 5 yellow, 6 lime, 7 green, 8 teal, 9 cyan, 10 sky, 11 blue, 12 purple, 13 magenta, 14 pink.
- **Image position.** `DISPLAY_ROTATION` in `display.h` turns the image for how the panel sits in the enclosure, with the DHT22 on the left. `DISPLAY_OFFSET_X` and `DISPLAY_OFFSET_Y` move it in pixels (+X right, +Y down, about 0.2 mm per pixel) to centre it in the circular cutout, which shows about 110 of the panel's 128 pixels across. Rebuild the firmware after changing them.
- **Why these pins.** They avoid the relays (GP16–18), the DHT22 (GP22), the Wi-Fi chip (GP23–25, GP29), the ADC pins (GP26–28) and the debug UART (GP0–1). The build stops with an error if a display pin in `display.h` is ever set to a relay or DHT22 pin.
- **Don't use the default `SPI` bus.** Its pins are GP16, GP18 and GP19, and starting it would take GP16 (G relay) and GP18 (W1 relay) away from the thermostat.
- **Keep the wires short.** The library clocks the bus at 32 MHz. If the picture is garbled, shorten the leads and keep them away from the 24 VAC wiring.
- **Font.** The temperatures use Josefin Sans SemiBold, the website's font, from `JosefinSansDigits.h`. To use FreeSansBold instead, which is larger and heavier, set `DISPLAY_FONT` in `display.h` to `DISPLAY_FONT_FREE_SANS_BOLD` and rebuild. Josefin Sans's digits have different widths, so its tens digit moves a little as the units digit changes.
- **Libraries.** Install **Adafruit ST7735 and ST7789 Library** from the Arduino Library Manager, along with the Adafruit GFX and Adafruit BusIO libraries it asks for. The fonts come with Adafruit GFX.
- **Drawing.** The screen is drawn into a 112 x 112 image in RAM (about 25 KB) and sent to the panel in one piece, only when something on it has changed, so it doesn't flicker.
- The HVAC doesn't depend on the display. The panel never answers the Pico, so a missing or unplugged display changes nothing but the screen.

## Safeguards

Every row marked **Controller** in the reference is enforced in two places. The thermostat logic follows the rules, and a final interlock (`applyInterlocks()`) checks every relay change against them. If the interlock ever overrides a change, that is a firmware bug: it is logged as `ERROR interlock: …`, counted, and shown under `errors:` in the status.

| Rule | Value | What the firmware does |
| --- | --- | --- |
| W1 and Y1 never together | | Both are held open if both are ever requested. Contacts always open before any contact closes, so the two are never closed at the same moment |
| Y1 never without G | | G closes before Y1 and opens after it. The interlock closes G if Y1 is ever requested without it |
| Compressor minimum off time | 5 min | Y1 can't close until 5 min after it last opened |
| Delay after power returns | 5 min | Power-up counts as heat and cool both just stopping, so neither can start for 5 min after boot |
| Compressor minimum on time | 5 min | Y1 stays closed at least 5 min, unless the mode is set to off or the sensor fails |
| Furnace minimum on time | 3 min | W1 stays closed at least 3 min, with the same two exceptions |
| Heat ↔ cool changeover | 5 min | W1 and Y1 must both be open for 5 min before the other stage can start |
| Starts per hour | heat 6, cool 4 | A start past the limit waits until the oldest start in the window is an hour old |
| Blower run-on after cool | 60 s | With fan auto, G stays closed for 60 s after a cool cycle ends normally. Off and a failed sensor skip the run-on |
| Blower on heat | | With fan auto, G stays open on heat: the furnace starts and stops its own blower |
| Relays open on boot | | All three relays are open before anything else runs. The saved mode is restored, and the timers above still apply |
| Relays open on a crash | 8 s / 30 s | The RP2040 hardware watchdog restarts the Pico if the HVAC core stops for 8 s or the network core for 30 s. The restart resets every pin, which opens every relay, and the status then reports `watchdog restart` (see *Safe mode* for repeated restarts) |
| Run limits | 1 h / 2 h, 3 h / 6 h | W1 and Y1 stop and rest 30 min after running with no temperature response, or at a hard cap (see *Run limits*). This goes beyond the reference, which has no maximum run time |
| Sensor not readable at startup | 60 s | Every relay stays open (startup lockout) until the sensor reads; in heat mode, the 25% fallback starts 10 min after boot |
| Sensor stale | 60 s | Nothing starts, and a running cycle stops after its minimum on time (see *Sensor failures*) |
| Sensor failed | 10 min | Heat runs at 25%; cool mode is switched off (see *Sensor failures*) |

## Wi-Fi

The HVAC doesn't depend on Wi-Fi: it keeps heating and cooling with no connection, from boot on. Wi-Fi is only needed to change settings and read the status.

- **Two cores.** Core 1 reads the sensor, runs the thermostat, drives the relays and feeds the watchdog. Core 0 runs Wi-Fi, the HTTP server and the EEPROM saves. A Wi-Fi attempt normally pauses core 0 for about 0.5 s, because `WiFi.beginNoBlock()` returns once the join is issued and the join and DHCP finish in the background (`WiFi.begin()` would pause it for up to 30 s). The library's wait has no firm limit if the join can't settle; the watchdog covers that. The HVAC on core 1 carries on meanwhile. One lock (`stateMutex`) guards everything both cores touch: settings, thermostat state, relays and the log. It is never held across a call to the Wi-Fi chip.
- **After boot.** If Wi-Fi hasn't connected, it's retried every 15 s until it does, for up to 5 min after boot. A first join can fail with the access point up, as seen after pulling the power. Only the first retry is logged, and these attempts don't count towards the 10 below. After 5 min the attempts below take over. Each attempt also pauses screen updates and the backlight fade while it runs, since they run on core 0.
- **Reconnecting.** A lost connection is logged as an error and retried at once, then every 2 min, up to 10 attempts. The connection coming back at any point clears the count.
- **Restart.** If all 10 attempts fail (about 20 min without Wi-Fi), the Pico restarts, but only once W1 and Y1 are both open, the 60 s blower run-on after cooling is over, and no setting is waiting for its EEPROM write. A heat or cool cycle already running finishes first, including its minimum on time. Waiting out the run-on keeps the blower from stopping while the coil is still cold. A fan-only run (fan on) doesn't hold the restart up. While a cycle holds it up, Wi-Fi is still retried every 2 min, and the connection coming back cancels the restart. After the restart all relays start open and the 5 min power-up delay applies. The log is cleared by the restart, so the next boot logs `ERROR SmartThermo restarted: Wi-Fi not reconnected after 10 attempts`, and the status shows `after Wi-Fi restart` until the next power-up.
- **During a long outage** the Pico restarts about every 27 min (5 min of quick attempts after boot, then 10 retries 2 min apart), or later if a cycle is running. Each restart adds the 5 min power-up delay before heat or cool can start, and the start counts per hour start again from zero.

## Run limits

These keep W1 and Y1 from running too long, whatever asks for them. Stops happen on the firmware's relay outputs, so a welded relay contact isn't covered, and there is no limit on total run time across many shorter cycles.

| Limit | When | What happens |
| --- | --- | --- |
| No response | W1 for 1 h, or Y1 for 2 h, without the room moving 1.0 °F the right way on fresh readings | The stage opens, cooling with its 60 s blower run-on, and both stages rest 30 min. The alarm stays in the status until the room responds to a later run or a mode is set |
| Hard cap | W1 for 3 h, or Y1 for 6 h, in one run, even while the room is improving | The same stop and rest |
| Frozen reading | The third no-response stop in a row, with the reading not moving 1.0 °F at any point since the first | **Heat:** the 25% fallback once the rest ends, 5 min on per 20, until the reading moves 1.0 °F. **Cool:** switched to off at once, with no run-on and saved, with the `cool was switched off by a sensor failure` error, until a mode is set |

- After a rest the call runs again as normal. A working sensor shows the room drifting, which resets the frozen-reading count, so a furnace lockout, a tripped AC breaker or equipment that can't keep up repeats its run and rest with the alarm showing, and never reaches the frozen-reading step.
- The longest the equipment can run on a reading stuck calling for more is 3 h in the first 4 h for W1, then 5 min per 20, and 6 h in the first 7 h for Y1, then off.
- Equipment that really can't keep up in extreme weather is limited to its run and rest pattern: at most 67% of the time for heat and 80% for cool. A cold or hot room during a rest is the cost of the limit.
- Setting any hvac mode ends a rest, the frozen-reading count, the fallback and the alarm. A stage's hard cap still counts from when it closed.
- A restart clears the rest, the run timers and the count, but it also opens the relays and adds the 5 min power-up delay. A cool shut-off is saved and survives it.
- A call waiting out the rest shows `call waiting: run limit rest, Ns left`. The log shows `ERROR heat: no temperature response for 3600s, resting 1800s` or `ERROR heat: run limit of 10800s reached, resting 1800s`.

## Safe mode

Every watchdog restart starts the 5 min power-up delay again, so a fault that hung core 0 soon after every boot could keep the HVAC from ever starting. Watchdog restarts in a row are counted in a watchdog scratch register for this.

- **Entering.** The third watchdog restart in a row boots into safe mode, logged as `ERROR safe mode: 3 watchdog restarts in a row, no Wi-Fi for 3600s`.
- **In safe mode** the thermostat, the relays with every rule above, the sensor and the display run as usual, with the saved settings. Nothing starts Wi-Fi or the API or touches the Wi-Fi chip, so the thermostat can't be reached. A stall on core 0 no longer restarts the Pico; a stall on core 1 still does.
- **Leaving.** After 1 h it starts Wi-Fi and the API without a restart, logging `safe mode: trying Wi-Fi again`, and watches core 0 again. A further stall comes straight back to safe mode, with the count kept.
- **The count** clears once Wi-Fi and the API have run for 10 min. A power cut or any restart other than the watchdog's also clears it.

The status shows the count on its `last start was a watchdog restart (firmware stalled), N in a row` line.

## Temperature control

- The setpoint is a whole number of degrees from 50 to 80 °F, with a default of 70 °F.
- The differential is 1.0 °F, centred on the setpoint. At 70 °F, heat comes on at 69.5 °F or below and goes off at 70.5 °F or above; cooling comes on at 70.5 °F or above and goes off at 69.5 °F or below. The differential is set by `TEMPERATURE_SWING_TENTHS_F` in the sketch.
- The DHT22 is read every 5 s. Each reading is checked against its checksum and the sensor's range (−40 to 80 °C, 0–100 % RH), and must then be believable:
    - It must be possible indoors, 20–120 °F with at least 1.0 % RH. This also rejects an all-zero frame, which passes its checksum.
    - A reading more than 5 °F from the last accepted one counts only once the next reading agrees with it. One bad frame can't start or stop the equipment, and a real change is accepted 5 s later.
    - A rejected reading counts as a failed read, so a sensor that keeps sending them goes stale and then failed.

## Sensor failures

Each sensor state is reported by `?2,0,2~` and the one-line status, shown under `errors:` in the full status, and logged as an `ERROR` line when the sensor enters it.

| State | `?2,0,2~` | When | What the HVAC does |
| --- | --- | --- | --- |
| ok | `0` | A valid reading within the last 60 s | Normal control |
| starting | `1` | No reading yet, under 60 s since boot | Heat and cool wait; the fan follows its mode |
| startup lockout | `2` | No reading in the first 60 s after boot | **Locked out: every relay open**, fan included. The lockout clears on the sensor's first valid reading, and normal control (with its timers) starts from there. In heat mode it becomes failed 10 min after boot; in off and cool it lasts until the sensor reads |
| stale | `3` | No valid reading for 60 s to 10 min | **Heat and cool paused:** nothing starts (the status shows `call waiting: sensor stale`), and a running furnace or compressor stops once its minimum on time is over, cooling with its 60 s blower run-on. The equipment never runs on an old reading |
| failed | `4` | No valid reading for over 10 min, or in heat mode none since boot for 10 min | **Heat mode:** W1 runs at 25%, 5 min on at the start of every 20 min, counted from the failure, with no time limit; the setpoint isn't used. **Cool mode:** switched to off at once (Y1 and G open, no run-on). The change is saved, and setting cool again is refused with 400 ERR until the sensor reads. **Off:** stays off |

- The fan mode works as usual in the stale and failed states (fan on keeps G closed).
- Every safeguard still applies to the 25% fallback: the furnace minimum on time, starts per hour (3 per hour, under the limit of 6) and the dead band.
- As soon as the sensor gives a valid reading again, the state returns to ok and heat goes back to normal control. Cool mode doesn't come back on its own: set it again. Until any hvac mode is set, the status shows `cool was switched off by a sensor failure` under `errors:`, also after a restart. The website's room status row doesn't show this: it reads Heat / AC Off.
- With heat mode saved, a dead sensor followed by a restart gives the startup lockout for the first 10 min after boot, then the 25% fallback. So a dead sensor can't leave the house with no heat, but the furnace does run with no feedback, in mild weather too, until someone notices the fault.
- Temperature and humidity replies are `NA` while there is no usable reading (starting, startup lockout, failed). While stale they give the last good reading.
- A failed read is logged when it follows a good one, and each state change is logged once, so a missing sensor doesn't flood the log.

## Modes

| Mode | Set with | W1 | Y1 | G |
| --- | --- | --- | --- | --- |
| 0 · off | `?4,0,0~` | open | open | closed only with fan on (fan only) |
| 1 · heat | `?4,0,1~` | closed while heat is called | open | fan on: closed. Fan auto: open (the furnace runs its own blower) |
| 2 · cool | `?4,0,2~` | open | closed while cooling is called | closed while cooling, plus a 60 s run-on with fan auto. Fan on: always closed |

| Fan mode | Set with | G |
| --- | --- | --- |
| 0 · auto | `?5,0,0~` | Closed only while cooling and for the 60 s run-on |
| 1 · on | `?5,0,1~` | Always closed, in every hvac mode, apart from the sensor startup lockout |

- Setting the mode to off opens W1 and Y1 at once, ignoring the minimum on times. This is the safety stop the reference allows.
- Switching straight between heat and cool lets the running stage finish its minimum on time, then waits out the 5 min dead band before the other stage starts.
- Changes take effect within about 10 ms of the reply, in the next pass of the HVAC core.

## Request format

Every request is an HTTP GET whose URL carries one command framed as `?<command>,<unused>,<value>~`, exactly as on BathroomLight.

- The node API's `callPico(target, arg1, arg2)` sends `http://<target>:4000/api?<arg1>,,<arg2>~`. The firmware ignores the path and the middle field.
- The command and the value must each be a whole number of 1 to 4 digits. A value left out, anything that isn't a digit or a longer number is rejected with 400 ERR, and nothing changes.
- Multi-line replies (commands 9 and 10) come back from `callPico()` with their lines run together, so read them with curl or a browser.
- One client is served at a time. A client that stops sending for 1 s, or takes longer than 2 s in total, is dropped without a reply.

| Status and body | When |
| --- | --- |
| 200 OK + a number or text | The command ran; the body is its result |
| 200 OK + `0` | Unknown command number |
| 400 Bad Request + `ERR` | No complete `?…~` frame, fewer than 2 commas, a command or value that isn't a whole number of 1 to 4 digits, an unknown reading, or a value out of range; nothing changes |
| No reply | The request didn't arrive within the time limits above |

## Commands

| Request | Value | What it does | Reply |
| --- | --- | --- | --- |
| `?2,0,0~` | — | Reads the temperature the thermostat controls on | °F with one decimal, e.g. `71.3` (the last reading while stale), or `NA` |
| `?2,0,1~` | — | Reads the humidity | % with one decimal, e.g. `45.2`, or `NA` |
| `?2,0,2~` | — | Reads the sensor state | `0` ok, `1` starting, `2` startup lockout, `3` stale, `4` failed |
| `?2,0,3~` | — | Reads the relays | `W1~Y1~G`, 1 = closed, e.g. `0~1~1` while cooling |
| `?2,0,4~` | — | Reads what the equipment is doing | `0` idle, `1` heating, `2` cooling, `3` fan only |
| `?3,0,N~` | 50–80 | Sets the setpoint, °F | the new setpoint |
| `?4,0,N~` | 0, 1 or 2 | Sets the hvac mode: 0 off, 1 heat, 2 cool. Cool is refused while the sensor has failed | the new mode, or 400 ERR |
| `?5,0,N~` | 0 or 1 | Sets the fan mode: 0 auto, 1 on | the new fan mode |
| `?6,0,N~` | 0–255 | Sets the display backlight's default level: 0 off, 255 full. Not saved to EEPROM | the new level |
| `?7,0,N~` | 0–14 | Sets the display's text colour (see Display). Saved to EEPROM | the new colour |
| `?8,0,0~` | — | Reads all settings | `mode~fan~setpoint`, e.g. `1~0~70` |
| `?8,0,N~` | 3, 4, 5, 6 or 7 | Reads the value that command N sets | the value |
| `?9,0,N~` | 0 = all, 1–60 = newest N | Reads the rolling log | log lines |
| `?10,0,0~` | 0 | Full status, including every current error | status text |
| `?11,0,0~` | 0 | One-line status, for a client that reads everything in one request | `mode~fan~setpoint~temperature~humidity~sensor~W1~Y1~G~running` |
| `?12,0,1~` | 1 only | Resets the settings to off, fan auto, 70 °F, white text | `0~0~70` |

Example `?11,0,0~` reply while heating: `1~0~70~69.4~41.0~0~1~0~0~1`, meaning heat mode, fan auto, setpoint 70 °F, 69.4 °F, 41.0 % humidity, sensor ok, W1 closed, Y1 and G open, heating. With the sensor failed and heat on its 25% fallback it reads `1~0~70~NA~NA~4~1~0~0~1`.

## Settings and EEPROM

| Setting | Set | Read | Range | Default |
| --- | --- | --- | --- | --- |
| hvac mode | `?4,0,N~` | `?8,0,4~` | 0 off, 1 heat, 2 cool | 0 off |
| fan mode | `?5,0,N~` | `?8,0,5~` | 0 auto, 1 on | 0 auto |
| setpoint (°F) | `?3,0,N~` | `?8,0,3~` | 50–80 | 70 |
| text colour | `?7,0,N~` | `?8,0,7~` | 0–14 | 0 white |

- All four settings are stored in EEPROM and restored after a power cut, so the thermostat comes back in the same mode, subject to the power-up delay and the sensor's first reading.
- **Writes happen at most once every 5 minutes**, to limit flash wear. Boot counts as a write, so the first save comes at least 5 minutes after power-up. Several changes in one window cost one write, and nothing is written if a setting is changed back. A power cut before the write loses the pending change; meanwhile the status shows `EEPROM: pending, writes in Ns`.
- **Upgrading:** as on BathroomLight, settings are never carried over to new firmware. The first boot after an upload wipes the EEPROM and starts **off**, fan auto, 70 °F, white text, and the log shows `no settings from this firmware: EEPROM wiped, using defaults (off)`. Set the mode again after every upload.

## Status reply

`?10,0,0~` returns the live status. A sample from heat mode, 4 minutes into a cycle:

```text
temperature:         69.6 F
humidity:            41.0 %
sensor:              ok, last reading 2s ago, 1440 ok, 3 failed
hvac mode:           1 (heat)
fan mode:            0 (auto)
setpoint:            70 F (heat on at 69.5 F, off at 70.5 F)
text color:          0 (white)
EEPROM:              saved
running:             heating
relays:              W1 closed, Y1 open, G open
heat:                on 241s
cool:                off 7200s
compressor:          ready
starts last hour:    heat 2 of 6, cool 0 of 4
Wi-Fi:               connected, IP 192.168.1.22, RSSI -58 dBm
uptime:              +0d 02:00:41, after power-up or reset
errors:              none
```

| Line | Meaning |
| --- | --- |
| temperature | The control temperature, `(last reading)` while stale, or `NA` |
| sensor | The sensor state, the age of the last valid reading and the read counters. While reads fail, a `last read:` line gives the failure (`no response`, `data timeout`, `bad checksum`, `reading out of range`, `implausible reading`, `unconfirmed jump`) and how many failed in a row |
| setpoint | The setpoint and the temperatures the current mode switches at, or `not used` while heat runs the 25% fallback |
| heat · cool | `on Ns` with any minimum on time still to run, `off Ns`, or `off Ns, call waiting: <reason>, Ns left` |
| compressor | `running`, `off-time lockout, Ns left` or `ready` |
| starts last hour | Starts within the last hour against each limit |
| Wi-Fi | Connected, or how long it has been down and the reconnect attempt, or a pending restart |
| uptime | Time since boot, and whether the last start was a watchdog restart or a Wi-Fi restart |
| errors | `none`, or one line per current error: startup lockout, sensor stale (with the time until it counts as failed), sensor failed (with what heat or cool is doing), cool switched off by a sensor failure, no temperature response, a run limit rest, a frozen reading, Wi-Fi not connected, interlock overrides, watchdog restart, Wi-Fi restart |

Reasons a call can wait: `sensor lockout`, `power-up delay`, `sensor starting`, `heat/cool dead band`, `compressor off time`, `starts per hour limit`, `run limit rest`.

The `errors:` section with the sensor failed in heat mode:

```text
errors:
  DHT22 failed, no reading for 912s (no response)
  heat running 25% fallback until the sensor reads again
```

## Log

`?9,0,0~` returns the rolling log: the newest 60 entries, oldest first, each stamped with the uptime (`?9,0,N~` returns the newest N). Every setting change, relay change, wait, sensor change, Wi-Fi change and EEPROM write is logged, and errors start with `ERROR`. Sample, ending with the sensor failing in cool mode:

```text
+0d 00:00:00  SmartThermo started, relays open
+0d 00:00:00  settings restored: mode cool, fan auto, setpoint 72 F
+0d 00:00:00  heat and cool wait 300s after power-up
+0d 00:00:00  connecting to Wi-Fi HomeNet
+0d 00:00:03  Wi-Fi connected, IP 192.168.1.22, RSSI -58 dBm
+0d 00:00:03  DHT22 reading: 74.1 F, 52.0 %
+0d 00:00:03  cool call waiting: power-up delay, 297s left
+0d 00:05:00  cool on (G, Y1 closed): 74.0 F, setpoint 72 F
+0d 00:21:12  cool off (Y1 opened) after 972s: setpoint reached, 71.5 F
+0d 00:22:12  fan off (G opened): fan auto, idle
+0d 01:40:05  DHT22 read failed: no response
+0d 01:41:00  ERROR DHT22 no reading for 60s (no response), heat/cool paused
+0d 01:50:00  ERROR DHT22 failed, no reading for 600s (no response)
+0d 01:50:00  ERROR DHT22 failed: cool mode switched off
```

Wi-Fi dropping for good, while heating:

```text
+0d 03:10:01  ERROR Wi-Fi connection lost, HVAC keeps running
+0d 03:10:01  Wi-Fi reconnect attempt 1 of 10
+0d 03:12:01  Wi-Fi reconnect attempt 2 of 10
  …
+0d 03:28:01  Wi-Fi reconnect attempt 10 of 10
+0d 03:30:01  ERROR Wi-Fi not back after 10 attempts: restart once heat and cool are off
+0d 03:30:01  Wi-Fi restart waiting for heat/cool, blower run-on and EEPROM write
+0d 03:34:12  heat off (W1 opened) after 498s: setpoint reached, 70.6 F
+0d 03:34:12  restarting: Wi-Fi not reconnected
```

A sensor that never reads after boot logs `ERROR DHT22 not readable at startup (no response): HVAC locked out`, and `DHT22 reading: …, startup lockout cleared` once it does. In heat mode it logs `ERROR DHT22 failed, no reading for 600s (no response)` 10 min after boot, as the 25% fallback starts.

## Examples

```bash
# modes and setpoint
curl "http://SmartThermo.home:4000/?4,0,1~"    # heat
curl "http://SmartThermo.home:4000/?4,0,2~"    # cool
curl "http://SmartThermo.home:4000/?4,0,0~"    # off
curl "http://SmartThermo.home:4000/?3,0,71~"   # setpoint 71 F
curl "http://SmartThermo.home:4000/?5,0,1~"    # fan on

# readings and diagnostics
curl "http://SmartThermo.home:4000/?11,0,0~"   # one-line status
curl "http://SmartThermo.home:4000/?10,0,0~"   # full status and errors
curl "http://SmartThermo.home:4000/?9,0,20~"   # newest 20 log lines
```

From the node API: `callPico('SmartThermo', 4, 1)` switches to heat and returns `1`, and `callPico('SmartThermo', 11)` returns the one-line status.

## Timing summary

| Item | Value | Constant |
| --- | --- | --- |
| Power-up delay | 5 min | `POWER_UP_DELAY_MS` |
| Compressor minimum off / on | 5 min / 5 min | `COMPRESSOR_MIN_OFF_MS`, `COMPRESSOR_MIN_ON_MS` |
| Furnace minimum on | 3 min | `FURNACE_MIN_ON_MS` |
| Heat ↔ cool dead band | 5 min | `CHANGEOVER_DEAD_BAND_MS` |
| Blower run-on after cool | 60 s | `COOL_FAN_RUN_ON_MS` |
| Starts per hour | heat 6, cool 4 | `HEAT_STARTS_PER_HOUR_MAX`, `COOL_STARTS_PER_HOUR_MAX` |
| Differential | 1.0 °F, centred | `TEMPERATURE_SWING_TENTHS_F` |
| Sensor read / stale / failed | 5 s / 60 s / 10 min | `DHT_READ_INTERVAL_MS`, `DHT_STALE_MS`, `DHT_FAILED_MS` |
| Startup lockout after | 60 s with no reading | `DHT_STARTUP_TIMEOUT_MS` |
| Believable reading | 20–120 °F, at least 1.0 % RH, a jump over 5 °F must repeat | `DHT_PLAUSIBLE_MIN_TENTHS_F`, `DHT_PLAUSIBLE_MAX_TENTHS_F`, `DHT_PLAUSIBLE_MIN_HUMIDITY_TENTHS`, `DHT_JUMP_LIMIT_TENTHS_F` |
| No temperature response | 1.0 °F in 1 h on heat, in 2 h on cool | `RESPONSE_MIN_CHANGE_TENTHS_F`, `HEAT_RESPONSE_WINDOW_MS`, `COOL_RESPONSE_WINDOW_MS` |
| Hard cap on one run | heat 3 h, cool 6 h | `HEAT_MAX_RUN_MS`, `COOL_MAX_RUN_MS` |
| Rest after a run limit | 30 min | `RUN_LIMIT_REST_MS` |
| Frozen reading | 3 no-response stops in a row | `FROZEN_READING_STOPS` |
| Safe mode | after 3 watchdog restarts in a row, for 1 h; the count clears after 10 min of networking | `SAFE_MODE_RESTART_COUNT`, `SAFE_MODE_NETWORK_RETRY_MS`, `RESTART_COUNT_STABLE_MS` |
| Heat fallback | 5 min on every 20 min (25%) | `HEAT_FALLBACK_ON_MS`, `HEAT_FALLBACK_PERIOD_MS` |
| Wi-Fi after boot | every 15 s until connected, for up to 5 min | `WIFI_STARTUP_RETRY_INTERVAL_MS`, `WIFI_STARTUP_WINDOW_MS` |
| Wi-Fi reconnect | every 2 min, 10 attempts, then restart when idle | `WIFI_RETRY_INTERVAL_MS`, `WIFI_RECONNECT_ATTEMPTS_MAX` |
| Watchdog | 8 s (HVAC core) / 30 s (network core) | `WATCHDOG_TIMEOUT_MS`, `CORE0_STALL_LIMIT_MS` |
| EEPROM writes | at most one every 5 min | `SETTINGS_SAVE_INTERVAL_MS` |
| On-board LED | lit during start-up, then a 100 ms blink per command | `ACTIVITY_LED_BLINK_MS` |

The timers run on the RP2040's 64-bit microsecond clock, so they never wrap. `millis()` wraps after 49.7 days.

## Source files

| File | Holds |
| --- | --- |
| `SmartThermo.ino` | The API routes (`runCommand()` and the command handlers), settings and EEPROM, Wi-Fi and the Wi-Fi restart, the rolling log, the status reply, `setup()` / `loop()` (core 0) and `setup1()` / `loop1()` (core 1) |
| `common.h` | Constants, the `Settings` block and the helpers (time, log, `stateMutex`) that the modules use from `SmartThermo.ino` |
| `http.cpp` · `http.h` | The HTTP server on port 4000, the `?<command>,<unused>,<value>~` decoder and the reply buffers; the decoder passes each command to the handler given to `startHttpServer()` |
| `thermo.cpp` · `thermo.h` | The DHT22 sensor, the thermostat decisions, the relays and their interlocks, and the thermostat's sections of the status; the relay and sensor GPIO pins are in `thermo.h` |
| `wdog.cpp` · `wdog.h` | The hardware watchdog, core 0's heartbeat and the restart cause kept across a restart |
| `display.cpp` · `display.h` · `JosefinSansDigits.h` | The TFT display on SPI1, its thermostat screen and its backlight PWM, started from `setup()` and updated from `loop()` on core 0; its GPIO pins, font choice and image position are in `display.h`, its layout, colours and icons in `display.cpp`, its default font's digits in `JosefinSansDigits.h` |
| `creds.h` | Wi-Fi network, password and hostname (git-ignored) |
| `test/` | Host test suite: runs `thermo.cpp`, `SmartThermo.ino` and `wdog.cpp` on simulated hardware with `g++` (no Arduino tools). Run `test/run_tests.sh`; see `test/README.md` |
