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
| Decoupling | 100 nF 0402 / 1 µF 0402 / 10 µF 0603 | C1525 / C52923 / C19702 | Basic | |

Prices at quantity 1, in USD: module $4.68, ATECC $1.00, socket $0.55, LDO $0.16, ESD $0.16, LED $0.09;
the rest are pennies. Extended parts add JLCPCB's per-part loading fee, which is accepted (none of them
has a Basic equivalent).

## Circuit

- **USB in (J1, JST-SH):** VBUS → U3 (USBLC6) VBUS pin → 10 µF → LDO input. D+/D− through U3 to the
  module's GPIO20 (USB_D+) and GPIO19 (USB_D−); short, paired traces with no stubs (USB 1.1 Full
  Speed, so no impedance control is needed). CC → 5.1 kΩ → GND.
- **3.3 V (U2, AP2112K):** VIN = VBUS with 1 µF in; EN tied to VIN; 10 µF + 100 nF out. 3V3 feeds the
  module, the ATECC and the LED.
- **Module (U1):** 3V3 with 10 µF + 100 nF at the pin. EN: 10 kΩ to 3V3 + 1 µF to GND (Espressif's RC
  reset delay). Unused GPIOs left unconnected. Strapping pins GPIO3/45/46 left unconnected (internal
  defaults).
- **Button (SW1):** GPIO0 → switch → GND, 10 kΩ to 3V3. Held at power-up = download mode; otherwise
  the firmware's presence button (the current firmware already uses GPIO0).
- **Secure element (U4, ATECC608B):** VCC (8) = 3V3 with 100 nF, GND (4), SDA (5) and SCL (6) to two
  GPIOs with 4.7 kΩ pull-ups. Pins 1–3 and 7 unconnected, per the datasheet.
- **LED (D1, WS2812B-2020):** VDD = 3V3 with 100 nF; DIN from one GPIO; DOUT unconnected.
- **Debug (TP):** pads for GND, 3V3, TXD0 (GPIO43), RXD0 (GPIO44) at the board edge for a pogo clip
  or a soldered header. No auto-reset: flash over USB in download mode, logs over UART.

GPIO choice for I2C and the LED is left to placement: any free non-strapping GPIO on the side nearest
the part (e.g. GPIO8/9 for I2C and GPIO38 for the LED), set once the module footprint's pad order is
known. The firmware gets the pin numbers from a header the generator writes.

## Board

- 2 layers, 1.6 mm FR-4, HASL (lead-free if Economic PCBA allows), all parts on top.
- About 20 × 40 mm. The module sits at one end with its **antenna overhanging the board edge or over
  a copper-free keep-out** (Espressif: no copper, traces or ground under or near the antenna). The JST
  socket sits at the other end, opening outwards, so the cable leaves the end like the Benfei adapter.
- Button and LED on the top face, between them, so the enclosure has a button cap and a light pipe.
- Two **M2** mounting holes (2.2 mm, plated, with a 4.4 mm copper keep-out for the screw head or
  heat-set insert), one at each end on the centre line. M3 would need a ~24 mm wide board.
- GND pour on both layers, stitched with vias, kept out of the antenna area.
- **Black** solder mask, white silkscreen.
- **Mu** on the back (house rule), about 18 × 20.5 mm, from Martin's Photos library (a close-up of
  his stare, 4 Jul 2026), thresholded and masked into a head-and-ears outline:
  `art/mu.png`, made by `art/make_mu.py`. White silk = his pale fur. "Designed by Nakomis/C", the
  board name and the version under him.

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
