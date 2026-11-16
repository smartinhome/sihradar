"""Podglad panelu SIH Radar bez modulu.

Serwuje webui/index.html i udaje API modulu (ruchome cele radaru, odczyty
BME688, skanowanie i laczenie z Wi-Fi). WebSocket nie jest udawany - panel
przechodzi wtedy na odpytywanie /api/sensors, co tez sprawdza ta sciezke.

    python tools/mock_server.py [port] [--ap]

--ap: modul w trybie punktu dostepowego (pierwsze uruchomienie).

Obecnosc i strefy liczone uproszczonie (bez pamieci osoby nieruchomej) -
pelna logika jest w firmware/components/presence i w tools/test_host.c.
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
         "test": "idle", "test_t": 0.0, "test_ssid": "", "test_pass": "", "upd_auto": True, "ota_t": 0.0,
         "room": {"set": False, "w": 4000, "l": 4000, "x": 2000, "y": 0, "rot": 0, "flip": False},
         "pz": {"cfg": {"dist": 6000, "hold": 30, "zoff": 5, "latch": 30, "exit": 400, "mem": True, "room": True},
                "zones": [{"name": "Kanapa", "t": 1, "x1": 2600, "y1": 1200, "x2": 3800, "y2": 2400},
                          {"name": "Wentylator", "t": 2, "x1": 0, "y1": 3200, "x2": 800, "y2": 4000}]},
         "pz_last": {}}

NETS = [
    {"ssid": "Dom", "rssi": -48, "auth": 3, "ch": 6},
    {"ssid": "Dom_IoT", "rssi": -61, "auth": 3, "ch": 6},
    {"ssid": "Sasiad 2.4G", "rssi": -74, "auth": 4, "ch": 11},
    {"ssid": "Kawiarnia", "rssi": -82, "auth": 0, "ch": 1},
]


def to_room(r, tx, ty):
    t = math.radians(r["rot"])
    X = -tx if r["flip"] else tx
    return (r["x"] + X * -math.cos(t) + ty * -math.sin(t), r["y"] + X * -math.sin(t) + ty * math.cos(t))


def inside(z, x, y):
    return min(z["x1"], z["x2"]) <= x <= max(z["x1"], z["x2"]) and min(z["y1"], z["y2"]) <= y <= max(z["y1"], z["y2"])


def presence(targets):
    r, cfg, zones, last = STATE["room"], STATE["pz"]["cfg"], STATE["pz"]["zones"], STATE["pz_last"]
    now = time.time()
    ts, tz, n, zn = [0, 0, 0], [0, 0, 0], 0, [0] * len(zones)
    for t in targets:
        i = t["id"] - 1
        x, y = to_room(r, t["x"], t["y"])
        if math.hypot(t["x"], t["y"]) > cfg["dist"]:
            st = 2
        elif cfg["room"] and r["set"] and not (-150 <= x <= r["w"] + 150 and -150 <= y <= r["l"] + 150):
            st = 3
        elif any(z["t"] == 2 and inside(z, x, y) for z in zones):
            st = 4
        else:
            st = 1
        ts[i] = st
        if st == 1:
            n += 1
            for k, z in enumerate(zones):
                if z["t"] == 1 and inside(z, x, y):
                    tz[i] |= 1 << k
                    zn[k] += 1
    if n:
        last["room"] = now
    zl = []
    for k, z in enumerate(zones):
        if zn[k]:
            last[k] = now
        zl.append([int(z["t"] == 1 and now - last.get(k, 0) < cfg["zoff"]), 0, zn[k]])
    return {"occ": now - last.get("room", 0) < cfg["hold"], "latch": False, "n": n, "held": 0,
            "z": zl, "ts": ts, "tz": tz}


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
        "pres": presence(targets),
        "up": int(t * 1000),
        "radar": {"online": True, "fw": "V2.04.23101915", "fps": 10, "count": len(targets),
                  "frames": int(t * 10), "errors": 3, "baud": 256000, "scan": False, "rx": int(t * 300),
                  "raw": "AA FF 03 00 0E 03 B1 86 10 00 40 01 00 00 00 00 00 00 00 00 00 00 00 00",
                  "targets": targets},
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
        if p == "/api/room":
            return self.send(200, STATE["room"])
        if p == "/api/zones":
            return self.send(200, STATE["pz"])
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
        if p == "/api/room":
            r = {k: int(body[k]) for k in ("w", "l", "x", "y", "rot")}
            if not (500 <= r["w"] <= 20000 and 500 <= r["l"] <= 20000):
                return self.send(400, {"ok": False, "error": "Wymiary pokoju od 0,5 do 20 m."})
            if not (0 <= r["x"] <= r["w"] and 0 <= r["y"] <= r["l"]):
                return self.send(400, {"ok": False, "error": "Radar musi być w obrębie pokoju."})
            STATE["room"] = dict(r, set=True, flip=bool(body.get("flip")))
            return self.send(200, STATE["room"])
        if p == "/api/zones":
            c = dict(STATE["pz"]["cfg"], **body.get("cfg", {}))
            if not 500 <= c["dist"] <= 7500:
                return self.send(400, {"ok": False, "error": "Zasięg od 0,5 do 7,5 m."})
            zs = body.get("zones", [])
            if len(zs) > 8:
                return self.send(400, {"ok": False, "error": "Najwyżej 8 stref."})
            for z in zs:
                if abs(z["x2"] - z["x1"]) < 200 or abs(z["y2"] - z["y1"]) < 200:
                    return self.send(400, {"ok": False, "error": "Strefa musi mieć co najmniej 20 × 20 cm."})
            STATE["pz"] = {"cfg": c, "zones": [dict(z, name=z.get("name", "")[:23]) for z in zs]}
            STATE["pz_last"] = {}
            return self.send(200, STATE["pz"])
        if p == "/api/radar":
            if body.get("action") not in ("restart", "factory"):
                return self.send(400, {"ok": False, "error": "Nieznane polecenie"})
            return self.send(200, {"ok": True})
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
