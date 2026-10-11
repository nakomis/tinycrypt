# tinycrypt key board: design (CRYPT-16)

A YubiKey-style USB dongle with a captive cable, assembled entirely by JLCPCB. It replaces the dev
board and the ATECC608 breakout. Martin designs and prints the enclosure.

## Blocks

| Block | Part | LCSC | Type | Notes |
|---|---|---|---|---|
| MCU + radio | ESP32-S3-MINI-1-N8 | C2913206 | Ext | 15.4 × 20.5 mm; PCB antenna, crystal and 8 MB flash inside; pre-certified |
| Secure element | ATECC608B-SSHDA-T | C1518769 | Ext | SOIC-8, I2C, unprovisioned (we write and lock our own config) |
| Cable socket | JST SM05B-SRSS-G-TB | C265419 | Ext | SH 1.0 mm, 5-pin, right angle, gold. VBUS, D−, D+, CC, GND |
| USB ESD | USBLC6-2SC6 | C7519 | Ext | SOT-23-6, on D+/D− and VBUS |
| 3.3 V regulator | AP2112K-3.3TRG1 | C51118 | Ext | SOT-23-5, 600 mA, 250 mV dropout; Wi-Fi peaks are ~350 mA |
| Button | TS-1187A-B-A-B | C318884 | Basic | 5.1 mm SMD tact switch on GPIO0: presence + download strap |
| Status LED | WS2812B-2020-V6 | C52917434 | Ext | 2 × 2 mm addressable RGB, 3.3–5.3 V, so it runs on 3V3 with 3V3 data |
| CC sink | 5.1 kΩ 0402 | C25905 | Basic | Rd on CC: tells the host "5 V device", no PD chip |
| Pull-ups | 10 kΩ 0402 | C25744 | Basic | EN, GPIO0 |
| I2C pull-ups | 4.7 kΩ 0402 | C25900 | Basic | SDA, SCL |
| LED series resistor | 330 Ω 0402 | C25104 | Basic | Damps the GPIO17 → DIN line |
| Decoupling | 100 nF 0402 / 1 µF 0402 / 10 µF 0603 | C1525 / C52923 / C19702 | Basic | 10 µF on VBUS in, 3V3 out and the module; 1 µF for EN |

Prices at quantity 1, in USD: module $4.68, ATECC $1.00, socket $0.55, LDO $0.16, ESD $0.16, LED $0.09;
the rest are pennies. Extended parts add JLCPCB's per-part loading fee, which is accepted (none of them
has a Basic equivalent).

## Circuit

- **USB in (J1, JST-SH):** J1 is 1 VBUS, 2 D−, 3 D+, 4 CC, 5 GND (printed on the silkscreen).
  VBUS → U3 (USBLC6) VBUS pin and → 10 µF (C1) → LDO input. D+/D− through U3 to the
  module's GPIO20 (USB_D+) and GPIO19 (USB_D−); short, paired traces with no stubs (USB 1.1 Full
  Speed, so no impedance control is needed). CC → 5.1 kΩ → GND.
- **3.3 V (U2, AP2112K):** VIN = VBUS with 10 µF in; EN tied to VIN; 10 µF + 100 nF out. 3V3 feeds the
  module, the ATECC and the LED.
- **Module (U1):** 3V3 with 10 µF + 100 nF at the pin. EN: 10 kΩ to 3V3 + 1 µF to GND (Espressif's RC
  reset delay). Unused GPIOs left unconnected. Strapping pins GPIO3/45/46 left unconnected (internal
  defaults).
- **Button (SW1):** GPIO0 → switch → GND, 10 kΩ to 3V3. Held at power-up = download mode; otherwise
  the firmware's presence button (the current firmware already uses GPIO0).
- **Secure element (U4, ATECC608B):** VCC (8) = 3V3 with 100 nF, GND (4), SDA (5) and SCL (6) to two
  GPIOs with 4.7 kΩ pull-ups. Pins 1–3 and 7 unconnected, per the datasheet.
- **LED (D1, WS2812B-2020):** VDD = 3V3 with 100 nF; DIN from GPIO17 through 330 Ω; DOUT unconnected.
- **Debug (TP1):** five pads on 2.54 mm pitch along the right edge, from the antenna end: EN, RX
  (RXD0, GPIO44), TX (TXD0, GPIO43), G, 3V3, for a pogo clip or a soldered header. Bridging EN to G
  resets the chip. No auto-reset: flash over USB in download mode, logs over UART.

GPIOs, chosen from the module's pad positions: button 0, USB D−/D+ 19/20, SDA 38, SCL 21, LED 17,
UART0 43/44. The firmware gets them from `out/key/pins.h`, which the generator writes.

## Board

- 2 layers, 1.6 mm FR-4, **ENIG** (flat pads for the module's land pattern; no HASL), all parts on
  top.
- **21 × 34.95 mm.** The board ends where the module's antenna strip begins, so the **antenna
  overhangs the board edge by 5.05 mm** (Espressif's preferred placement: no board, copper or ground
  under or beside the antenna). All module pads stay on the board. The overhang is intentional and
  marked on Dwgs.User. The JST socket sits at the other end, opening outwards, so the cable leaves the
  end like the Benfei adapter.
- Button and LED on the top face, between them, so the enclosure has a button cap and a light pipe.
- Two **M2** mounting holes (2.2 mm, plated, 4.4 mm GND ring as the copper keep-out for the screw
  head or heat-set insert): H1 just past the module, right of centre, H2 beside J1, left of centre.
  The centre line is the USB path, and the module end is all module. The ring is under mask on the
  back so that the portrait can print over it. M3 would need a ~24 mm wide board.
- GND pour on both layers, stitched with vias that keep to the dark areas of the back art.
- Power trunks (VBUS, 3V3) hand-routed at 0.45 mm. The USB pair is hand-routed, 0.2 mm, and has
  B.Cu clear beneath it except for a 1.75 mm VBUS hop to U3's VBUS pin, which sits between the lines.
- Parts keep 1 mm from the long edges (assembly rails) except in the 2.8 mm strip right of the
  module (R2, C6, R4, R5, the debug pads).
- **Black** solder mask, white silkscreen.
- **Mu** on the back (house rule), 17.92 × 20.64 mm (112 × 129 px at 0.16 mm), from Martin's Photos
  library (Mu face-on with her ears up, 28 Dec 2024). Her outline is lifted with macOS Vision
  (`art/liftsubject.swift`), and `art/make_mu.py` keeps her darker features as bare black board.
  White silk = her pale fur. "Mu", "Designed by Nakomis/C", the board name and the version under her.
  No via under her white fur, and no B.Cu track under her face.

## Ordering notes (JLCPCB)

- Edge rails on the **long sides only**, joined with **mouse-bite tabs, not V-cut**: the antenna
  overhangs one short edge and J1's opening is flush with the other.
- The antenna overhang is intentional. If the engineering query asks, the module may overhang.
- Surface finish **ENIG**. Black mask, white silkscreen.
- Check the rotations of J1, D1, U1, U2, U3 and U4 in JLCPCB's placement preview.
- **Check the cable's pinout with a meter before the first plug-in.** There is no reverse-polarity
  protection: J1 is 1 VBUS, 2 D−, 3 D+, 4 CC, 5 GND.

## Cable

USB-C plug → 5-wire → JST-SH 5-pin housing (VBUS, D−, D+, CC, GND), about 10–15 cm. The CC wire is
needed because the board, not the plug, carries the 5.1 kΩ Rd. Sources, in order of preference:
1. LCSC Custom Cables (made to a drawing).
2. A ready-made "USB-C 2.0 male to JST SH 1.0 5-pin" pigtail (AliExpress), checking the pin order.
3. A USB-C plug breakout plus pre-crimped JST-SH leads (Martin crimps nothing; leads come crimped).

## Not in this board
- No battery and no USB-PD.
- No second USB port or UART bridge (that's what made the dev board confusing).

## Decided with Martin (11 Oct 2026)
1. Black board, Mu on the silkscreen.
2. Mounting holes: M2 proposed (M3 would mean a wider board).
3. Quantity: decided at order time, comparing 2 against 5 boards (Economic PCBA needs 5+).
