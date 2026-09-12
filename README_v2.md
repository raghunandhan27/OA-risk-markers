# Osteoarthritis Monitoring Prototype — Split Architecture (v2)

This version splits the system into three independent pieces:

```
[ ESP32 + sensors ]  --WiFi/HTTP JSON-->  [ Python/Flask backend ]  --serves-->  [ Browser dashboard ]
   sensor_node.ino          raw samples         backend.py + filters              static/dashboard.html
```

- **`sensor_node.ino`** — ESP32 firmware. Only reads sensors and sends raw JSON. No filtering, no web server.
- **`backend/backend.py`** — Python/Flask app. Receives raw JSON, runs ALL the filters
  (complementary filter, moving average, crepitus detection) and the heuristic risk
  score, and serves the dashboard.
- **`backend/static/dashboard.html`** — the website. Served by Flask, polls `/data`.

**Why split it up?** The ESP32 becomes a "dumb" sensor streamer, and all the
signal processing/AI logic lives in Python — which is much easier to develop,
debug, and later upgrade with a real trained ML model (scikit-learn,
TensorFlow, etc.) than editing and reflashing C++ firmware every time.

> **Disclaimer:** Still a DIY prototype, not a certified medical device. The
> risk score is a simple fixed heuristic (see `backend.py`), not a clinically
> validated algorithm.

---

## Part 1 — Hardware & wiring (identical to the single-file version)

| Qty | Part |
|---|---|
| 1 | ESP32 DevKit |
| 2 | MPU6050 breakout boards |
| 1 | Piezo disc sensor |
| 1 | FSR (Force Sensitive Resistor) |
| 1 | 10 kΩ resistor (FSR divider) |
| 1 | 100 kΩ resistor (piezo series) |
| 1 | 1 MΩ resistor (piezo bleed) |
| 2 | 1N4148 diodes (piezo ADC protection) |

**I2C (both MPU6050s share the bus):**
- VCC → 3.3V, GND → GND, SCL → GPIO 22, SDA → GPIO 21 (both boards, same wires)
- Thigh board: AD0 → GND (address `0x68`)
- Shank board: AD0 → 3.3V (address `0x69`)

**FSR (voltage divider):**
```
3.3V ---[ FSR ]---+---[ 10kΩ ]--- GND
                   |
                 GPIO 34 (ADC)
```

**Piezo (with protection):**
```
Piezo(+) --+--[100kΩ]-- GPIO 35 (ADC)
           |                  |
        [1MΩ]            [1N4148]--3.3V
           |                  |
Piezo(-)---+--- GND       [1N4148]--GND
```

**Sensor placement:** thigh IMU above the knee, shank IMU below the knee,
piezo taped near the knee joint line, FSR under the heel/forefoot in a shoe.

---

## Part 2 — Set up the Python backend (on your laptop / PC / Raspberry Pi)

1. Install **Python 3.9+** if you don't have it (check with `python3 --version`).
2. Open a terminal in the `backend/` folder (it should contain `backend.py`,
   `requirements.txt`, and the `static/` folder with `dashboard.html`).
3. (Recommended) create a virtual environment:
   ```bash
   python3 -m venv venv
   source venv/bin/activate        # on Windows: venv\Scripts\activate
   ```
4. Install dependencies:
   ```bash
   pip install -r requirements.txt
   ```
5. Find this computer's **local IP address** — you'll need it for the ESP32:
   - Windows: `ipconfig` → look for "IPv4 Address" (e.g. `192.168.1.100`)
   - Mac/Linux: `ifconfig` or `ip addr` → look for your WiFi adapter's `inet` address
6. Make sure your firewall allows incoming connections on **port 5000** on
   this computer (or temporarily disable it while testing on your home network).
7. Run the backend:
   ```bash
   python backend.py
   ```
   You should see Flask start up and listen on `0.0.0.0:5000`.
8. On the same computer, open a browser to `http://localhost:5000/` — you
   should see the dashboard (with no live data yet, since the ESP32 isn't
   sending anything).

---

## Part 3 — Set up the ESP32 sensor node

1. Install the **Arduino IDE**, the ESP32 board package, and these libraries
   (Sketch → Include Library → Manage Libraries):
   - Adafruit MPU6050
   - Adafruit Unified Sensor
   - ArduinoJson (v6.x)
2. Open `sensor_node.ino`.
3. Edit the config section near the top:
   ```cpp
   const char* WIFI_SSID     = "YOUR_WIFI_NAME";
   const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
   const char* SERVER_URL = "http://192.168.1.100:5000/ingest"; // <- your PC's IP from Part 2, step 5
   ```
   Your ESP32 and your PC must be on the **same WiFi network** for this to work.
4. Select your board and COM port, then **Upload**.
5. Open the Serial Monitor (115200 baud). You should see it connect to WiFi
   and print its own IP, followed by continuous streaming with no repeated
   "POST failed" errors (a couple at startup while the backend is still
   loading are normal).

---

## Part 4 — Watch it work

1. With `backend.py` running and the ESP32 powered on and connected, refresh
   `http://<your-PC-IP>:5000/` in your browser (use the actual IP, not
   `localhost`, if you're viewing it from a phone).
2. You should see live charts for knee angle and foot load, plus the
   crepitus counter and risk report updating.
3. Click **Reset Session** to clear ROM/crepitus/loading stats and start a
   fresh measurement window — this now resets state inside `backend.py`,
   nothing on the ESP32 needs to change.

---

## Part 5 — Calibration (now done in Python — no reflashing needed!)

Open `backend.py` and adjust the constants near the top:

| Constant | What it controls | How to tune it |
|---|---|---|
| `PIEZO_EVENT_THRESHOLD` | How big a vibration spike counts as "crepitus" | Watch raw piezo values by temporarily printing `deviation` in `process_sample()`; set the threshold comfortably above the noise floor when the joint is still |
| `ALPHA` | Complementary filter trust (gyro vs accelerometer) | Increase toward 1.0 if the angle feels jittery; decrease if it drifts over time |
| `FSR_WINDOW` | Moving-average smoothing window for the FSR | Increase for smoother but slower-responding load readings |
| `HEALTHY_ROM_DEG` | Assumed "normal" full knee flexion used in the ROM risk calc | Set to a realistic benchmark for your use case, or replace with a per-person calibration value |

Since this is just a Python file, changes take effect the moment you restart
`backend.py` — no need to touch the ESP32 at all. This is the main advantage
of the split architecture.

Also double check the IMU axis mapping in `process_sample()`
(`accThighY`/`accThighZ` etc.) matches how your sensors are actually mounted —
bend the knee slowly and watch the `angle` value in `/data` to confirm it
moves the way you expect.

---

## Part 6 — Where to plug in a real ML model later

Because all the logic now lives in `backend.py`, upgrading from the heuristic
`compute_risk_report()` function to a trained model is a matter of:

1. Logging feature snapshots (ROM, crepitus rate, load variability, etc.)
   alongside a real label (clinician assessment, symptom score) across many
   sessions — you can add a simple CSV logger inside `process_sample()`.
2. Training a model offline in Python (scikit-learn / PyTorch) on that data.
3. Loading the trained model in `backend.py` (e.g. with `joblib.load(...)`)
   and calling `model.predict(...)` inside `compute_risk_report()` instead of
   the fixed-weight formula.

No ESP32 firmware changes needed for any of this — another benefit of keeping
the "brain" in Python.

---

## Part 7 — Files in this project

```
sensor_node.ino              ESP32 firmware — reads sensors, POSTs raw JSON
backend/
  backend.py                 Flask app — filters, risk score, API routes
  requirements.txt           Python dependencies (Flask)
  static/
    dashboard.html           The website — served by Flask at "/"
```
