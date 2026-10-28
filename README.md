# Kawaii Keychain

A tiny battery-powered keychain with an OLED screen that shows animated kawaii faces. One button switches faces, and holding it opens a menu with a mini runner game, a reaction game and a fortune orb.

Built with an ESP32-C3 Super Mini, a 128x64 I2C OLED, a 250 mAh LiPo and a TP4056 charger.

## Features

- **10 animated faces**: happy, sleepy, surprised, in love, wink, shy, crying, angry, starry, kitty. They blink at random, bounce, blush, cry, twitch their ears and more.
- **One-button controls**: tap for the next face, hold for the menu.
- **Mochi Run**: a one-button runner game. Hop over thorns, catch hearts for +10. The high score is saved.
- **Reaction game**: wait for the eyes to pop open, then tap. Your best time is saved.
- **Fortune orb**: think of a yes/no question and tap for an answer.
- **Remembers** the last face and best scores after power off.
- **Auto-nap**: after 5 minutes without a press, the screen turns off and the ESP32 goes into deep sleep. Press the button to wake it.
- **Power saving**: 80 MHz CPU, Wi-Fi/Bluetooth off, low OLED contrast.

## Controls

| Where | Tap | Hold (~0.7 s) |
|---|---|---|
| Faces | Next face | Open menu |
| Menu | Next item | Choose |
| Mochi Run | Jump | Back to menu |
| Reaction | Start / react | Back to menu |
| Fortune | Ask the orb | Back to menu |

The menu goes back to the faces by itself after 15 seconds without a press.

## Components

| Part | Notes |
|---|---|
| ESP32-C3 Super Mini (USB-C) | Microcontroller |
| 128x64 I2C OLED, 4 pins (VDD, GND, SCK, SDA) | SH1106 or SSD1306 driver (see [Screen driver](#screen-driver)) |
| 3.7 V LiPo battery, 250 mAh | Red = +, black = − |
| TP4056 Type-C charger, 6-pin version **with protection** | Pins: IN+, IN−, B+, B−, OUT+, OUT− |
| Mini slide switch, 3 pins | Power on/off |
| Tactile push button, 4 pins | Face / menu / game button |
| Perf board, thin wire (28–30 AWG) | For assembly |
| **Recommended:** one 10 kΩ or 5.1 kΩ resistor | To lower the TP4056 charge current (see [Safety](#safety)) |

## Wiring

### Connections

| # | From | To | Notes |
|---|---|---|---|
| 1 | Battery red (+) | TP4056 **B+** | |
| 2 | Battery black (−) | TP4056 **B−** | |
| 3 | TP4056 **OUT+** | Slide switch **middle** pin | |
| 4 | Slide switch **one outer** pin | ESP32 **5V** | The other outer pin stays empty |
| 5 | TP4056 **OUT−** | ESP32 **G** | Must be OUT−, not B− |
| 6 | OLED **VDD** | ESP32 **3.3** | |
| 7 | OLED **GND** | ESP32 **G** | |
| 8 | OLED **SCK** | ESP32 **GPIO 9** | |
| 9 | OLED **SDA** | ESP32 **GPIO 8** | |
| 10 | Button leg | ESP32 **GPIO 3** | |
| 11 | Button leg **diagonally opposite** | ESP32 **G** | No resistor needed (internal pull-up) |

Nothing connects to TP4056 **IN+ / IN−**. Those pads are only for the charger's USB-C input.

### Diagram

```
                     TP4056 (6-pin, with protection)
   [USB-C] IN+ IN−  |  B+    B−  |  OUT+      OUT−
   (charging only)  |  |     |   |   |          |
                       |     |       |          |
          Battery red (+)   Battery black (−)   |
                                     |          |
                              Slide switch      |
                         [outer] [middle] [outer]
                            |       ^      (empty)
                            |       +-- OUT+
                            v                   v
                   +--------------------------------+
                   | 5V                           G |----+--- OLED GND
                   |                                |    +--- Button leg B
                   |   ESP32-C3 Super Mini          |
                   | 3.3 ------------------------------------ OLED VDD
                   | GPIO 9 --------------------------------- OLED SCK
                   | GPIO 8 --------------------------------- OLED SDA
                   | GPIO 3 --------------------------------- Button leg A
                   +--------------------------------+
```

All ground wires (TP4056 OUT−, ESP32 G, OLED GND, button) can meet at one shared ground point.

### 4-pin push button

The four legs form two pairs that are always connected inside the button. Pressing the button joins the two pairs. Use two **diagonally opposite** legs and it works however the button is turned.

```
  1 ●━━━━━━━● 2     1–2 always connected
    |  btn  |       3–4 always connected
  3 ●━━━━━━━● 4     pressing joins them -> use 1 & 4 or 2 & 3
```

### Slide switch

The middle pin is the common pin. It connects to one outer pin in one position and to the other outer pin in the other position. If the wires are on the two outer pins, the switch never connects. Solder the switch quickly (1–2 s per pin), because these switches melt easily.

## Safety

- **Slide the switch OFF before plugging USB into the ESP32.** On many Super Mini boards the 5V pin connects straight to USB 5V. With the switch ON, USB power would flow into the battery.
- **Never connect the battery to the 3.3 pin.** A full LiPo is 4.2 V, and the ESP32-C3 is rated for at most 3.6 V. The 5V pin goes through the board's regulator.
- **ESP32 GND goes to OUT−, not B−.** Otherwise the battery protection is bypassed.
- **Check battery polarity** before connecting. Reversing it destroys the TP4056.
- **Charge current:** most TP4056 boards ship with a 1.2 kΩ resistor (R3 / "PROG", marked `122`), which sets **1 A**. That's too much for a 250 mAh cell. Replace it with:

  | Resistor | Colour bands | Charge current |
  |---|---|---|
  | 10 kΩ | brown, black, orange | about 130 mA |
  | 5.1 kΩ | green, brown, red | about 240 mA |
  | 4.7 kΩ | yellow, violet, red | about 250 mA |

  Replace the resistor; don't add one alongside it, because a parallel resistor *increases* the current. Until it's swapped, only charge while you're nearby, with the switch OFF, and unplug it if the battery gets hot.
- Keep the battery off the TP4056 board, which heats up while charging. **Never use a swollen battery.**

## Charging and battery life

- Charge through the **TP4056's** USB-C port. **Red** LED = charging, **blue/green** = full. The TP4056 stops charging by itself when the battery is full.
- Charge with the switch **OFF**, so the charger can tell when the battery is full.
- Upload code through the **ESP32's** USB-C port, with the switch **OFF**.

| Usage | Approximate runtime (250 mAh) |
|---|---|
| Screen on (faces or games) | 6–8 hours |
| Napping | 3–5 days (mostly the ESP32's red power LED) |
| Switch OFF | months |

Removing the Super Mini's red power LED roughly doubles the nap time.

## Software setup

### Arduino IDE

1. Install **Arduino IDE 2.x**.
2. **File → Preferences → Additional boards manager URLs:**
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. **Boards Manager:** install **esp32 by Espressif Systems** (3.x).
4. **Library Manager:** install **Adafruit SH110X** (for SH1106 screens) or **Adafruit SSD1306**. Choose "Install All" to also get Adafruit GFX and BusIO. `Preferences` is included with the ESP32 package.
5. Open `KawaiiKeychain/KawaiiKeychain.ino` and set **Tools**:

   | Setting | Value |
   |---|---|
   | Board | ESP32C3 Dev Module |
   | USB CDC On Boot | **Enabled** |
   | Flash Mode | **DIO** |
   | Flash Size | 4MB |
   | Port | the ESP32's COM port |

6. Slide the switch **OFF**, plug in the ESP32 and click **Upload**.

### arduino-cli

```sh
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install "Adafruit SH110X" "Adafruit SSD1306" "Adafruit GFX Library" "Adafruit BusIO"

arduino-cli compile --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc,FlashMode=dio" KawaiiKeychain
arduino-cli upload  --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc,FlashMode=dio" -p COM7 KawaiiKeychain
```

Replace `COM7` with your port.

## Settings

All settings are at the top of `KawaiiKeychain.ino`:

| Setting | Default | What it does |
|---|---|---|
| `USE_SH1106` | `1` | `1` = SH1106 screen, `0` = SSD1306 |
| `SDA_PIN` / `SCL_PIN` / `BUTTON_PIN` | `8` / `9` / `3` | Pin numbers |
| `OLED_CONTRAST` | `0x30` | Brightness, `0x00`–`0xFF`. Lower = longer battery life |
| `FRAME_MS` / `GAME_FRAME_MS` | `50` / `33` | Time between frames for faces / the runner game |
| `LONG_PRESS_MS` | `700` | How long a hold has to be |
| `SAVE_LAST_FACE` | `1` | Remember the face after power off |
| `AUTO_SLEEP_MINUTES` | `5` | Nap after this many idle minutes (`0` = never) |
| `DEBUG_SERIAL` | `1` | Serial Monitor messages. Set to `0` for the finished keychain |

### Screen driver

Many "0.96 inch SSD1306" modules actually use an **SH1106** chip. If the picture is squeezed into a thin strip at the top with random noise below it, change `USE_SH1106`. The sketch finds the screen at I2C address `0x3C` or `0x3D` automatically. A module labelled "0x78" is the same as 0x3C.

## Troubleshooting

| Problem | Check |
|---|---|
| Blank screen, Serial Monitor says "OLED not found" | Wiring: SDA→8, SCK→9, VDD→3.3, GND→G. Look for cold joints (dull blobs) and solder bridges. The sketch keeps retrying every second. |
| Thin strip with noise below it | Wrong screen driver. Change `USE_SH1106`. |
| Button does nothing | Use **diagonal** button legs. Make sure it's on GPIO 3, not the 3.3 pin. |
| No COM port | Use a data USB cable (not charge-only). Hold **BOOT**, tap **RST**, release BOOT, then upload. A napping board disappears from USB, so press the button first. |
| Boot loop after upload | Set Flash Mode to **DIO**. |
| Works on USB but not on battery | Charge the battery first (a flat battery trips the TP4056 protection). Check the switch: OUT+ on the **middle** pin, 5V on an outer pin. To test, briefly bridge OUT+ to 5V. If it powers up, the switch or its joints are at fault. |
| Wakes from nap over and over | Add a 10 kΩ pull-up from GPIO 3 to 3.3, or set `AUTO_SLEEP_MINUTES` to `0`. |

## Fitting it in a case

- Rough part sizes: OLED module about 27×27 mm, Super Mini about 22×18 mm, TP4056 about 28×17 mm, 250 mAh LiPo about 30×20×4 mm.
- Remove the header pins and solder thin wires directly to save height.
- Insulate the boards from each other and from the battery with Kapton or electrical tape.
- The case needs openings for: the TP4056 USB-C port (charging), the ESP32 USB-C port (updates), the slide switch, the button and the screen window.
- Leave about 1 mm of space around the battery. LiPos swell slightly with age.
