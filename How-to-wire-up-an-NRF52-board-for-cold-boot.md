# ColdBoot Functionality

<img width="1304" height="438" alt="ColdBoot circuit schematic" src="https://github.com/user-attachments/assets/72845c85-a650-47be-856f-c60948f76009" />

<img width="303" height="305" alt="FP1 front-panel connector pinout" src="https://github.com/user-attachments/assets/765b12bb-53e0-48b6-81d6-4af9bcdacdd8" />

**Capability:** ColdBoot lets a paired Steam Controller turn the PC on from a fully off state. Short-press the Steam button. The Pro Micro pulses a GPIO pin. The pulse closes the PC's power switch circuit, the same as a press of the case power button. This works because the USB port keeps standby power on the Pro Micro even while the PC is off, so it stays awake and keeps listening for the button press. ColdBoot is off by default in the firmware and needs the extra wiring below to work.

**Level of effort:** Basic soldering skills. You solder one resistor and one transistor, then run two wires to the motherboard's front panel header.

**Bill of materials:**
| Qty | Part |
|---|---|
| 1 | Pro Micro NRF52840 (the same board that runs OpenPuck) |
| 1 | 2N3904 NPN transistor (or equivalent general-purpose NPN) |
| 1 | 1 kΩ resistor |
| ~30 cm | Hookup wire |
| — | Heat-shrink tubing or electrical tape |
| — | Soldering iron and solder |
| 1 (optional) | LED + matching series resistor (e.g. 330 Ω) — diagnostic, lights each time the pulse fires |

**Setup:**
1. Check that pin 29 is correct for your board. Some Pro Micro clones print it as `017` instead of `029`, and the mapping can change with the board core. If you are not sure, flash [`blink_test/blink_test.ino`](./blink_test/blink_test.ino) first. It blinks one pin so you can confirm it by eye, or edit `TEST_PIN` and reflash until you find the right one.
2. Build the circuit shown in the schematic above. Connect Pro Micro pin 29 to one leg of the 1 kΩ resistor.
3. Connect the other leg of the resistor to the base of Q1 (2N3904).
4. Connect the collector of Q1 to the `PWRBTN#` pin on the motherboard's FP1 header, shown in the pinout diagram above.
5. Connect the emitter of Q1 to a `GND` pin on the FP1 header.
6. Wire Q1 in parallel with the case's existing power button. Do not remove the case button.
7. Optional: solder the LED, in series with its resistor, between pin 29 and GND on the Pro Micro. It lights each time the Pro Micro sends the pulse, a quick way to check the trigger without opening the case.
8. Insulate all exposed leads with heat-shrink tubing or tape.
9. Flash firmware built with the ColdBoot feature enabled: `make uf2 EXTRA_FLAGS="-DOPK_PWR_SWITCH=1"`.
10. Plug the Pro Micro into the PC over USB and pair the Steam Controller as usual.
11. Test it: turn the PC fully off, wake the controller then short-press the Steam button. The PC should power on.

> [!WARNING]
> Never connect the Pro Micro's GPIO pin directly to the FP1 header. Always switch it through a transistor, relay, or optocoupler. A direct short can damage the motherboard or the Pro Micro.
>
> Your motherboard's FP1 connector may not match the pinout shown above. Check your motherboard manual first. If you wire this in line with the power button and it does not work, reverse the two pins and try again.

<img width="3024" height="4032" alt="image" src="https://github.com/user-attachments/assets/28be0984-1f17-4c78-a56e-fc294f698c88" />
<img width="3024" height="4032" alt="image" src="https://github.com/user-attachments/assets/1bfe6c3e-c16e-4c26-aaa8-363b7662873f" />
<img width="3024" height="4032" alt="image" src="https://github.com/user-attachments/assets/085c2337-4bdd-4cf4-b58b-6c189a6b0f1d" />
<img width="3024" height="4032" alt="image" src="https://github.com/user-attachments/assets/897e0741-b2a4-4c3c-93a3-ba05dec8c5ac" />