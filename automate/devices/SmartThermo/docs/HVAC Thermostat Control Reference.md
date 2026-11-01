# HVAC Thermostat Control Reference

Oct 22, 2026 · @Ben

## System setup

The system is a single-stage gas furnace with single-stage central AC, run by one 24 VAC transformer. A Braeburn 2020NC thermostat controls it, wired with five conductors including a C wire. Every function is switched by connecting **R** (24 VAC hot) to one or more output terminals.

### Thermostat settings (Braeburn 2020NC, board 2020NC-150-03)

| Setting | Position | Meaning |
| --- | --- | --- |
| CONV / HP | CONV | Conventional furnace plus AC, not a heat pump |
| HG / HE | HG | Gas heat: the furnace runs its own blower on a heat call, so only W1 is energized |
| F / C | F | Display in °F |
| LS / OP jumper | OP | Normal operation; any heat-max or cool-min setpoint limits are locked in. LS opens limit setting |
| Power | 24 VAC on C, plus 2 × AA batteries | Batteries keep the clock and settings through a power loss |

### Wall terminals

| Terminal | Wire colour | Type | Function |
| --- | --- | --- | --- |
| Rh | Short blue (likely the Rc–Rh jumper) | 24 VAC hot | Heating hot; tied to Rc on a single-transformer system |
| Rc | Red | 24 VAC hot | Transformer hot; feeds every output |
| Y1 | Yellow | Output | Outdoor compressor contactor (cooling) |
| G | Green | Output | Indoor blower relay |
| W1 | White | Output | Furnace heat call (gas valve sequence) |
| C | Blue | 24 VAC common | Transformer return; powers the thermostat or any controller |
| O | Not connected | Output | Heat pump reversing valve; not used on this system |
| B | Not connected | Output | Heat pump reversing valve; not used on this system |

The thermostat board has a W2 pin, but this wall plate has no W2 terminal, so a second heat stage is not available.

## Control functions

Each action closes a contact from **R** to the terminals marked **CLOSED**; everything else stays **open**. C is never switched: it only returns current from the relay coils.

| Action | R → W1 (heat) | R → Y1 (compressor) | R → G (blower) | What the equipment does |
| --- | --- | --- | --- | --- |
| Off / idle | open | open | open | Nothing runs |
| Fan only (Fan ON) | open | open | **CLOSED** | Indoor blower runs; no heating or cooling |
| Cool | open | **CLOSED** | **CLOSED** | Outdoor compressor and condenser fan start; indoor blower moves air over the cold coil |
| Heat (Fan AUTO) | **CLOSED** | open | open | Furnace runs its ignition sequence, then starts the blower itself after its warm-up delay (about 30–90 s) and keeps it running for its post-purge after W1 opens |
| Heat with Fan ON | **CLOSED** | open | **CLOSED** | Blower runs continuously; furnace heats as normal. Allowed, but the blower moves cool air until the heat exchanger warms |
| End of a cool cycle | open | open | open (or held 30–90 s) | Compressor stops; an optional blower run-on pulls the remaining cooling off the coil |

Short form:

- **Fan:** R to G.
- **Cool:** R to Y1 and R to G, together.
- **Heat:** R to W1 only.
- **Off:** nothing connected.

Rc and Rh are both R. With the jumper in place, either one can feed every output.

## Rules: what never to do

The outputs must only ever be connected to R through contacts, and the cooling and heating outputs are mutually exclusive.

| Never | Why | Result if broken |
| --- | --- | --- |
| Connect R to C | Direct short across the 24 VAC transformer | Blows the 3–5 A fuse on the furnace control board; can burn the transformer |
| Connect C to any output (W1, Y1, G) | Puts the relay coil across nothing, or shorts R to C once that output is closed | Same as above when the output is energized |
| Close W1 and Y1 at the same time | Furnace heats the air while the AC cools it | Wasted energy; high head pressure and possible damage to the AC coil sitting above the hot furnace |
| Close Y1 without G | Compressor runs with no airflow across the indoor coil | Coil freezes into a block of ice; liquid refrigerant can return to the compressor |
| Restart Y1 before the compressor's off time is up | Compressor tries to start against high pressure | Locked-rotor current, tripped breaker, shortened compressor life (see Lockout and dead time) |
| Feed any voltage into an output from a controller's own supply | Outputs expect 24 VAC from R only | Damages the furnace board or the controller |
| Use solid-state switching that leaks current | Many triac or SSR outputs leak a few mA when off | Relays chatter or hold in; the furnace can see a false heat call |
| Rewire with the furnace powered | A stray strand can bridge R and C | Blown fuse |
| Use O or B | They are heat-pump reversing-valve outputs | Nothing on this system is connected to them |

Turn off the furnace's power switch (or its breaker) before touching any thermostat wire.

## Lockout and dead time

The compressor must stay off for at least 5 minutes after every stop, including after a power loss. Any controller replacing the thermostat has to enforce every row marked **Controller** below. The Braeburn does the compressor delay itself; the furnace's own board handles the rest.

| Condition | Value | Enforced by | Rule |
| --- | --- | --- | --- |
| Compressor minimum off time | 5 min | Braeburn (built in) · Controller | After Y1 opens, Y1 may not close again for 5 min |
| Compressor delay after power returns | 5 min | Braeburn · Controller | Treat any power-up as if the compressor just stopped |
| Compressor minimum on time | 3–5 min (recommended) | Controller | Once Y1 closes, keep it closed at least this long unless switching to Off for safety |
| Cooling cycle limit | 3–4 starts per hour (recommended) | Controller | Widen the temperature differential if it short-cycles |
| Heat ↔ cool changeover dead band | 5 min with W1 and Y1 both open (recommended) | Controller | Never go straight from heat to cool or back |
| Furnace minimum on time | 3 min (recommended) | Controller | Short burns leave the heat exchanger wet with condensation |
| Heating cycle limit | 5–6 starts per hour (recommended) | Controller |  |
| Blower start delay on heat | about 30–90 s | Furnace board | Normal: do not close G to “help” it on HG |
| Blower run-on after heat | about 60–180 s | Furnace board | Normal: the blower keeps running after W1 opens |
| Blower run-on after cool | 0–90 s (optional) | Controller | Hold G after Y1 opens to use the cold left in the coil |
| Temperature differential (swing) | 0.5 °F default; 1.0 or 2.0 °F options | Braeburn user menu (hold RETURN about 3 s) | Larger values mean fewer, longer cycles |
| Setpoint limits | Heat max / cool min, if set | LS / OP jumper (on OP = locked) | Move to LS to change, then back to OP |

**Furnace lockouts** come from the furnace control board, not the thermostat. Typical boards:

- **Ignition failure:** after about 3 failed tries the furnace locks out, usually for 1 hour. Turning the furnace power off for 30 s resets it.
- **High-limit trip:** the burner shuts off and the blower keeps running until the heat exchanger cools. This is usually caused by a dirty filter or closed vents.
- **Pressure switch or rollout trip:** the burner stops and the board flashes a code on its LED. The code chart is on the furnace door.

Exact timings depend on the furnace model, which is not recorded here.

## Verification and controller notes

With the furnace powered, R to C should read 24–28 VAC, and each output should read about 24 VAC to C only while it is being called.

| Test (meter on AC volts) | Idle | Called |
| --- | --- | --- |
| R to C | 24–28 V | 24–28 V |
| W1 to C | about 0 V | 24–28 V during a heat call |
| Y1 to C | about 0 V | 24–28 V during a cool call |
| G to C | about 0 V | 24–28 V during a cool or fan call |

Open question: confirm that the short blue wire on Rh is the Rc–Rh jumper and not a second conductor from the wall.

A replacement controller (for example a Pico W like the lamp controllers) needs:

- Three relays with dry contacts rated at least 1 A at 24 VAC, one each for W1, Y1 and G, all fed from R.
- An AC-input buck converter from R–C to 5 V for the board. The transformer is typically 40 VA, which leaves room for a small controller.
- An interlock in firmware: W1 and Y1 never closed together, Y1 always with G, and every timer in Lockout and dead time.
- A safe default of all relays open on boot or crash. Loss of Wi-Fi changes nothing: the controller runs the thermostat by itself, so opening the relays would only stop the heat or AC whenever the access point restarts.

## Sources

- [Braeburn 2020NC user guide (thermostat.guide)](https://thermostat.guide/braeburn/braeburn-2020nc-programmable-thermostats-user-guide/): switch settings, terminals, wiring, user menu, power
- [Braeburn patent: tamper-resistant thermostat with hidden limit adjustment](https://www.freepatentsonline.com/y2017/0364104.html): the operation / limit-setting jumper (LS / OP meaning is inferred from it)
- [Braeburn 2020NC product sheet](https://xrefs1.plumbersstock.com/product/8/5/856970_3xHm45.pdf): adjustable temperature limits, 45–90 °F range

Recommended timings marked “recommended” are common HVAC practice, not values from Braeburn.
