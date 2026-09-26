// pwr_switch.h -- GPIO trigger emulating a momentary press of the HOST motherboard's power switch.
//
// Optional feature (build with -DOPK_PWR_SWITCH=1, see config.h): fires ONE momentary pulse on
// PWR_SWITCH_PIN when the paired Steam Controller's STEAM button gets a short press (down+up
// within 1s -- the same gesture rf_link.cpp already recognizes for USB remote wakeup) WHILE the
// host is OFF: PWR_SENSE_PIN reads on the "off" side of PWR_SENSE_THRESHOLD (which side that is
// depends on PWR_SENSE_ACTIVE_HIGH -- see below, this is NOT always "reads low"), sustained for
// HOST_OFF_DEBOUNCE_MS so a brief sag/blip can't false-fire. Assumes the puck's own supply stays up
// while the host is off (so this firmware keeps running and listening) and the controller is
// already bonded -- there is no pairing UI here.
//
// Hardware note: PWR_SWITCH_PIN must drive an ISOLATING stage (relay / optocoupler / transistor)
// wired across the motherboard's front-panel power-switch header -- never short the header
// directly to the nRF's GPIO. This module only knows how to drive one pin; the isolation is on you.
#pragma once

#ifndef PWR_SWITCH_PIN
// Arduino pin number for physical P0.17 ("017" on this SuperMini clone's silkscreen) under the
// Adafruit Feather core this builds against (variants/feather_nrf52840_express/variant.cpp:
// g_ADigitalPinMap[29] == 17) -- confirmed on real hardware with a standalone blink sketch. The
// silkscreen number is NOT the Arduino pin number; don't assume identity on a different board.
#define PWR_SWITCH_PIN 29
#endif
#ifndef PWR_SWITCH_ACTIVE
#define PWR_SWITCH_ACTIVE \
	HIGH // level that closes the switch; set LOW if your driver is active-low
#endif

#ifndef PWR_SENSE_PIN
// Arduino pin number for physical P0.29 ("029" on this SuperMini clone's silkscreen), aka A6 on
// the Adafruit Feather core this builds against (variants/feather_nrf52840_express/variant.cpp:
// g_ADigitalPinMap[20] == 29). Fed by an external resistor divider off whatever rail you're
// sensing -- see README's ColdBoot section for divider values per rail. Don't assume this Arduino
// pin number identifies the same physical pin on a different board core.
#define PWR_SENSE_PIN A6
#endif

// ---- Adapting PWR_SENSE_ACTIVE_HIGH / PWR_SENSE_THRESHOLD to YOUR motherboard ----
//
// DO NOT trust the defaults below without measuring your own board first. Front-panel sense
// points do not all behave the same way, and getting this backwards means the feature fires
// while the host is ON and does nothing while it's OFF -- exactly the failure mode this section
// exists to prevent (found the hard way: PWR LED+ on the reference board reads ~5V with the host
// OFF and ~2.7V with it ON -- the OPPOSITE of the naive "LED lit == powered rail present"
// assumption, because with the host off no current flows through the LED so its anode floats up
// to the rail through the motherboard's own current-limiting resistor; with the host on and the
// LED actually lit, that same point sits near the LED's forward-voltage drop instead).
//
// 1. With a multimeter (or the CDC console / a throwaway analogRead+Serial.println sketch),
//    measure the UNDIVIDED voltage at your chosen sense tap in BOTH states: host fully OFF, and
//    host ON. Do this before you finalize the divider or wiring -- the two states must actually
//    differ enough to tell apart, and you need to know which one reads higher.
//      - A rail that's simply present/absent (a 3.3V/5V standby test point, most 12V fan headers)
//        behaves the "expected" way: ~0V when off, full rail when on.
//      - An LED tap (PWR LED+, HDD LED+, etc.) can easily be inverted, as above -- MEASURE, don't
//        assume, even if your board also uses a PWR LED like the reference one.
// 2. Build the divider for your tap per the README table (or scale your own: pick R2, then
//    R1 = R2 * (V_rail / V_target - 1) for whatever V_target <= ~3.0V you want at the pin).
//    Compute the DIVIDED voltage for both states: V_pin = V_measured * R2/(R1+R2).
// 3. Set PWR_SENSE_ACTIVE_HIGH: 1 if the divided ON voltage is HIGHER than the divided OFF
//    voltage, 0 if it's LOWER (inverted, like the reference PWR LED+ tap below).
// 4. Set PWR_SENSE_THRESHOLD to the midpoint of the two divided voltages, in analogRead() counts
//    (default 10-bit, 0..3.6V full-scale): round((V_pin_on + V_pin_off) / 2 / 3.6 * 1023).
//
// Defaults below are the MEASURED values for the reference build's PWR LED+ tap (5V undivided
// off / 2.7V undivided on, 10k/20k divider -> 3.33V off / 1.80V on at the pin) -- verify your own
// board reproduces something close to this before relying on these numbers unchanged.
#ifndef PWR_SENSE_ACTIVE_HIGH
// 0: host-ON reads LOW (reference PWR LED+ tap); 1: host-ON reads HIGH
#define PWR_SENSE_ACTIVE_HIGH 0
#endif
#ifndef PWR_SENSE_THRESHOLD
// Midpoint between the reference board's measured on (~1.80V / ~511 counts) and off (~3.33V /
// ~946 counts) divided voltages: (511+946)/2 ~= 730.
#define PWR_SENSE_THRESHOLD 730
#endif

// call once from setup(): pins to their idle state (switch released, sense as input)
void pwrSwitchInit();
void pwrSwitchTask(); // call every loop(): edge-detect the Steam short press, gate, pulse, release
