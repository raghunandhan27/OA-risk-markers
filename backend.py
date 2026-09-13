"""
Osteoarthritis Monitoring Prototype — BACKEND (runs on your PC/laptop/Raspberry Pi)
=====================================================================================
Receives raw sensor JSON from the ESP32 sensor node, applies:
  - a complementary filter -> knee flexion angle
  - a moving-average filter -> smoothed foot loading (FSR)
  - baseline-subtraction + threshold detection -> crepitus events
then computes a heuristic risk score, and serves a live dashboard.

Run with:
    pip install -r requirements.txt
    python backend.py

Then open http://<this-computer's-IP>:5000/ in a browser.
The ESP32's sensor_node.ino should POST to http://<this-computer's-IP>:5000/ingest
"""

import time
import math
import threading
from collections import deque

from flask import Flask, request, jsonify, send_from_directory

app = Flask(__name__, static_folder="static", static_url_path="")

# ---------------------------------------------------------------------------
# TUNABLE CONSTANTS  (edit these here — no need to re-flash the ESP32!)
# ---------------------------------------------------------------------------
ALPHA = 0.96                     # complementary filter: trust gyro short-term, accel long-term
FSR_WINDOW = 12                  # moving-average window size for the FSR
PIEZO_BASELINE_ALPHA = 0.98      # how slowly the "quiet joint" baseline adapts
PIEZO_EVENT_THRESHOLD = 250      # ADC counts above baseline counted as a crepitus event (TUNE THIS)
PIEZO_REFRACTORY_S = 0.12        # debounce window for crepitus events
RISK_UPDATE_INTERVAL_S = 5       # how often the plain-English report is recomputed
HEALTHY_ROM_DEG = 130.0          # assumed healthy full knee-flexion ROM for this demo

# ---------------------------------------------------------------------------
# SESSION STATE (single-user prototype -> plain globals behind a lock)
# ---------------------------------------------------------------------------
lock = threading.Lock()


def fresh_state(name="", age=None):
    return {
        "userName": name,
        "userAge": age,
        "angleThigh": 0.0,
        "angleShank": 0.0,
        "kneeAngle": 0.0,
        "lastT": None,                 # last ESP32 timestamp (ms) seen, for dt
        "sessionMin": float("inf"),
        "sessionMax": float("-inf"),
        "piezoBaseline": None,
        "lastCrepitusTime": 0.0,
        "crepitusCount": 0,
        "fsrBuffer": deque(maxlen=FSR_WINDOW),
        "fsrPrevFiltered": None,
        "fsrVarAccum": 0.0,
        "fsrVarSamples": 0,
        "sessionStart": time.time(),
        "riskScore": 0.0,
        "riskCategory": "Collecting data...",
        "report": "Move the joint through a few gentle reps to begin collecting data.",
        "lastRiskUpdate": 0.0,
    }


state = fresh_state()


def clamp(v, lo, hi):
    return max(lo, min(hi, v))


# ---------------------------------------------------------------------------
# CORE FILTER PIPELINE — this is the "backend filters" piece
# ---------------------------------------------------------------------------
def process_sample(d):
    """Takes one raw JSON sample from the ESP32 and updates all filters/state."""
    t = d["t"] / 1000.0  # ms -> s
    if state["lastT"] is None:
        dt = 0.02
    else:
        dt = t - state["lastT"]
        if dt <= 0:
            dt = 0.02
    state["lastT"] = t

    # --- Complementary filter: fuse accelerometer (stable long-term) with
    #     gyroscope (accurate short-term, but drifts) into one clean angle ---
    acc_angle_thigh = math.degrees(math.atan2(d["accThighY"], d["accThighZ"]))
    acc_angle_shank = math.degrees(math.atan2(d["accShankY"], d["accShankZ"]))
    gyro_rate_thigh = math.degrees(d["gyroThighX"])
    gyro_rate_shank = math.degrees(d["gyroShankX"])

    state["angleThigh"] = ALPHA * (state["angleThigh"] + gyro_rate_thigh * dt) + (1 - ALPHA) * acc_angle_thigh
    state["angleShank"] = ALPHA * (state["angleShank"] + gyro_rate_shank * dt) + (1 - ALPHA) * acc_angle_shank
    state["kneeAngle"] = state["angleShank"] - state["angleThigh"]

    if state["kneeAngle"] < state["sessionMin"]:
        state["sessionMin"] = state["kneeAngle"]
    if state["kneeAngle"] > state["sessionMax"]:
        state["sessionMax"] = state["kneeAngle"]

    # --- Moving-average filter on the FSR, plus a running variability estimate
    #     (a proxy for uneven / guarded loading) ---
    fsr_raw = d["fsrRaw"]
    state["fsrBuffer"].append(fsr_raw)
    filtered = sum(state["fsrBuffer"]) / len(state["fsrBuffer"])
    if state["fsrPrevFiltered"] is not None:
        state["fsrVarAccum"] += abs(filtered - state["fsrPrevFiltered"])
    state["fsrPrevFiltered"] = filtered
    state["fsrVarSamples"] += 1

    # --- Baseline-subtraction + threshold crossing -> crepitus event detection ---
    piezo_raw = d["piezoRaw"]
    if state["piezoBaseline"] is None:
        state["piezoBaseline"] = piezo_raw
    state["piezoBaseline"] = (
        PIEZO_BASELINE_ALPHA * state["piezoBaseline"] + (1 - PIEZO_BASELINE_ALPHA) * piezo_raw
    )
    deviation = abs(piezo_raw - state["piezoBaseline"])
    now = time.time()
    if deviation > PIEZO_EVENT_THRESHOLD and (now - state["lastCrepitusTime"]) > PIEZO_REFRACTORY_S:
        state["crepitusCount"] += 1
        state["lastCrepitusTime"] = now

    if now - state["lastRiskUpdate"] > RISK_UPDATE_INTERVAL_S:
        state["lastRiskUpdate"] = now
        compute_risk_report()


def get_fsr_filtered():
    if not state["fsrBuffer"]:
        return 0
    return sum(state["fsrBuffer"]) / len(state["fsrBuffer"])


# ---------------------------------------------------------------------------
# HEURISTIC RISK SCORE (same logic as before — now editable without reflashing)
# ---------------------------------------------------------------------------
def compute_risk_report():
    rom = state["sessionMax"] - state["sessionMin"]
    if state["sessionMax"] < state["sessionMin"]:
        rom = 0.0

    elapsed_min = max(0.05, (time.time() - state["sessionStart"]) / 60.0)
    crepitus_rate = state["crepitusCount"] / elapsed_min

    avg_variability = (
        state["fsrVarAccum"] / state["fsrVarSamples"] if state["fsrVarSamples"] > 1 else 0.0
    )
    load_risk = clamp((avg_variability / 40.0) * 100.0, 0, 100)
    rom_risk = clamp(((HEALTHY_ROM_DEG - rom) / HEALTHY_ROM_DEG) * 100.0, 0, 100)
    crepitus_risk = clamp(crepitus_rate * 8.0, 0, 100)

    score = 0.40 * rom_risk + 0.35 * crepitus_risk + 0.25 * load_risk
    state["riskScore"] = score

    if score < 30:
        category = "Low"
    elif score < 60:
        category = "Moderate"
    else:
        category = "High"
    state["riskCategory"] = category

    state["report"] = (
        f"ROM so far: {rom:.1f}\u00b0 (deficit contributes {rom_risk:.0f}/100). "
        f"Crepitus: {state['crepitusCount']} events ({crepitus_rate:.1f}/min, "
        f"contributes {crepitus_risk:.0f}/100). "
        f"Loading variability contributes {load_risk:.0f}/100. "
        f"This is a heuristic indicator for a DIY prototype, not a diagnosis."
    )


# ---------------------------------------------------------------------------
# ROUTES
# ---------------------------------------------------------------------------
@app.route("/ingest", methods=["POST"])
def ingest():
    """Called by the ESP32 sensor node with one raw sample."""
    data = request.get_json(force=True, silent=True)
    if not data:
        return jsonify({"status": "error", "message": "no/invalid JSON body"}), 400
    with lock:
        process_sample(data)
    return jsonify({"status": "ok"})


@app.route("/data", methods=["GET"])
def data():
    """Polled by the browser dashboard twice a second."""
    with lock:
        rom = state["sessionMax"] - state["sessionMin"]
        if state["sessionMax"] < state["sessionMin"]:
            rom = 0.0
        elapsed_min = max(0.05, (time.time() - state["sessionStart"]) / 60.0)
        crepitus_rate = state["crepitusCount"] / elapsed_min
        payload = {
            "userName": state["userName"],
            "userAge": state["userAge"],
            "angle": state["kneeAngle"],
            "rom": rom,
            "crepitusCount": state["crepitusCount"],
            "crepitusRate": crepitus_rate,
            "fsrFiltered": get_fsr_filtered(),
            "riskScore": state["riskScore"],
            "riskCategory": state["riskCategory"],
            "report": state["report"],
            "sessionTime": int(time.time() - state["sessionStart"]),
        }
    return jsonify(payload)


@app.route("/reset", methods=["GET", "POST"])
def reset():
    """Starts a new session. Optionally accepts {"name": ..., "age": ...} in the
    JSON body — sent by the dashboard's 'Start Monitoring' screen."""
    global state
    body = request.get_json(force=True, silent=True) or {}
    name = str(body.get("name", "")).strip()
    age = body.get("age")
    with lock:
        state = fresh_state(name=name, age=age)
    return jsonify({"status": "reset"})


@app.route("/")
def index():
    return send_from_directory(app.static_folder, "dashboard.html")


if __name__ == "__main__":
    # threaded=True so the ESP32's POSTs and the browser's GETs don't block each other
    app.run(host="0.0.0.0", port=5000, threaded=True)
