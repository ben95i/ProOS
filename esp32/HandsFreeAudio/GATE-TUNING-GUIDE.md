# Mic Ducking — In-Car Setup Guide

Honda_HR-V hands-free unit. Tuning the voice-switched gate in `main.c`.

---

## What you're tuning, in one paragraph

The gate is not an echo canceller. It doesn't model the acoustic path or subtract
anything. Once per 7.5 ms frame it asks two questions — *is the far end talking?* and
*is the near end talking?* — and if the answer is yes/no it pulls the uplink down by
32 dB so the echo never leaves the box. Everything below is about teaching it to
answer that second question correctly in **your** car.

The whole job is three numbers. The firmware measures two of them for you.

---

## Before you start

Confirm these are set in `main.c`, then flash:

```c
#define AEC_GATE_ENABLED      1
#define AEC_GATE_LOG_ENABLED  1
#define AEC_CAL_ENABLED       1
```

Have a serial monitor on the unit (`idf.py monitor`) and a friend who will talk to
you continuously for a few minutes. A podcast played into the far end works too and
is easier to arrange — you need *sustained speech*, not one-word answers.

Do the whole procedure **in the car, engine running, doors closed, at your normal
head unit volume**. Coupling changes with volume, seat position and where the mic
ended up. A bench measurement tells you nothing useful.

---

## The two log lines

Every second:

```
GATE far=4103 mic=8741 echoRef=8187 gain=-32.0dB duck=87%
```

| Field | Meaning |
|---|---|
| `far` | The far end's voice level as sent to the DAC |
| `mic` | Level at the mic, after HPF/LPF and `MICROPHONE_GAIN` — **before** the gate |
| `echoRef` | The threshold `mic` must beat for the gate to open |
| `gain` | What the uplink is scaled by right now |
| `duck` | Share of the last second spent ducked |

Every five seconds, the calibration assistant:

```
GATE CAL  coupling +4 dB | headroom +6.0 dB | leak 0% | *** IN RANGE *** (headroom band 3 to 12 dB, leak under 15%)
GATE CAL  far idle 41 vs threshold 300 (7.3x) OK | duck 91% of far-active
```

`coupling` is your measured acoustic coupling. `headroom` is how far the gate's
threshold sits above it. `leak` is the fraction of far-end frames where the echo
alone was loud enough to prise the gate open — that's the number that actually
matters, and it's measured, not modelled.

**`*** IN RANGE ***` is the target.** When you see it on every window for a
minute straight, you're done.

---

## Step 1 — Far-end activity threshold

**Goal:** `AEC_FAR_ACTIVE_LEVEL` sits safely above the idle line noise, so the gate
can't latch shut and mute you for the whole call.

Place a call and have both ends stay silent for ten seconds. Read the second CAL
line:

```
GATE CAL  far idle 41 vs threshold 300 (7.3x) OK
```

- **`OK`** (ratio ≥ 3×) — move on.
- **`THRESHOLD TOO LOW`** — the line tells you what to raise
  `AEC_FAR_ACTIVE_LEVEL` to. Apply it and reflash.

If the ratio is enormous (say 100×) your line is very quiet and you could *lower*
`AEC_FAR_ACTIVE_LEVEL` to catch quiet talkers, but this is rarely worth doing.
Being muted is a much worse failure than under-suppressing a whisper.

---

## Step 2 — Coupling (the one that matters)

**Goal:** `AEC_ECHO_COUPLING_DB` matches your car.

Have the far end talk continuously for 30 seconds. You can talk too — the
measurement is a median, so your voice doesn't poison it, though staying quiet
converges faster.

Read the first CAL line. If it says `ECHO LEAKING` or `OVER-DUCKING`, it also tells
you the value to use:

```
GATE CAL  coupling +4 dB | headroom +2.0 dB | leak 20% | ECHO LEAKING -> set AEC_ECHO_COUPLING_DB to +4
```

Set it, reflash, repeat. **One iteration is normally enough.**

Take two or three readings before you act on one. The measurement has a few dB of
run-to-run spread, which is exactly why the healthy band is 9 dB wide rather than a
single number.

If you'd rather do it by hand: during far-end-only speech, `20·log10(mic/far)` from
the once-per-second line is your coupling. With `AEC_NEAREND_MARGIN` at its default
of 6.0, you type that number straight into `AEC_ECHO_COUPLING_DB`.

---

## Step 3 — Near-end margin (duplex feel)

**Goal:** you can interrupt the far end without them hearing themselves.

This is the only step with no right answer — it's a preference, and it's a genuine
trade. Steps 1 and 2 get the gate working; this one decides how it *feels*.

Both of you talk at once, for a while.

| Symptom | Change |
|---|---|
| They still hear echo when you both talk | Raise `AEC_NEAREND_MARGIN` (try 9.0) |
| Your voice gets chopped when you talk over them | Lower it (try 4.0) |
| First word of your reply is clipped | Lower `AEC_DUCK_RECOVERY_MS` (try 80) |
| Echo burst at the end of their sentences | Raise `AEC_FAR_RELEASE_MS` (try 250) |

Change one thing at a time and re-run Step 2 afterwards, since the margin feeds
into the headroom the assistant reports.

---

## Step 4 — Confirm and lock in

A five-minute call at highway speed with normal back-and-forth. You want:

- `*** IN RANGE ***` on every CAL window
- `leak` consistently under 15%
- `duck` between roughly 70% and 95% of far-active frames
- No complaint about echo from the far end
- Your own voice not obviously chopped

Then turn the logging off for daily use:

```c
#define AEC_GATE_LOG_ENABLED  0
#define AEC_CAL_ENABLED       0
```

Leave `AEC_GATE_ENABLED` at 1. Write your final three numbers down somewhere — if
you ever move the mic or change the head unit, Step 2 has to be redone.

---

## Troubleshooting

**Far end still hears echo.**
Check `leak` first. High leak means Step 2 isn't done — the threshold is under your
echo. If leak is near zero and they *still* hear echo, the residue is getting
through during double-talk: raise `AEC_NEAREND_MARGIN`, or deepen
`AEC_DUCK_DEPTH_DB` to −40.

**Far end can't hear you at all.**
Check `duck` on the once-per-second line while you're the only one talking. If it's
high, the gate is stuck shut — almost always `AEC_FAR_ACTIVE_LEVEL` latching onto
line noise (Step 1), or `AEC_ECHO_COUPLING_DB` set far too high.

**Your voice cuts in and out mid-sentence.**
`AEC_ECHO_COUPLING_DB` is too high, so your own voice can't clear the threshold.
Re-run Step 2 — the assistant will report `OVER-DUCKING` and give you the number.

**`not enough steady far-end speech to measure`.**
The assistant only counts frames where the far end is mid-utterance and both
envelopes are on a plateau — deliberately, since the edges of speech give garbage
readings. Keep the call going with continuous speech. Short answers won't do it.

**Clicking or popping on the far end.**
Shouldn't happen — the gain is interpolated per sample across the frame. If it does,
lengthen `AEC_DUCK_ONSET_MS`.

**Everything reads sensibly but echo persists at high volume only.**
You're driving the amp into distortion, and a gate can't suppress what it doesn't
predict. Use `AEC_SPEAKER_ATTENUATION_DB` (try −6.0) and **re-run Step 2**, since
that changes your coupling.

---

## What this won't fix

The gate is half-duplex by design. While the far end is talking you are attenuated
by 32 dB, so you cannot cut in mid-sentence the way you can on a phone handset. That
is the deal you accepted in exchange for ~30 lines of code and no risk of an
adaptive filter diverging.

If the half-duplex feel turns out to be the thing that bothers you rather than the
echo, that's the point at which a linear AEC with a residual suppressor becomes
worth the effort — and the delay-alignment groundwork (short TX DMA queue, bounded
uplink backlog) is already in place for it.

---

## Quick reference

| Constant | Default | Set by |
|---|---|---|
| `AEC_FAR_ACTIVE_LEVEL` | `300.0f` | Step 1 — measured |
| `AEC_ECHO_COUPLING_DB` | `0.0f` | Step 2 — measured |
| `AEC_NEAREND_MARGIN` | `6.0f` | Step 3 — preference |
| `AEC_DUCK_DEPTH_DB` | `-32.0f` | Rarely changed |
| `AEC_FAR_RELEASE_MS` | `180.0f` | Echo at end of their sentences |
| `AEC_DUCK_RECOVERY_MS` | `120.0f` | Clipping of your first word |
| `AEC_SPEAKER_ATTENUATION_DB` | `0.0f` | Only if the amp is distorting |
