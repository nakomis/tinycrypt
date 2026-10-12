# tinycrypt key, bare-chip variant (CRYPT-24)

`--board bare` (`board/bare_board.py`). The same key as `DESIGN.md`, with the ESP32-S3-MINI-1 module
replaced by the bare ESP32-S3FN8 and the parts the module had inside it. The point: JLCPCB places
every ESP32-S3 module (MINI-1, WROOM-1, checked 12 Oct 2026) on Standard PCBA only, and the bare chip
is Economic-eligible. **Untested: no board has been made.**

## Is it worth it?

Not obviously. The module is about $4.71 and the chip $2.10, so parts cost per board barely moves
($6.86 against $6.42 for the whole BOM at JLCPCB's single-unit prices). The saving is the PCBA tier,
a per-order amount:

| | Module board (Standard, Martin's quote for 5) | This board (Economic, estimate) |
|---|---|---|
| Setup | £19.40 | about £6 |
| Feeder loading | £16.35 (14 part types at £1.17) | none |
| Extended-part fee | £13.30 (6 types) | about £27 (12 types) |
| Stencil, SMT, X-ray, packaging | £9.84 | about £5 |
| **PCBA total** | **£58.89** | **about £36** |

So roughly £20–25 an order, against the risks below. JLCPCB's quote is the real number: upload the
zip, `bom.csv` and `cpl.csv` from `out/bare/`.

## Parts (all "Economic and Standard" on their JLCPCB pages, 12 Oct 2026)

| Block | Part | LCSC | Notes |
|---|---|---|---|
| MCU | ESP32-S3FN8 | C2913196 | QFN-56 7 × 7; 8 MB flash in the package, 3.3 V |
| Crystal | 40 MHz ±10 ppm, 10 pF, 2520 | C706805 | Espressif: 40 MHz only, ±10 ppm |
| XTAL_P series | 24 nH 0402 | C74301 | Espressif's starting value ("R4") |
| Crystal load | 15 pF C0G 0402 ×2 | C1548 | Starting value: C = 2(CL − Cstray) for CL 10 pF; tune |
| RF supply filter | 2.0 nH 0402, 900 mA | C18216 | Murata LQG15HS; MINI-1's L1 is 2.0 nH |
| RF match | 1.5 pF ×2, 2.4 nH, 0402 | C1552, C97997 | Espressif's ranges: C 1.2–1.8 pF, L 2.0–3.0 nH; tune |
| Antenna | Johanson 2450AT18D0100E | C2917718 | 3.2 × 1.6 mm, "detuning resilient", edge mount |
| LED | XL-1615RGBC-RF | C965840 | 1.6 × 1.5 mm common-anode RGB, three GPIOs |
| LED resistors | 330 Ω (R), 100 Ω (G, B) | C25104, C25076 | G and B have ~3 V forward voltage: little headroom |
| VDDA | 10 nF | C15195 | |

The rest (JST, USBLC6, AP2112K, ATECC608B, switch, passives) is the module board's. **0201 parts
are all Standard Only**, so the RF parts are 0402, not Espressif's recommended 0201. **No
Economic addressable LED is rated for 3.3 V** (SK6805/SK6812 parts need 3.7 V or more), hence the
plain RGB.

## Circuit, and where each value comes from

From the ESP32-S3-MINI-1 datasheet v1.7, figure 8-1 (the module's own schematic), and Espressif's
ESP32-S3 hardware design guidelines (schematic checklist and PCB layout):

- **VDD3P3 (pins 2, 3), the RF supply:** 3V3 with 10 µF, 1 µF, 100 nF, then 2.0 nH in series, then
  100 nF at the pins.
- **VDDA (55, 56):** 10 nF at pin 56. The schematic also has 1 µF here; it is C11 at pin 46 instead,
  on the same 3V3, which reaches VDDA under the chip.
- **VDD3P3_CPU (46):** 100 nF (+ 1 µF, C11). **VDD3P3_RTC (20):** 100 nF.
- **VDD_SPI (29):** 100 nF + 1 µF and nothing else. The FN8's flash is 3.3 V, powered from
  VDD3P3_RTC inside the chip while GPIO45 is low (its internal pull-down; never pull it up).
- **CHIP_PU:** 10 kΩ + 1 µF (as the module board), and debug pad EN.
- **Crystal:** XTAL_P through 24 nH, load capacitors to tune with a TX tone to ±10 ppm.
- **RF:** LNA_IN → 1.5 pF shunt → 2.4 nH series → 1.5 pF shunt → the antenna. Values vary with the
  board and must be tuned (Espressif); Johanson's 0 Ω matching pads are not included.
- **Straps:** GPIO0 is the button (as before); GPIO3, 45 and 46 unconnected.
- **Pins:** button 0; LED R/G/B 10/11/12 (cathodes, active low); SDA 47, SCL 21; USB 19/20; console
  UART TX 33, RX 34 (UART0 through the GPIO matrix, `CONFIG_ESP_CONSOLE_UART_CUSTOM`; the ROM's first
  messages still go to U0TXD, which is not wired). `out/bare/pins.h` has them.

## Layout

- 2 layers, 1.6 mm, 21 × 34.95 mm, the module board's outline. The lower half is the module board's
  placement; the top 13 mm is new.
- **Antenna** on the top edge, top left, fed from the right (datasheet function 2: pad 4 the feed,
  pads 1–3 GND). Its clearance, 4.0 × 6.0 mm from the edge with no copper on either layer, is a rule
  area with notches for its own pads.
- **RF line:** 1.1 mm grounded coplanar with the pours' 0.2 mm gap, 52 Ω by the conformal-mapping
  formula for 1.53 mm FR-4 at εr 4.5 (about 6 mm long, so the match matters more than the impedance).
- **Power under the chip:** Espressif's two-layer scheme. The exposed pad is shrunk from 4.0 to
  3.4 mm (`footprints/local.pretty`) and 3V3 runs in the 1.08 mm moat between it and the pin row,
  from pin 46 to VDDA (55, 56) and VDD3P3_RTC (20). Without it VDDA can't be fed: the RF line to the
  board edge cuts the corner off on F.Cu, and B.Cu under the RF, the crystal and the chip stays
  unbroken ground.
- **Exposed pad:** nine 0.3 mm vias (numbered 57 so that they are GND, tented, no paste) and a
  −0.4 mm paste margin (58 % coverage).
- **3V3 trunks** on B.Cu: up the left edge (RF supply caps, pull-ups, LED), along the bottom under J1
  (as the module board), up the right edge to pin 46's caps. EN reaches its debug pad on B.Cu,
  around below the chip.
- **KiCad 10's DRC does not report tracks or vias inside the ANTENNA rule area** (tested: a planted
  track goes unreported; the pour does stay out). Freerouting once drew GND tracks across it, so the
  router gets the clearance as a keep-out and `build.py` refuses any copper in it
  (`clearance_violations`).
- The nine thermal vias show as dots in Mu's face on the back: the chip is under her.

## Risks

- **RF untuned.** Without a network analyser the match is Espressif's starting values; range may be
  shorter than the module's. The watch is usually close, but it's only known with a board.
- **0402 RF parts** against Espressif's 0201.
- **Not radio-certified** (the module is). Fine for personal use.
- **Tight placement:** several 0402s sit 0.13–0.2 mm from neighbours on the same net (the crystal
  and its caps, decoupling at the chip). JLCPCB's DFM review may comment.
- **Firmware** needs changes before this board runs: LED driver (LEDC PWM, not the WS2812/RMT), I2C
  pins (47/21), console UART on custom pins.
