"""Podglad panelu SIH Radar bez modulu.

Serwuje webui/index.html i udaje API modulu (ruchome cele radaru, odczyty
BME688, skanowanie i laczenie z Wi-Fi). WebSocket nie jest udawany - panel
przechodzi wtedy na odpytywanie /api/sensors, co tez sprawdza ta sciezke.

    python tools/mock_server.py [port] [--ap]

--ap: modul w trybie punktu dostepowego (pierwsze uruchomienie).
"""
import json
import math
import random
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
T0 = time.time()
STATE = {"mode": "ap" if "--ap" in sys.argv else "sta", "ssid": "" if "--ap" in sys.argv else "Dom",
         "test": "idle", "test_t": 0.0, "test_ssid": "", "test_pass": "", "upd_auto": True, "ota_t": 0.0}

NETS = [
    {"ssid": "Dom", "rssi": -48, "auth": 3, "ch": 6},
    {"ssid": "Dom_IoT", "rssi": -61, "auth": 3, "ch": 6},
    {"ssid": "Sasiad 2.4G", "rssi": -74, "auth": 4, "ch": 11},
    {"ssid": "Kawiarnia", "rssi": -82, "auth": 0, "ch": 1},
]


def sensors():
    t = time.time() - T0
    targets = []
    # cel 1: chodzi po luku, cel 2: siedzi (lekki szum), cel 3 pojawia sie co jakis czas
    a = math.sin(t / 4) * 0.8
    targets.append({"id": 1, "x": int(3000 * math.sin(a)), "y": int(3000 * math.cos(a) + 300 * math.sin(t / 1.7)),
                    "v": int(-40 * math.cos(t / 4)), "res": 320})
    targets.append({"id": 2, "x": int(-1200 + random.randint(-30, 30)), "y": int(1800 + random.randint(-30, 30)),
                    "v": 0, "res": 320})
    if int(t / 10) % 2:
        targets.append({"id": 3, "x": int(1500 - (t % 10) * 250), "y": 4800, "v": 30, "res": 360})
    return {
        "up": int(t * 1000),
        "radar": {"online": True, "fw": "V2.04.23101915", "fps": 10, "count": len(targets),
                  "frames": int(t * 10), "errors": 3, "targets": targets},
        "bme": {"online": True, "chip": "BME688", "addr": 119, "t": 22.4 + math.sin(t / 60) * 0.3,
                "h": 46.2, "p": 1012.8, "gas": 84.3 + t / 30, "gas_ok": t > 6, "n": int(t / 3)},
    }


def status():
    ap = STATE["mode"] == "ap"
    return {
        "fw": "v0.1.0-beta1", "build": "Nov 16 2026 10:00:00", "idf": "v5.2.1",
        "uptime": int(time.time() - T0) + 3725, "part": "ota_0",
        "heap": 214532, "heap_min": 198004, "mac": "40:4C:CA:12:1A:2B", "chip_temp": 41.6,
        "wifi": {"mode": STATE["mode"], "ssid": STATE["ssid"], "ip": "192.168.4.1" if ap else "192.168.1.57",
                 "rssi": 0 if ap else -52, "ch": 0 if ap else 6, "host": "sih-radar-1a2b",
                 "ap_ssid": "SIH-Radar-1A2B", "ap_clients": 1 if ap else 0},
        "upd": {"auto": STATE["upd_auto"], "busy": False, "state": "none", "err": False, "latest": "", "new": False},
    }


def ota_status():
    # Udawane pobieranie: 8 s od klikniecia, potem sukces
    if not STATE["ota_t"]:
        return {"state": "idle", "progress": 0, "error": ""}
    p = int((time.time() - STATE["ota_t"]) / 8 * 100)
    if p >= 100:
        STATE["ota_t"] = 0
        return {"state": "success", "progress": 100, "error": ""}
    return {"state": "downloading" if p < 5 else "writing", "progress": max(p, 0), "error": ""}


def wifi_test():
    s = STATE
    if s["test"] == "running" and time.time() - s["test_t"] > 4:
        s["test"] = "ok" if s["test_pass"] == "haslo1234" or s["test_ssid"] == "Kawiarnia" else "fail"
    return {"state": s["test"], "ssid": s["test_ssid"], "ip": "192.168.1.57" if s["test"] == "ok" else "",
            "reason": 15 if s["test"] == "fail" else 0, "try": min(3, 1 + int((time.time() - s["test_t"]) / 1.5)),
            "host": "sih-radar-1a2b", "restart_s": 20}


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, code, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        p = self.path.split("?")[0]
        if p == "/":
            return self.send(200, (ROOT / "webui" / "index.html").read_bytes(), "text/html; charset=utf-8")
        if p == "/api/status":
            return self.send(200, status())
        if p == "/api/sensors":
            return self.send(200, sensors())
        if p == "/api/wifi/scan":
            time.sleep(1.2)
            return self.send(200, NETS)
        if p == "/api/wifi/test":
            return self.send(200, wifi_test())
        if p == "/api/ota/status":
            return self.send(200, ota_status())
        self.send(404, {"ok": False, "error": "Nie ma takiej strony"})

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n)
        p = self.path.split("?")[0]
        if p == "/api/ota/upload":
            ok = raw[:1] == b"\xe9"   # magic obrazu ESP
            return self.send(200 if ok else 400, {"ok": True} if ok else {"ok": False, "error": "To nie jest plik firmware"})
        if p in ("/api/ota/github", "/api/ota/url"):
            if STATE["mode"] != "sta":
                return self.send(409, {"ok": False, "error": "Moduł nie ma dostępu do internetu - wgraj plik .bin"})
            STATE["ota_t"] = time.time()
            return self.send(200, {"ok": True})
        body = json.loads(raw or b"{}")
        if p == "/api/update":
            if "auto" in body:
                STATE["upd_auto"] = bool(body["auto"])
            return self.send(200, {"ok": True})
        if self.path == "/api/wifi/connect":
            if not body.get("ssid"):
                return self.send(400, {"ok": False, "error": "Podaj nazwę sieci."})
            if STATE["mode"] != "ap":
                STATE["ssid"] = body["ssid"]
                return self.send(200, {"ok": True, "test": False})
            STATE.update(test="running", test_t=time.time(), test_ssid=body["ssid"],
                         test_pass=body.get("password", ""))
            return self.send(200, {"ok": True, "test": True})
        if self.path == "/api/reboot":
            return self.send(200, {"ok": True})
        self.send(404, {"ok": False, "error": "Nie ma takiej strony"})


if __name__ == "__main__":
    port = next((int(a) for a in sys.argv[1:] if a.isdigit()), 8718)
    print(f"SIH Radar - podglad panelu: http://localhost:{port}/ (tryb {STATE['mode']})")
    ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
