# SmartThermo host tests

Runs the real firmware (`thermo.cpp`, `SmartThermo.ino`, `wdog.cpp`) on a PC against simulated hardware, a simulated house and simulated weather. You only need `g++` (or `clang++`) on Linux or macOS. No Arduino tools or Pico are needed.

```bash
test/run_tests.sh
```

The script builds `test/build/thermo_test` and runs it. Any options are passed through, for example:

```bash
test/run_tests.sh --scenario seasons --season winter --days 14
```

It exits with 0 when every run passed and 1 otherwise.

## What runs

| Run | What it does |
| --- | --- |
| `units` | Unit tests of thermo.cpp's own functions:<br>• the pins and timers against the HVAC reference<br>• DHT22 decoding: the datasheet example, negative temperatures, the range limits and the checksum<br>• the DHT22 bit-banged read over a simulated data line, with every line fault<br>• the plausibility and jump checks on readings<br>• the read schedule<br>• the sensor states and their log lines<br>• the run limits<br>• the 25% heat fallback timing<br>• the 1°F switching temperatures<br>• every start blocker<br>• the starts-per-hour history<br>• the final interlock<br>• the relay pins' 12 mA drive and switching order<br>• the fan decision<br>• the safety stops<br>• the status text<br>• the names |
| `startup` | Power-up: relays open, EEPROM defaults, the 5 min power-up delay, the first heat call |
| `api-selections` | Every API command with every valid value and a set of invalid ones, checking each reply. Then:<br>• heat → cool → heat inside the minimum on times<br>• dead band and compressor off time<br>• off inside a minimum on time<br>• fan only<br>• reset to defaults |
| `fan-cool` · `fan-heat` | Fan auto and fan on while cooling, while heating and with the mode off. Checks the 60 s blower run-on after cooling |
| `sensor-missing` | DHT22 unplugged at power-up: startup lockout with every relay open while the mode is off, the 25% fallback once heat is set (past 10 min), cleared when it is plugged in |
| `sensor-dead-restart` | The DHT22 dies, then a power cut with heat saved: locked out for the first 10 min, then the 25% fallback |
| `sensor-intermittent` | 8% of DHT22 frames fail all day; single failures never make the sensor stale |
| `sensor-stale` | Short DHT22 dropouts while cooling: going stale stops the compressor (with its run-on) and holds new starts until a fresh reading arrives |
| `sensor-fail-heat` | DHT22 dead for 2 h in heat: going stale stops the furnace, failed after 10 min starts a measured 25% duty, cool mode is refused |
| `sensor-fail-cool` | DHT22 dies 1 min into a compressor run: the compressor is held to its 5 min minimum, then stops with its run-on. When the sensor counts as failed, cool is switched off and saved, and refused until it reads again. The status keeps the error after the sensor recovers, until the mode is set |
| `sensor-bad-data` | Each DHT22 failure kind in turn (data timeout, bad checksum, out of range, shorted line), then 15 min of garbage |
| `sensor-spike` | Valid but wrong single readings are rejected, so none starts or stops the furnace |
| `run-limit-heat-frozen` | The reading freezes low in heat: 1 h runs with 30 min rests, the 25% fallback after the third stop, normal control once the reading moves |
| `run-limit-cool-frozen` | The reading freezes high in cool: 2 h runs with 30 min rests and blower run-ons, cool off and saved after the third stop, setting a mode clears it |
| `run-limit-lockout` | A furnace lockout: 1 h runs with 30 min rests and the alarm, never counted as a frozen reading |
| `power-loss` · `power-blip-cool` | Power cuts while heating and while the compressor runs: settings restored, power-up delay, compressor protection |
| `power-loss-unsaved` | Power lost inside the 5 min EEPROM write limit: the last saved settings come back |
| `eeprom-corrupt` · `eeprom-write-fail` | Settings from another firmware build, flash bit rot, failed flash writes |
| `wifi-restart-wait` | The access point returns while a long furnace run holds the restart up: the retries continue, Wi-Fi returns and the restart is cancelled |
| `wifi-startup` | A first join that fails after a power cut: retries every 15 s for 5 min, then every 2 min, none of the quick ones counted or logged after the first |
| `wifi-short` · `wifi-restart` | Wi-Fi outages:<br>• the HVAC keeps running<br>• reconnects every 2 min<br>• after 10 failed attempts, the restart waits until heat and cool are off |
| `watchdog-core0` · `watchdog-core1` | Each core hangs: the watchdog restarts the Pico, with the relays open |
| `watchdog-loop` | Core 0 hangs after every boot: safe mode after 3 restarts, the furnace runs, a further hang restarts nothing, networking returns after 1 h and the count clears 10 min later |
| `short-cycle-cool` · `short-cycle-heat` | The setpoint flips every minute for 2 h: the minimum on/off times and starts-per-hour limits hold |
| `extreme-cold` · `heat-wave` | Two days of polar vortex or heat wave |
| `season-<season>` | `--days` of the season with a household schedule driven through the API: setbacks and a fan schedule. Spring and fall switch heat, off and cool every day |
| `season-faults-<season>` | The same, plus random realistic faults every day:<br>• sensor dropouts and noise<br>• Wi-Fi outages<br>• power cuts<br>• core hangs<br>• flash write errors<br>• wrong readings |

`--list` prints the same list.

## Safety rules checked on every run, all the time

These are checked on the GPIO pins themselves (what the equipment would see), on every relay change and after every pass of the control loop. They are written from the HVAC reference with their own constants, not taken from thermo.cpp:

- W1 and Y1 are never closed together.
- G closes before Y1, Y1 opens before G, and G runs on 60 s after a normal cool stop.
- A contact closes only on a pin set to 12 mA drive strength, for the 2N5551 driver's base.
- **Compressor (Y1):**
  - minimum 5 min off, also across restarts and power loss
  - 5 min dead band after heat, and 5 min after boot
  - closes only in cool mode, on a fresh reading (never while stale)
  - at most 4 starts an hour
  - runs at least 5 min unless switched off or the sensor failed
  - runs at most 6 h in one run
- **Furnace (W1):**
  - 5 min dead band after cooling, and 5 min after boot
  - closes only in heat mode, on a fresh reading or the failed-sensor fallback (never while stale)
  - at most 6 starts an hour
  - runs at least 3 min unless switched off
  - runs at most 3 h in one run
- During the startup lockout every relay is open. In heat mode the lockout ends in the failed-sensor fallback 10 min after boot. While the sensor is stale, a running furnace or compressor stops once its minimum on time is over. With a failed sensor, Y1 is open and cool mode is off.
- **Fan:**
  - mode off: W1 and Y1 open
  - fan on: G closed
  - fan auto: G open with heat and when idle
- The firmware's relay state matches the pins. The core mutex is released and interrupts are back on after every pass, and interrupts are never held off more than 8 ms. The final interlock never has to override the thermostat logic.
- EEPROM writes are at least 5 min apart. Boot counts as a write.
- **Comfort (normal runs):** the room is never more than 2°F past the setpoint while the furnace or compressor sits idle. This is checked once settings, restarts and the sensor have been steady for 90 min.

Scenarios add their own expectations on the API replies, relays, firmware log, EEPROM flash and restarts.

## Options

| Option | Default | |
| --- | --- | --- |
| `--scenario NAMES` | `all` | Comma-separated: `all`, `units`, `scenarios` (every fixed scenario), `seasons`, or scenario names |
| `--days N` | 3 | Length of season runs. 50 or more also crosses the 49.7-day `millis()` wrap |
| `--season S` | `all` | `winter`, `spring`, `summer`, `fall` or `all`, for season runs |
| `--fan PLAN` | `schedule` | Season runs: `schedule` (fan on 3 h every other day), `auto` or `on` |
| `--setpoint F` | schedule | Season runs: hold one setpoint (50-80) |
| `--seed N` | 1 | Weather, sensor noise and random faults. The same seed gives the same run |
| `--step-ms N` | 1000 | Simulated time between loop passes (10-1000). The firmware's loop is 10 ms; 1000 is much faster and its timers are all seconds or longer |
| `--sensor-log N` | 1 | Trace every Nth DHT22 reading; faulted reads are always traced |
| `--snapshot N` | 15 | Minutes between WORLD lines |
| `--out DIR` | `test/output` | Output folder |

## Output

Each run writes these files to `test/output/` (git-ignored):

- **`<run>.log`**: the detailed trace. Every line starts with the simulated day and time, then the Pico's uptime.
- **`<run>.csv`**: one row per simulated minute, for plotting. Columns: outdoor, indoor air, structure and at-sensor temperatures, humidity, setpoint, mode, fan, W1/Y1/G, sensor state, and whether the Pico is on.
- **`summary.txt`**: one line per run, plus the first failures.

Trace line categories:

| Category | Holds |
| --- | --- |
| `SENSOR` | Every DHT22 reading:<br>• the room and at-sensor temperatures<br>• the 5 bytes the sensor sent, decoded<br>• any injected fault<br>• what the firmware made of it and the sensor state |
| `GPIO` | Every relay pin write, with all three contacts after it, and the relay pins' drive strength |
| `FW` | Every firmware log line |
| `API` | Every request and its reply (status and log replies in full) |
| `WORLD` | Outdoor temperature, indoor air / structure / at-sensor temperatures, humidity, mode, fan, setpoint, relays, sensor state, and furnace and coil output |
| `EEPROM` · `WIFI` · `WDOG` · `BOOT` · `RESET` | The flash, the access point, the watchdog, boots and resets |
| `EVENT` | Injected faults and outages |
| `STATUS` | The full status reply at the end of every day and of the run |
| `DAY` | Per-day and whole-run summary:<br>• temperatures and humidity<br>• heat, cool and fan run time and starts<br>• comfort<br>• sensor reads<br>• EEPROM writes<br>• API requests and boots |
| `EXPECT` · `FAIL` | Scenario expectations met, and any broken rule or expectation |

## Independent audit

```bash
python3 test/audit_logs.py test/output
```

The audit re-reads every trace and checks the behaviour a second time, from the log text alone and without the C++ checker:

- the relay timings, order and starts per hour
- the switching temperatures against the setpoint
- every DHT22 frame decoded again, put through the plausibility and jump checks again, and compared with what the firmware reported
- the stale, failed and lockout timing
- the blower run-on
- EEPROM spacing and contents, and the settings restored after a restart
- Wi-Fi retry spacing and watchdog reset timing

It prints any problems, then the measured values (minimum, median and maximum) and event counts.

## How it works

| Part | What it does |
| --- | --- |
| `mocks/` | Stand-ins for `Arduino.h`, `WiFi.h`, `EEPROM.h`, `neotimer.h` and the pico-sdk GPIO, timer, watchdog and mutex headers. `sim_creds.h` holds test Wi-Fi credentials; the script points the sketch copy's `#include "creds.h"` at it, so the real one is never used |
| `sim_hal.cpp` | Simulated hardware:<br>• the clock: `time_us_32()` costs 1 µs per call, so the DHT22 busy-wait timing is real<br>• the GPIO pins<br>• a DHT22 that answers the start signal with real pulse lengths<br>• the flash sector<br>• an access point<br>• the watchdog<br>• the HTTP reply buffers in place of `http.cpp` |
| `sim_house.cpp` | The weather: daily fronts and a curve coldest at 04:00, warmest at 16:00, with clouds.<br>The house (1,500 sq ft, two thermal masses) gains heat from sun, people and cooking and loses it to outdoors. Equipment and sensor:<br>• furnace: 48,000 BTU/h, with heat exchanger lag<br>• AC: 2.5 ton, with coil lag and dehumidification<br>• the DHT22 lags the air by about 90 s |
| `thermo_test.cpp` | Each boot runs in a forked child process. A power cut, watchdog reset or the Wi-Fi restart starts the firmware from its pristine globals, while the house, the clock, the flash and the watchdog scratch registers carry on in shared memory. Both cores' loops run in turn each step; a `delay()` or a Wi-Fi join holds only its own core |
| `thermo_units.cpp` | Includes `thermo.cpp` directly, so its internal functions can be unit tested; the scenarios link against this copy |

The HTTP decoder in `http.cpp` is not compiled here: requests reach the API routes (`runCommand()`) already decoded, as `http.cpp` hands them over. Nor are the real Wi-Fi library, lwIP or flash, so their behaviour is only what the mocks model.
