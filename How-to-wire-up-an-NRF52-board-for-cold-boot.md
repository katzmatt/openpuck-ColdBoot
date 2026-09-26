# ColdBoot Functionality

<img alt="ColdBoot circuit schematic" src="https://github.com/user-attachments/assets/72845c85-a650-47be-856f-c60948f76009" />

<img alt="FP1 front-panel connector pinout" src="https://github.com/user-attachments/assets/765b12bb-53e0-48b6-81d6-4af9bcdacdd8" />

**Capability:** ColdBoot lets a paired Steam Controller turn the PC on from a fully off state. Short-press the Steam button. The Pro Micro pulses a GPIO pin. The pulse closes the PC's power switch circuit, the same as a press of the case power button. This works because the USB port keeps standby power on the Pro Micro even while the PC is off, so it stays awake and keeps listening for the button press. ColdBoot is off by default in the firmware and needs the extra wiring below to work.

To know whether the host is actually off (so it never sends a stray power press during normal use), the firmware reads a second GPIO (`PWR_SENSE_PIN`) through a resistor divider off some rail on the motherboard that reads differently when the host is on vs. off — a PWR LED, a fan header, or a standby rail. *(Earlier firmware revisions tried to infer this from whether the host's USB controller had enumerated the Pro Micro, but that turned out to be unreliable across different motherboards/BIOSes, so it was replaced with this direct voltage sense.)*

> [!WARNING]
> **The sense line does not necessarily read "high when on, low when off."** On the reference build, the default `PWR LED+` tap measured **~5V with the host OFF** and **~2.7V with the host ON** — backwards from the naive assumption. With the host off, no current flows through the LED, so its anode floats up to the rail through the motherboard's own current-limiting resistor; once the host is on and the LED is actually lit, that same point sits near the LED's forward-voltage drop instead. Your board may behave differently, in either direction, depending on which point you tap and how that motherboard drives it. **Measure your own sense point in both power states with a multimeter before finalizing the divider or the firmware config below** — see "Measuring your sense point" underneath the table. The schematic above shows the switch-trigger side only; it predates the sense divider described here.

**Level of effort:** Basic soldering skills. You solder two resistors, one transistor, then run four wires (two to the front-panel header, two to whatever rail you're sensing).

**Bill of materials:**
| Qty | Part |
|---|---|
| 1 | Pro Micro NRF52840 (the same board that runs OpenPuck) |
| 1 | 2N3904 NPN transistor (or equivalent general-purpose NPN) |
| 1 | 1 kΩ resistor (transistor base) |
| 2 | Resistors for the sense divider — values depend on which rail you tap, see table below |
| ~30 cm | Hookup wire |
| — | Heat-shrink tubing or electrical tape |
| — | Soldering iron and solder |
| 1 (optional) | LED + matching series resistor (e.g. 330 Ω) — diagnostic, lights each time the pulse fires |
| — | Multimeter — required to measure your sense point before wiring/configuring the divider |

**Host-off sense divider:** pick whichever rail is convenient to tap on your board. The sense pin only ever sees the divided-down voltage, and the nRF52840's ADC reads 0–3.6V by default, so the divider must keep the pin's voltage safely under that even at the rail's worst-case high tolerance.

| Source rail | Typical tap point | R1 (rail → pin) | R2 (pin → GND) | Notes |
|---|---|---|---|---|
| 3.3V | 3.3V standby / any always-hot 3.3V test point | 1 kΩ | 10 kΩ | Lightest divider of the three; 3.3V is already close to the ADC ceiling so don't skip it. A plain rail tap like this is normally NOT inverted (off ≈ 0V, on ≈ 3.3V) but still measure yours |
| 5V (default, tested) | `PWR LED+` on the front-panel header | 10 kΩ | 20 kΩ | What ColdBoot ships wired to by default. **Measured inverted** on the reference board — see warning above and the firmware config section below |
| 12V | Fan header `+12V` (e.g. boards with no PWR LED) | 33 kΩ | 10 kΩ | Extra divider margin since 12V rails run looser tolerance than 3.3V/5V. A fan header is normally a plain rail (off = 0V, on = 12V, not inverted like an LED tap) but confirm on your board — some boards keep fan headers powered or PWM'd even in a low-power state |

If you use a rail not listed here, keep the pin voltage in roughly the 1.5–3.3V range at the rail's nominal voltage (both states) for margin against ADC noise and against the ~3.6V ceiling.

**Measuring your sense point (do this before wiring the divider):**
1. With the PC fully off (unplugged or switched off at the PSU, not just software shutdown, to match the "off" state ColdBoot targets) and a multimeter in DC volts mode, measure your chosen tap point relative to a chassis/motherboard `GND` pin. Record this as `V_off`.
2. Power the PC on normally and measure the same point again. Record this as `V_on`.
3. Confirm `V_off` and `V_on` are far enough apart to reliably tell apart (at least ~1V of separation) — if they're close, pick a different tap point.
4. Note which one is higher — you'll need this for `PWR_SENSE_ACTIVE_HIGH` below.

**Configuring the firmware for your measured values:** `pwr_switch.h` exposes two overridable macros for this — `PWR_SENSE_ACTIVE_HIGH` and `PWR_SENSE_THRESHOLD`. Using your `V_on`/`V_off` from above and the R1/R2 you're using:
1. Compute the divided voltage for each state: `V_pin = V_measured * R2 / (R1 + R2)`.
2. Set `PWR_SENSE_ACTIVE_HIGH` to `1` if `V_pin` at `V_on` is higher than at `V_off`, or `0` if it's lower (inverted, like the reference `PWR LED+` tap).
3. Set `PWR_SENSE_THRESHOLD` to the midpoint of the two, in `analogRead()` counts (default 10-bit, 0–3.6V full-scale): `round((V_pin_on + V_pin_off) / 2 / 3.6 * 1023)`.
4. Override both via `-DPWR_SENSE_ACTIVE_HIGH=<0|1> -DPWR_SENSE_THRESHOLD=<n>` in `EXTRA_FLAGS` (see the build command in step 11 below), or edit the defaults directly in `pwr_switch.h`.

The shipped defaults (`PWR_SENSE_ACTIVE_HIGH=0`, `PWR_SENSE_THRESHOLD=730`) match the reference board's measured `PWR LED+` behavior (5V off / 2.7V on undivided → 3.33V off / 1.80V on at the pin with the 10k/20k divider), and have been confirmed working end-to-end on that board: a short press correctly fires the pulse only while the host is genuinely off. Still don't assume they're correct for your board without measuring your own — the whole point of the warning above is that this varies by motherboard.

**Setup:**
1. Check that pin 29 is correct for your board. Some Pro Micro clones print it as `017` instead of `029`, and the mapping can change with the board core. If you are not sure, flash a minimal blink sketch first (an `#include <Arduino.h>` sketch that just toggles the pin in `loop()`), so you can confirm the pin by eye before wiring anything up. The same trick works for finding the sense pin (Arduino pin `20`, silkscreened `029` aka `A6`).
2. Build the switch-trigger circuit shown in the schematic above. Connect Pro Micro pin 29 to one leg of the 1 kΩ resistor.
3. Connect the other leg of the resistor to the base of Q1 (2N3904).
4. Connect the collector of Q1 to the `PWRBTN#` pin on the motherboard's FP1 header, shown in the pinout diagram above.
5. Connect the emitter of Q1 to a `GND` pin on the FP1 header.
6. Wire Q1 in parallel with the case's existing power button. Do not remove the case button.
7. Measure your chosen sense tap point per "Measuring your sense point" above, in both power states, BEFORE wiring the divider.
8. Build the sense divider from the table above for your chosen rail. Connect R1 between the rail and the sense pin (Arduino pin `20`, silkscreened `029`), and R2 between that same pin and `GND`.
9. Optional: solder the LED, in series with its resistor, between pin 29 and GND on the Pro Micro. It lights each time the Pro Micro sends the pulse, a quick way to check the trigger without opening the case.
10. Insulate all exposed leads with heat-shrink tubing or tape.
11. Flash firmware built with the ColdBoot feature enabled, including your `PWR_SENSE_ACTIVE_HIGH`/`PWR_SENSE_THRESHOLD` from "Configuring the firmware" above if they differ from the shipped defaults: `make uf2 EXTRA_FLAGS="-DOPK_PWR_SWITCH=1 -DPWR_SENSE_ACTIVE_HIGH=0 -DPWR_SENSE_THRESHOLD=730"` (adjust the last two values, or drop them entirely to keep the reference-board defaults).
12. Plug the Pro Micro into the PC over USB and pair the Steam Controller as usual.
13. Test it: turn the PC fully off, wake the controller then short-press the Steam button. The PC should power on.

> [!WARNING]
> Never connect the Pro Micro's GPIO pin directly to the FP1 header. Always switch it through a transistor, relay, or optocoupler. A direct short can damage the motherboard or the Pro Micro.
>
> Never connect a sensed rail directly to the sense pin without the divider — even 5V and 3.3V standby rails can spike, and 12V will damage the pin outright. Double-check R1/R2 are wired correctly (rail → R1 → pin → R2 → GND) before powering anything on.
>
> Your motherboard's FP1 connector may not match the pinout shown above. Check your motherboard manual first. If you wire this in line with the power button and it does not work, reverse the two pins and try again.

<img alt="image" src="https://github.com/user-attachments/assets/28be0984-1f17-4c78-a56e-fc294f698c88" />
<img alt="image" src="https://github.com/user-attachments/assets/1bfe6c3e-c16e-4c26-aaa8-363b7662873f" />
<img alt="image" src="https://github.com/user-attachments/assets/085c2337-4bdd-4cf4-b58b-6c189a6b0f1d" />
<img alt="image" src="https://github.com/user-attachments/assets/897e0741-b2a4-4c3c-93a3-ba05dec8c5ac" />
