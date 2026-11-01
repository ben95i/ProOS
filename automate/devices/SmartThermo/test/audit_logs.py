#!/usr/bin/env python3
"""Independent audit of the SmartThermo host-test traces: rebuilds the relays, settings, sensor
readings, EEPROM and Wi-Fi history from the .log text alone and re-checks the behaviour against the
HVAC reference, without using the C++ checker (a second implementation of the rules, so a bug in
one is caught by the other). Usage: python3 test/audit_logs.py test/output"""
import glob
import os
import re
import statistics
import sys
from collections import Counter, defaultdict

LINE = re.compile(r'^d(\d+) (\d\d):(\d\d):(\d\d)\.(\d{3})  (up (\d+):(\d\d):(\d\d)|power off)\s+(\S+)\s+(.*)$')
RELAYS = re.compile(r'\| W1 (open|closed), Y1 (open|closed), G (open|closed)$')
SENSOR = re.compile(r'room air ([-\d.]+) F, at sensor ([-\d.]+) F, RH ([-\d.]+) % \| (.*?) \| firmware: (\S+) F, (\S+) %, sensor (.+)$')
FRAME = re.compile(r'DHT22 sent ((?:[0-9A-F]{2} ){4}[0-9A-F]{2}) \(([-\d.]+) C, ([-\d.]+) %\)(?: (FAULT .*))?$')
SEC = 1000
MIN = 60 * SEC

def fw_decode(frame):
    """thermo.cpp's decodeDht22Bytes, re-implemented: returns ('ok', tenthsF, humTenths) or (result,)."""
    if (sum(frame[:4]) & 0xFF) != frame[4]:
        return ('bad checksum',)
    humidity = (frame[0] << 8) | frame[1]
    magnitude = ((frame[2] & 0x7F) << 8) | frame[3]
    tenths_c = -magnitude if frame[2] & 0x80 else magnitude
    if humidity > 1000 or tenths_c < -400 or tenths_c > 800:
        return ('reading out of range',)
    scaled = tenths_c * 9
    tenths_f = int(scaled / 5) + 320  # C++ integer division truncates toward zero
    return ('ok', tenths_f, humidity)

def tenths(text):
    return int(round(float(text) * 10))


class Run:
    def __init__(self, name):
        self.name = name
        self.problems = []
        self.notes = Counter()
        self.stats = defaultdict(list)

    def bad(self, when, text):
        self.problems.append(f'{fmt(when)} {text}')


def fmt(ms):
    day = ms // 86400000 + 1
    rest = ms % 86400000
    return f'd{day} {rest // 3600000:02d}:{rest // 60000 % 60:02d}:{rest // 1000 % 60:02d}.{rest % 1000:03d}'


def audit(path):
    run = Run(os.path.basename(path)[:-4])
    w1 = y1 = g = False
    boot_ms = None
    up = False
    mode, fan, setpoint = 0, 0, 70
    last_w1_rise = last_y1_rise = None
    last_w1_fall = last_y1_fall = -10**12
    y1_stop_normal = False
    heat_starts, cool_starts = [], []
    pending = []          # relay edges waiting for their FW reason line: (kind, time, duration)
    last_valid_ms = None  # world time of the last valid DHT22 reading
    last_good = None      # (tenthsF, humTenths) of the last valid reading
    last_read_ms = None
    sensor_state = None
    last_commit_up = None
    last_commit = None
    commit_seen = False
    fallback_on = []
    in_fallback = False
    wifi_attempt_ms = None
    hang = None
    first_reading_checked = False
    pending_starts = []   # W1 / Y1 starts judged against the sensor state of their own pass
    accepted_f10 = None   # the firmware's last accepted temperature this boot
    jump_f10 = None       # a far-off reading waiting to be repeated

    def believe(f10, h10):
        """thermo.cpp's checkReadingBelievable, re-implemented: returns 'ok' or the failure."""
        nonlocal accepted_f10, jump_f10
        if not 200 <= f10 <= 1200 or h10 < 10:
            return 'implausible reading'
        first = accepted_f10 is None
        if first or abs(f10 - accepted_f10) <= 50 or (jump_f10 is not None and abs(f10 - jump_f10) <= 50):
            accepted_f10 = f10
            jump_f10 = None
            return 'ok'
        jump_f10 = f10
        return 'unconfirmed jump'

    def check_starts():
        for relay, start_ms in pending_starts:
            allowed = ('ok',) if relay == 'Y1' else ('ok', 'failed')
            if sensor_state not in allowed:
                run.bad(start_ms, f'{relay} closed with the sensor {sensor_state} (only on a fresh reading'
                                  f'{"" if relay == "Y1" else " or the fallback"})')
        pending_starts.clear()

    def settings_text():
        return (mode, fan, setpoint)

    for raw in open(path, errors='replace'):
        m = LINE.match(raw.rstrip('\n'))
        if not m:
            continue
        day, hh, mi, ss, ms3 = (int(m.group(i)) for i in range(1, 6))
        t = ((((day - 1) * 24 + hh) * 60 + mi) * 60 + ss) * 1000 + ms3
        category, text = m.group(10), m.group(11)
        if category == 'FW' and text.startswith('ERROR DHT22 failed, no reading'):
            sensor_state = 'failed'  # with no reading since boot this needn't fall on a read
        if pending_starts and t > pending_starts[-1][1]:
            check_starts()
        uptime_ms = None if m.group(6) == 'power off' else t - boot_ms if boot_ms is not None else None

        if category == 'BOOT':
            boot_ms = t
            up = True
            last_read_ms = None
            commit_seen = False
            first_reading_checked = False
            wifi_attempt_ms = None
            accepted_f10 = None
            jump_f10 = None
            run.notes['boots'] += 1
            sensor_state = None
            continue

        if category == 'RESET' and not text.startswith(('W1 opened', 'Y1 opened', 'G opened')):
            kind = text.split(':')[0]
            run.notes['reset: ' + kind] += 1
            if 'RESTART requested' in text and (w1 or y1):
                run.bad(t, 'Wi-Fi restart with W1/Y1 closed')
            if hang is not None:
                delay = (t - hang[1]) / 1000
                run.stats[f'watchdog reset after core {hang[0]} hang (s)'].append(delay)
                hang = None
            if w1:
                last_w1_fall = t
            if y1:
                last_y1_fall = t
                y1_stop_normal = False
            w1 = y1 = g = False
            up = False
            pending.clear()
            continue

        if category == 'EVENT':
            hm = re.match(r'core (\d) HANGS', text)
            if hm and up:
                hang = (int(hm.group(1)), t)
            continue

        if category == 'GPIO':
            rm = RELAYS.search(text)
            if not rm:
                continue
            nw1, ny1, ng = (rm.group(i) == 'closed' for i in (1, 2, 3))
            if nw1 and ny1:
                run.bad(t, 'W1 and Y1 closed together')
            if ny1 and not y1:  # Y1 closes
                if not g:
                    run.bad(t, 'Y1 closed before G')
                if t - last_y1_fall < 300 * SEC - 1:
                    run.bad(t, f'Y1 closed {(t - last_y1_fall) / 1000:.1f} s after it opened')
                if t - last_w1_fall < 300 * SEC - 1:
                    run.bad(t, f'Y1 closed {(t - last_w1_fall) / 1000:.1f} s after W1 opened')
                if t - boot_ms < 300 * SEC - 1:
                    run.bad(t, 'Y1 closed inside the power-up delay')
                if mode != 2:
                    run.bad(t, f'Y1 closed in mode {mode}')
                pending_starts.append(('Y1', t))
                cool_starts = [s for s in cool_starts if t - s < 3600 * SEC] + [t]
                run.stats['cool starts in an hour'].append(len(cool_starts))
                if len(cool_starts) > 4:
                    run.bad(t, f'{len(cool_starts)} cool starts in an hour')
                last_y1_rise = t
                pending.append(('cool on', t, None))
            if y1 and not ny1:  # Y1 opens
                y1_stop_normal = None  # known once the firmware logs why
                pending.append(('cool off', t, t - last_y1_rise))
                last_y1_fall = t
            if nw1 and not w1:  # W1 closes
                if t - last_y1_fall < 300 * SEC - 1:
                    run.bad(t, f'W1 closed {(t - last_y1_fall) / 1000:.1f} s after Y1 opened')
                if t - boot_ms < 300 * SEC - 1:
                    run.bad(t, 'W1 closed inside the power-up delay')
                if mode != 1:
                    run.bad(t, f'W1 closed in mode {mode}')
                pending_starts.append(('W1', t))
                heat_starts = [s for s in heat_starts if t - s < 3600 * SEC] + [t]
                run.stats['heat starts in an hour'].append(len(heat_starts))
                if len(heat_starts) > 6:
                    run.bad(t, f'{len(heat_starts)} heat starts in an hour')
                last_w1_rise = t
                pending.append(('heat on', t, None))
            if w1 and not nw1:  # W1 opens
                pending.append(('heat off', t, t - last_w1_rise))
                last_w1_fall = t
            if g and not ng:  # G opens
                if ny1:
                    run.bad(t, 'G opened with Y1 closed')
                since = t - last_y1_fall
                if y1_stop_normal is None and since < 100:
                    run.notes['G opened with Y1 in the same pass (safety stop)'] += 1
                if y1_stop_normal and since < 60 * SEC - 1 and mode != 0:
                    run.bad(t, f'blower run-on cut to {since / 1000:.1f} s')
                if y1_stop_normal and since < 120 * SEC:
                    run.stats['blower run-on (s)'].append(since / 1000)
            if nw1 and ng and fan == 0:
                run.bad(t, 'G closed with W1 in fan auto')
            w1, y1, g = nw1, ny1, ng
            continue

        if category == 'EEPROM' and text.startswith('commit:'):
            cm = re.search(r'mode (\d+), fan (\d+), setpoint (\d+) F', text)
            stored = tuple(int(cm.group(i)) for i in (1, 2, 3))
            boot_wipe = uptime_ms < 1000 and not commit_seen
            if boot_wipe:
                mode, fan, setpoint = stored
            if stored != settings_text():
                run.bad(t, f'EEPROM wrote {stored} but the settings are {settings_text()}')
            if commit_seen and uptime_ms - last_commit_up < 300 * SEC - 1:
                run.bad(t, f'EEPROM writes {(uptime_ms - last_commit_up) / 1000:.1f} s apart')
            if not commit_seen and uptime_ms > 1000 and uptime_ms < 300 * SEC - 1:
                run.bad(t, f'first EEPROM write {uptime_ms / 1000:.1f} s after boot')
            if commit_seen:
                run.stats['EEPROM write spacing (s)'].append((uptime_ms - last_commit_up) / 1000)
            commit_seen = True
            last_commit_up = uptime_ms
            last_commit = stored
            continue

        if category == 'SENSOR':
            sm = SENSOR.match(text)
            if not sm:
                run.bad(t, 'unparsed SENSOR line')
                continue
            at_sensor = float(sm.group(2))
            sent, fw_t, fw_h, state = sm.group(4), sm.group(5), sm.group(6), sm.group(7)
            sensor_state = state
            if pending_starts:
                check_starts()
            if state == 'stale':
                if w1 and t - last_w1_rise > 180 * SEC + 10:
                    run.bad(t, f'the furnace runs {(t - last_w1_rise) / 1000:.0f} s into its cycle on a stale reading')
                if y1 and t - last_y1_rise > 300 * SEC + 10:
                    run.bad(t, f'the compressor runs {(t - last_y1_rise) / 1000:.0f} s into its cycle on a stale reading')
                run.notes['DHT22 reads while stale'] += 1
            if last_read_ms is not None and boot_ms is not None and last_read_ms >= boot_ms:
                gap = t - last_read_ms
                run.stats['DHT22 read spacing (ms)'].append(gap)
                if abs(gap - 5000) > 10:
                    run.bad(t, f'DHT22 reads {gap} ms apart')
            elif boot_ms is not None and not first_reading_checked:
                first_reading_checked = True
                if abs((t - boot_ms) - 2000) > 10:
                    run.bad(t, f'first DHT22 read {t - boot_ms} ms after boot')
            last_read_ms = t
            fmm = FRAME.match(sent)
            result = None
            if fmm:
                frame = [int(b, 16) for b in fmm.group(1).split()]
                fault = fmm.group(4)
                decoded = fw_decode(frame)
                if fault and 'stopped part way' in fault:
                    decoded = ('data timeout',)
                result = decoded[0]
                if fault is None and result != 'ok':
                    run.bad(t, f'a good frame decoded as {result}')
                if fault and 'bit flipped' in fault and result != 'bad checksum':
                    run.bad(t, 'a flipped bit passed the checksum')
                if fault and 'garbage' in fault and result != 'reading out of range':
                    run.bad(t, 'garbage passed the range check')
                if result == 'ok':
                    _, f10, h10 = decoded
                    sent_c10 = tenths(fmm.group(2))
                    if fault is None and (frame[2] & 0x80 == 0) and ((frame[2] << 8) | frame[3]) != abs(sent_c10):
                        run.bad(t, 'frame bytes and printed C disagree')
                    result = believe(f10, h10)
                    if result == 'ok':
                        if fw_t != 'NA' and (tenths(fw_t) != f10 or tenths(fw_h) != h10):
                            run.bad(t, f'firmware shows {fw_t} F {fw_h} % for a frame that decodes to {f10 / 10} F {h10 / 10} %')
                        run.stats['reading error vs sensor (F)'].append(f10 / 10 - at_sensor)
                        last_valid_ms = t
                        last_good = (f10, h10)
                    else:
                        run.notes['DHT22 readings rejected: ' + result] += 1
            else:
                result = 'no response' if 'unplugged' in sent or 'stuck low' in sent else 'data timeout'
            if result != 'ok':
                if state in ('ok', 'stale'):
                    if last_good is None or fw_t == 'NA' or (tenths(fw_t), tenths(fw_h)) != last_good:
                        run.bad(t, f'failed read: firmware shows {fw_t} F, not the last good reading {last_good}')
                elif fw_t != 'NA':
                    run.bad(t, f'sensor {state} but the temperature is {fw_t}, not NA')
                expected_state = None
                if last_valid_ms is not None and last_valid_ms >= (boot_ms or 0):
                    age = round((t - last_valid_ms) / 1000) * 1000
                    expected_state = 'failed' if age >= 600 * SEC else 'stale' if age >= 60 * SEC else 'ok'
                    if state != expected_state:
                        run.bad(t, f'sensor {state} with the last good reading {age / 1000:.0f} s old (expected {expected_state})')
            continue

        if category != 'FW':
            continue

        # ---- firmware log lines ----
        hm = re.match(r'http: setpoint set to (\d+) F', text)
        if hm:
            setpoint = int(hm.group(1))
        hm = re.match(r'http: hvac mode set to (\d)', text)
        if hm:
            mode = int(hm.group(1))
        hm = re.match(r'http: fan mode set to (\d)', text)
        if hm:
            fan = int(hm.group(1))
        hm = re.match(r'settings restored: mode (\w+), fan (\w+), setpoint (\d+) F', text)
        if hm:
            mode = {'off': 0, 'heat': 1, 'cool': 2}[hm.group(1)]
            fan = {'auto': 0, 'on': 1}[hm.group(2)]
            setpoint = int(hm.group(3))
            if last_commit is not None and last_commit != (mode, fan, setpoint):
                run.bad(t, f'restored {(mode, fan, setpoint)} but the flash last got {last_commit}')
            run.notes['settings restored after a reset'] += 1
        if text.startswith(('no settings from this firmware', 'EEPROM wipe failed', 'http: settings reset to defaults')):
            mode, fan, setpoint = 0, 0, 70
        if text.startswith(('ERROR DHT22 failed: cool mode switched off', 'ERROR reading frozen: cool mode switched off')):
            mode = 0
        if text.startswith('ERROR interlock'):
            run.bad(t, 'the final interlock overrode a relay request: ' + text)
        hm = re.match(r'ERROR DHT22 no reading for 60s', text)
        if hm and last_valid_ms is not None:
            run.stats['stale after last good reading (s)'].append((t - last_valid_ms) / 1000)
        hm = re.match(r'ERROR DHT22 failed, no reading for 600s', text)
        if hm and last_valid_ms is not None and last_valid_ms >= boot_ms:
            run.stats['failed after last good reading (s)'].append((t - last_valid_ms) / 1000)
        if text.startswith('ERROR DHT22 not readable at startup'):
            run.stats['startup lockout after boot (s)'].append((t - boot_ms) / 1000)
        if text.startswith('ERROR DHT22 failed: heat running'):
            in_fallback = True
        if text.startswith('DHT22 reading again'):
            in_fallback = False
        hm = re.match(r'Wi-Fi reconnect attempt (\d+) of 10', text)
        if hm:
            if wifi_attempt_ms is not None and int(hm.group(1)) > 1:
                gap = (t - wifi_attempt_ms) / 1000
                run.stats['Wi-Fi retry spacing (s)'].append(gap)
                if gap < 120 - 0.01 or gap > 127:
                    run.bad(t, f'Wi-Fi retry {gap:.1f} s after the last')
            wifi_attempt_ms = t
        if text.startswith('restarting: Wi-Fi not reconnected') and (w1 or y1):
            run.bad(t, 'restarting with W1/Y1 closed')

        # match pending relay edges with their reason lines
        hm = re.match(r'(heat|cool) (on|off) \(([^)]*)\)(?: after (\d+)s)?: (.*)$', text)
        if hm:
            kind = hm.group(1) + ' ' + hm.group(2)
            match = next((p for p in pending if p[0] == kind), None)
            if match is None:
                run.bad(t, f'"{text}" logged with no matching relay change')
                continue
            pending.remove(match)
            _, edge_t, duration = match
            rest = hm.group(5)
            if kind == 'heat on':
                if 'fallback' in rest:
                    run.notes['heat starts on the 25 % fallback'] += 1
                else:
                    om = re.match(r'([-\d.]+) F, setpoint (\d+) F', rest)
                    if int(om.group(2)) != setpoint:
                        run.bad(t, f'heat on reports setpoint {om.group(2)}, tracked {setpoint}')
                    if tenths(om.group(1)) > setpoint * 10 - 5:
                        run.bad(t, f'heat started at {om.group(1)} F with setpoint {setpoint}')
                    run.stats['heat start temperature - setpoint (F)'].append(tenths(om.group(1)) / 10 - setpoint)
            elif kind == 'cool on':
                om = re.match(r'([-\d.]+) F, setpoint (\d+) F', rest)
                if tenths(om.group(1)) < setpoint * 10 + 5:
                    run.bad(t, f'cool started at {om.group(1)} F with setpoint {setpoint}')
                run.stats['cool start temperature - setpoint (F)'].append(tenths(om.group(1)) / 10 - setpoint)
            elif kind == 'heat off':
                reason = rest.split(',')[0]
                run.notes['heat stop: ' + reason] += 1
                if reason == 'setpoint reached':
                    temp = tenths(rest.split(', ')[1].split(' ')[0])
                    if temp < setpoint * 10 + 5:
                        run.bad(t, f'heat stopped at {temp / 10} F with setpoint {setpoint}')
                    run.stats['heat stop temperature - setpoint (F)'].append(temp / 10 - setpoint)
                if reason in ('setpoint reached', 'mode changed', 'sensor stale', 'run limit', 'no temperature response') and duration < 180 * SEC - 1:
                    run.bad(t, f'heat ran only {duration / 1000:.0f} s ({reason})')
                if duration > 180 * 60 * SEC + 100:
                    run.bad(t, f'heat ran {duration / 60000:.1f} min in one run')
                if reason == 'fallback off time':
                    run.stats['fallback W1 on time (s)'].append(duration / 1000)
                if reason == 'setpoint reached':
                    run.stats['heat cycle on time (min)'].append(duration / 60000)
            elif kind == 'cool off':
                reason = rest.split(',')[0]
                run.notes['cool stop: ' + reason] += 1
                y1_stop_normal = reason in ('setpoint reached', 'mode changed', 'sensor stale', 'run limit', 'no temperature response')
                if duration > 360 * 60 * SEC + 100:
                    run.bad(t, f'cool ran {duration / 60000:.1f} min in one run')
                if reason == 'setpoint reached':
                    temp = tenths(rest.split(', ')[1].split(' ')[0])
                    if temp > setpoint * 10 - 5:
                        run.bad(t, f'cool stopped at {temp / 10} F with setpoint {setpoint}')
                    run.stats['cool stop temperature - setpoint (F)'].append(temp / 10 - setpoint)
                    run.stats['cool cycle on time (min)'].append(duration / 60000)
                if y1_stop_normal and duration < 300 * SEC - 1:
                    run.bad(t, f'compressor ran only {duration / 1000:.0f} s ({reason})')
                if not y1_stop_normal:
                    run.notes['compressor safety stops (off / failed sensor)'] += 1
    if pending:
        run.bad(0, f'{len(pending)} relay changes never logged by the firmware')
    return run


def summarise(values):
    if not values:
        return ''
    return f'n={len(values)} min={min(values):.2f} median={statistics.median(values):.2f} max={max(values):.2f}'


def main(folder):
    runs = [audit(path) for path in sorted(glob.glob(os.path.join(folder, '*.log'))) if not path.endswith('units.log')]
    total_problems = 0
    merged = defaultdict(list)
    for run in runs:
        total_problems += len(run.problems)
        status = 'OK ' if not run.problems else 'BAD'
        print(f'{status} {run.name:24s} {len(run.problems)} problems')
        for problem in run.problems[:8]:
            print('      ', problem)
        for key, values in run.stats.items():
            merged[key].extend(values)
    print('\nmeasured across all runs:')
    for key in sorted(merged):
        print(f'  {key:42s} {summarise(merged[key])}')
    notes = Counter()
    for run in runs:
        notes.update(run.notes)
    print('\nevents across all runs:')
    for key, count in sorted(notes.items()):
        print(f'  {key:50s} {count}')
    print(f'\n{total_problems} problems in {len(runs)} traces')


folder = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'output')
main(folder)
