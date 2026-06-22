# OpenPuck RF Sniffer

A **passive** 2.4 GHz sniffer for the link between a real Valve SC2 puck and its controller. It never
transmits, so it cannot disturb the pairing — it only listens, and streams every frame to a browser app in
real time with **Start Cap / End Cap**.

Use it to capture the actual puck↔controller traffic for any function (shutdown, LED, battery, …), isolate
the relevant frames, and export them.

## What you need
- A **second** nRF52840 board (e.g. the same Pro Micro / Feather you flash OpenPuck onto) — *not* the puck.
- The real Valve puck + controller, paired and working.
- Chrome or Edge (WebUSB).

## 1. Flash the sniffer firmware
```
arduino-cli compile -b adafruit:nrf52:feather52840 \
  --build-property "build.extra_flags=-DNRF52840_XXAA {build.flags.usb}" \
  --upload -p <PORT> puck_sniffer
```
(Same manual-DFU/double-tap-reset dance as OpenPuck if the port won't auto-reset.) It enumerates as
`28DE:534E "OpenPuck Sniffer"`.

## 2. Open the app
WebUSB needs a secure context, so **not** a `file://` path. Either:
- **Hosted:** https://safijari.github.io/OpenPuck/sniffer.html (once `docs/` is pushed to gh-pages), or
- **Local:** `cd docs && python3 -m http.server` → open http://localhost:8000/sniffer.html

Click **Connect sniffer**, pick *OpenPuck Sniffer*.

## 3. Capture — the exact sequence

The link is Nordic **Gazell**. A controller's **bonded on-air address is fixed at pairing and REUSED on every
reconnect** (reconnect is a silent ESB resume — no fresh address, no guaranteed ch2 re-beacon), and the session
hops a small **stable** channel map. So the sniffer **learns each bond once** — it persists the session
(base/prefix/channel) to flash keyed by the bond's **ibex_uuid** and remembers which channels carry its replies —
and from then on **auto-camps** straight onto it, catching the next clean reconnect with **no ch2 catch and no
manual pinning**.

### Setup (once)
1. **Unplug the copycat** (the OpenPuck dev board) if it's plugged in — it beacons its own `E1`s and pollutes
   ch2. Leave only: the **real Valve puck**, the **controller**, and the **sniffer board**.
2. With `pairtui` (see `../pairtui/`), note the connected slot's **ibex_uuid** (e.g. `EF7171B4`).

### First run — teach it the bond (one time per controller)
3. Flash the sniffer, open the app (`docs/sniffer.html` over localhost/https — not `file://`), **Connect** →
   *OpenPuck Sniffer*. Confirm **rx** is climbing.
4. In **lock ibex**, type the uuid (`EF7171B4`), click **Lock**.
   - If **bond** shows `… camped`, it already knows this controller → skip to step 6.
   - Otherwise status is **ACQUIRE** (waiting on ch2 for that slot's `E1`).
5. Click **● Start Cap**, then **power-cycle the controller** (hold power until off, ~2 s, back on). When its `E1`
   is caught the sniffer locks the session, **persists the bond**, and the **bond** stat shows `1 learned · camped`.

### Every run after — just capture the reconnect
6. With the bond known, the link goes **CAPTURE (auto-camped on learned bond)** on its own (on plug-in if it's the
   only bond, or the moment you **Lock** its uuid). **Power-cycle the controller** and its reconnect is captured
   from the first packets — `C→P` climbs with no further steps.
7. Wiggle sticks / trackpad to keep traffic flowing; do the thing in Steam (change brightness / LED, or turn off).
8. **■ End Cap → Download.**

> **Forget bonds** clears the learned store (relearn from the next `E1`). Use it if a controller was re-paired
> (its bonded address changed). The learned channel set self-corrects as new reply channels are observed.

Direction: `P→C` (opcode `0xE_`, puck→controller — LED/shutoff/brightness commands ride here) vs `C→P` (`0xF_`,
controller→puck — input + battery/telemetry).

### Filters (find the needle in the input spam)
The table has a filter bar:
- **hide input/polls** — drops the 99% routine traffic (the 49-byte `0xF1` input replies and the bare `0xE3`
  polls), leaving only commands, feature responses, beacons, and anything unusual.
- **dir** — `P→C` / `C→P` / all.
- **op** — exact opcode, e.g. `f1`, `e3`.
- **len** — `≠ 49 (non-input)`, `> 49`, `> 1`. **`≠ 49` is the key for finding feature responses / battery.**
- **hex** — substring match on the payload (e.g. `9f`, `2d`).

Filters re-render the visible table from a buffer, so you can change them after the fact.

### Capturing battery
Battery is **not** in the input stream — it's `ID_GET_BATTERY_DATA`, a feature GET Steam issues **on demand**
(when it shows the battery), answered by the controller in a **longer `C→P` frame (len > 49)** carrying a
`type-04` TLV. To catch it: lock + Start Cap as above, set **hide input/polls** + dir **C→P** (or len **≠ 49**),
then **open the controller's page in Steam** (Settings → Controller / the battery indicator) so Steam polls it.
Grab the non-49 `C→P` frame that appears.

### If `C→P` won't climb after the power-cycle
- **Survey ch2** and look at the `op e1` rows during the reconnect: one payload should contain your uuid in
  little-endian (`EF7171B4` → `… b4 71 71 ef …`). If you only ever see a *different* uuid, the copycat or another
  bonded slot is the only thing reconnecting — make sure the real controller is re-pairing to the **real** puck.
- Re-check you did **Lock → Start Cap → power-cycle** in that order.

> Bond info (which controller/slot, its serial + uuids) is read over USB, not the air — see `../pairtui/`. Useful
> for the uuid lock and labelling a capture; it does **not** give the on-air address (random per session).

## How it works
After RX the radio buffer is `[LENGTH][S1][payload…]`; `payload[0]` is the opcode. The puck advertises its
per-session base/prefix/channel inside the `E1` host frame on the shared `ibex`/ch2 rendezvous, so the sniffer
reads those, retunes to the session, and receives both directions (same ESB base — and it now listens on **all
8 ESB pipes** of that base, so a controller reply on a different prefix is still caught; the **pipe** column /
status `last pipe` shows which `RXMATCH` each frame hit). A QoS channel-hop is followed via the `E1` keepalive.
When the session goes silent it **sweeps clean candidate channels keeping the learned address** (rather than
abandoning to ch2) and only falls back to a full discovery scan after two dry sweeps. Radio parameters are
OpenPuck's CRC-validated config (`radio.cpp`).

### If you only see `E2`/`E1` on ch2 and never the controller's `0xF_` replies
That means it never camps on the session. Read a real `E1` row in the table (a `P→C`, op `e1` frame): its
payload bytes are `e1 [proteus:4] [ibex:4] [CH] 00 00 00 [BASE:4] [PFX]` — i.e. **byte 9 = session channel,
bytes 13–16 = session base, byte 17 = prefix** (the "E1 advertises" stat shows the sniffer's own read of these).
Cross-check that against where traffic actually is, then type the six values into **pin session**
(`b0 b1 b2 b3 pfx ch`, address bytes hex) and click **Pin** to force the sniffer onto that exact session — this
bypasses auto-acquire entirely, which is the reliable path if the real puck's `E1` offsets differ from ours.

## Stream protocol (WebUSB bulk)
- packet: `C0 DE [N] [t_us:4 LE] [ch] [flags] [rssi] [match] [N raw bytes]`  (flags bit0 = CRC ok; match = RXMATCH pipe; raw = `[LEN][S1][payload]`)
- status: `C1 DE [state] [curCh] [base:4] [prefix] [cap] [hb:2] [advCh] [advBase:4] [advPfx] [lastMatch] [drops:2 LE] [bond]`  (22 B; adv* = session parsed from the last `E1`; `drops` = frames lost to a full on-device ring, `0` = lossless capture; `bond` bit7 = camped on a learned bond, bits4-6 = #bonds stored, bits0-3 = #channels learned for the camped bond)
- commands (bulk OUT): `01` start · `02` stop · `03` re-acquire (un-camp) · `04 <ch>` pin channel · `05 <b0 b1 b2 b3 pfx ch>` pin full session · `06` survey · `07 <u0 u1 u2 u3>` lock ibex_uuid (auto-camps if the bond is already learned) · `08` forget all learned bonds

Learned bonds persist in flash (`/sniffbonds.bin`, keyed by ibex_uuid: session base/prefix/channel + observed reply channels), so after one successful learn the sniffer auto-camps on the controller's stable bonded address and catches its reconnects with no manual steps.
