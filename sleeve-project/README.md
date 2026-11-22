# Sleeve: wearable heat-stress monitor (electronics and firmware)

A sensor hub on an ESP32-S3 that tracks heat and activity on the wearer and alerts them locally with an LED, an OLED readout and a vibration motor.

## Layout
| Folder | What's in it |
|---|---|
| `firmware/sleeve_cloud/` | **Main firmware** for the XIAO ESP32-S3. Reads all sensors and drives the LED, OLED and haptic motor. It also uploads readings over WiFi to a Firebase Realtime Database (see the note below). |
| `firmware/tests/` | Small single-purpose sketches for bench testing each sensor, plus `wifi_firebase_basic` for testing WiFi and Firebase on their own. |

## Hardware
| Part | Job | I²C address |
|---|---|---|
| XIAO ESP32-S3 | Controller + WiFi | n/a |
| SSD1306 0.96" OLED | Local readout | 0x3C |
| SHT45 | Air temp + humidity | 0x44 |
| TMP119 | Skin temp | 0x48 |
| LTR390 | UV index | 0x53 |
| MAX30102 | Heart rate | 0x57 |
| LIS3DH (or LIS3DSH) | Movement | 0x18/0x19 (LIS3DH), 0x1E/0x1D (LIS3DSH) |
| Coin vibration motor, 3 V | Haptic alert (via transistor) | n/a |

### Pin map (XIAO ESP32-S3)
| Pin | Connects to |
|---|---|
| D4 / D5 | SDA / SCL (all I²C sensors, 100 kHz) |
| D0 | MAX30102 INT (unused so far) |
| D1 | Button to GND (unused so far) |
| D2 | Status LED through 7.5 kΩ |
| D3 | Vibration motor transistor base through 1 kΩ |
| 3V3 / GND | Power rails |

- **Accelerometer:** CS to 3V3 (selects I²C). SDO/SA0 to GND or 3V3 sets the address. Leave INT1, INT2 and ADC1-3 unconnected.
- **LTR390:** power into VIN. Leave 3Vo unconnected.
- **Vibration motor:** motor between 3V3 and the transistor collector, diode across the motor (stripe on the 3V3 side), emitter to GND. Use an NPN transistor such as a 2N2222 or 2N3904.

## Firmware setup
1. Arduino IDE with the **ESP32 board package**. Board: *XIAO_ESP32S3*. Tools > USB CDC On Boot: **Enabled**.
2. Install libraries (Library Manager): Adafruit SSD1306, Adafruit GFX, SparkFun MAX3010x, **Adafruit LTR390** (accept Adafruit BusIO). Wire, WiFi and HTTPClient are built in.
3. Open `firmware/sleeve_cloud/sleeve_cloud.ino`. Copy `config.example.h` to `config.h` and fill in your values. **Never commit config.h.**
4. Upload. The Serial Monitor (115200 baud) prints each sensor's status at startup and a status line every second.

Arduino gotcha: every `.ino` in a sketch folder is compiled together, so keep one `.ino` per folder, named like the folder.

## WiFi upload (optional)
The firmware sends readings to a Firebase Realtime Database every 15 s (`readings/<deviceId>` and `latest/<deviceId>`; fields `t`, `hr`, `skin`, `air`, `rh`, `uv`, `act`, `pk`, `alert`). It needs the WiFi and Firebase values in `config.h`. To check WiFi and Firebase before using the full firmware, run `firmware/tests/wifi_firebase_basic/` first. It needs no sensors, sends a made-up value every 10 s, and prints the server's reply (200 means it worked). Without WiFi the sensors, OLED, LED and haptic still work, and the upload task just waits.

## Status
| Area | State |
|---|---|
| TMP119, SHT45 reads, MAX30102 heart rate, OLED | Working on the bench |
| LTR390, LIS3DH | Working (LIS3DH verified at 0x18) |
| SHT45 humidity response to breath | Unresolved. Heater test passes; steam/salt-chamber test pending |
| Vibration motor circuit | Written, untested |
| WiFi/Firebase upload | Tested only against a fake server. Not run on hardware. `wifi_firebase_basic` has not been run either |

## Known limits
- UV doesn't pass through most plastic: the LTR390 needs a clear window facing the sun.
- This is a hobby prototype, not a medical or safety device.

## To do
- Hardware test of the haptic motor and the WiFi upload.
- Resolve the SHT45 humidity response.
