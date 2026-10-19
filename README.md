# ClimateNet DIY Node

Firmware for an ESP32-based ClimateNet air-quality and weather station. Every 10 minutes the node wakes up on a DS3231 RTC alarm, warms its sensors up for 30 s, averages readings for 60 s, sends the result to the ClimateNet cloud, and goes back to deep sleep.

## Hardware

| Part | Purpose | I2C address |
|------|---------|-------------|
| ESP32 dev board | Controller, Wi-Fi | – |
| DS3231 | Real-time clock, wake-up alarm | 0x68 |
| SHT45 | Temperature, humidity | 0x44 |
| SCD41 | CO₂ | 0x62 |
| SPS30 (SEL pin to GND) | PM1 / PM2.5 / PM10 | 0x69 |
| LTR-390 | Ambient light, UV index | 0x53 |

| Signal | ESP32 pin |
|--------|-----------|
| I2C SDA | GPIO 21 |
| I2C SCL | GPIO 22 |
| RTC SQW (alarm) | GPIO 25 |
| Fan tachometer | GPIO 27 |
| Battery divider (1 MΩ / 1 MΩ) | GPIO 34 |

## Setup

### 1. Download the repository

Click **Code → Download ZIP** on the repository page (or `git clone` it) and unzip it.

### 2. Move the sketch to your Arduino folder

Move the sketch folder into your Arduino sketchbook folder:

- Windows: `Documents\Arduino`
- macOS: `~/Documents/Arduino`
- Linux: `~/Arduino`

The folder name must match the `.ino` file name (for example, `portable_device_cop/portable_device_cop.ino`), otherwise the Arduino IDE will ask to move it.

### 3. Open the Arduino IDE

Install the [Arduino IDE](https://www.arduino.cc/en/software) if you don't have it, then open the `.ino` file.

**Install ESP32 board support** (needed for `WiFi`, `WiFiClientSecure`, `HTTPClient` and deep sleep):

1. Go to **File → Preferences** and add this URL to *Additional boards manager URLs*:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
2. Open **Tools → Board → Boards Manager**, search for **esp32** and install **esp32 by Espressif Systems**.
3. Select **Tools → Board → esp32 → ESP32 Dev Module**.

### 4. Install the libraries

Open **Sketch → Include Library → Manage Libraries…** (or the Library Manager icon on the left), search for each library below and click **Install**. When the IDE asks to install dependencies, choose **Install All**.

| Search for | Library | Author | Used for |
|------------|---------|--------|----------|
| `RTClib` | RTClib | Adafruit | DS3231 clock |
| `ArduinoJson` | ArduinoJson (v6 or v7) | Benoit Blanchon | Building the data packet |
| `Sensirion I2C SCD4x` | Sensirion I2C SCD4x | Sensirion | SCD41 CO₂ sensor |
| `Sensirion I2C SPS30` | Sensirion I2C SPS30 | Sensirion | SPS30 particulate sensor |
| `Adafruit SHT4x` | Adafruit SHT4x Library | Adafruit | SHT45 temperature / humidity |
| `Adafruit LTR390` | Adafruit LTR390 Library | Adafruit | LTR-390 light / UV |

These dependencies are installed automatically with *Install All*: **Adafruit BusIO**, **Adafruit Unified Sensor** and **Sensirion Core**.

`Wire`, `WiFi`, `WiFiClientSecure`, `HTTPClient` and `driver/rtc_io.h` come with the ESP32 board package and don't need to be installed separately.

> Use the latest versions of the Sensirion libraries. The sketch uses the new class names (`SensirionI2cScd4x`, `SensirionI2cSps30`), which older versions don't have.

### 5. Request your credentials

Go to [climatenet.am/en/diy](https://climatenet.am/en/diy/) and fill in the request form for your device credentials.

### 6. Check your email

When your request is processed you will receive an email with your credentials. Download the attached ZIP file.

### 7. Unzip and rename the certificate files

Unzip the archive. The name of the unzipped folder is your **thing name**; you'll need it later.

Rename these files so they open in any text editor:

| Original name | Rename to |
|---------------|-----------|
| `certificate.pem.crt` | `certificate.pem.txt` |
| `private.pem.key` | `private.pem.txt` |
| `rootCA.pem` | `rootCA.txt` |

> On Windows, turn on **View → File name extensions** in File Explorer first, otherwise you may end up with `certificate.pem.crt.txt`.

### 8. Fill in `secrets.h`

Open the `secrets.h` tab in the Arduino IDE. Open each renamed file in a text editor, copy its **entire** content (including the `-----BEGIN …-----` and `-----END …-----` lines), and paste it into the matching place:

| Copy from | Paste into |
|-----------|------------|
| `rootCA.txt` | Root CA certificate |
| `certificate.pem.txt` | Device certificate |
| `private.pem.txt` | Device private key |
| IoT endpoint line in `env.txt` | IoT endpoint |

### 9. Add Wi-Fi credentials and thing name

In the same `secrets.h`, fill in:

- your Wi-Fi network name (SSID) and password (the ESP32 supports 2.4 GHz networks only)
- your thing name, which is the name of the unzipped credentials folder

A filled-in `secrets.h` usually looks like this (variable names in your file may differ slightly):

```cpp
#define THINGNAME "your-thing-name"            // name of the unzipped folder

const char WIFI_SSID[]     = "your-wifi-name";
const char WIFI_PASSWORD[] = "your-wifi-password";
const char AWS_IOT_ENDPOINT[] = "xxxxxxxxxxxx-ats.iot.eu-central-1.amazonaws.com";  // from env.txt

// Root CA (rootCA.txt)
static const char AWS_CERT_CA[] PROGMEM = R"EOF(

)EOF";

// Device certificate (certificate.pem.txt)
static const char AWS_CERT_CRT[] PROGMEM = R"KEY(

)KEY";

// Device private key (private.pem.txt)
static const char AWS_CERT_PRIVATE[] PROGMEM = R"KEY(

)KEY";
```

> Never commit a filled-in `secrets.h` to a public repository. Anyone with these files can send data as your device.

### 10. Upload the code to the ESP32

1. Connect the ESP32 to your computer with a USB data cable.
2. Select the port in **Tools → Port** (`COMx` on Windows, `/dev/cu.usbserial-…` or `/dev/ttyUSB0` on macOS / Linux).
3. Click **Upload** (→).
4. If the upload stalls at `Connecting…`, hold the **BOOT** button on the board until uploading starts.

## Checking that it works

Open **Tools → Serial Monitor** at **115200 baud** and press the board's **EN/RST** button. After about 90 seconds you should see a summary line like:

```
T 22.4  RH 41  PM2.5 6.3  CO2 512 lux 340  UVI 0.2  fan 2400 rpm  batt 4.05 V
```

followed by the JSON packet, the server response (`AWS HTTP 200` means success) and `Sleeping … s`.

On first start, or whenever the RTC has lost power, the node connects to Wi-Fi to sync the time from NTP. Timestamps use Armenia time (UTC+4).

## Troubleshooting

| Message / symptom | What to check |
|-------------------|---------------|
| `DS3231 not found` | Wiring of SDA/SCL and power. The sketch prints the I2C devices it can see and whether pull-ups are present (both pins must read `1`). It retries every 60 s. |
| `WiFi failed, data not sent` | SSID and password in `secrets.h`, and that the network is 2.4 GHz. |
| `AWS HTTP` with a negative number or 4xx | Endpoint, certificates or thing name in `secrets.h`. Make sure each certificate was pasted in full. |
| A sensor value is `nan` / `null` | That sensor isn't responding. Check its wiring and I2C address. |
| Compile error about `SensirionI2cScd4x` or `SensirionI2cSps30` | Update the Sensirion libraries to the latest version. |
