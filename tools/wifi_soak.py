#!/usr/bin/env python3
"""Soak test for the CYD weather station with WiFi actually associated.

The zero-leak result recorded in CLAUDE.md was measured with the radio OFF, so the
fetch path (HTTPClient + the one Arduino String in fetchWeather) was never exercised.
This script is the confirmation run: it watches the serial log with WiFi up and reports
whether the heap is still flat, whether fetches succeed, and whether NTP ever synced.

Usage:
    python3 tools/wifi_soak.py [minutes] [--port /dev/cu.usbserial-110]

Recommended: 20+ minutes. At the old leak rate (~18 B/s) a 20 minute run would have
lost ~21KB, so anything near flat over that window is a clear result.

Log strings matched here are copied verbatim from src/main.cpp -- if you change a
Serial.print there, update the patterns below.
"""
import re, sys, time, collections

try:
    import serial
except ImportError:
    sys.exit("pyserial not installed: python3 -m pip install pyserial")

minutes = 20.0
port = "/dev/cu.usbserial-110"
args = sys.argv[1:]
for i, a in enumerate(args):
    if a == "--port" and i + 1 < len(args):
        port = args[i + 1]
    elif re.fullmatch(r"[\d.]+", a):
        minutes = float(a)

DUR = minutes * 60.0
PAT = {
    "heap":      re.compile(r"Heap: free=(\d+) largest_free_block=(\d+)"),
    "ok":        re.compile(r"OK: (.+?) (.+?) \[(.+?)\]"),
    "fetching":  re.compile(r"Fetching \[(.+?)\]"),
    "switch":    re.compile(r"Auto-switch -> (.+)$"),
    "httperr":   re.compile(r"HTTP error: (-?\d+)"),
    "jsonerr":   re.compile(r"JSON parse error: (.+)$"),
    "nowifi":    re.compile(r"Skipping fetch, WiFi not connected"),
    "wifiup":    re.compile(r"WiFi connected! IP: (\S+)"),
    "wifidrop":  re.compile(r"WiFi dropped"),
    "wifiback":  re.compile(r"WiFi reconnected"),
    "lowheap":   re.compile(r"Free heap critically low"),
    "boot":      re.compile(r"=== CYD Weather Station"),
    "tzguard":   re.compile(r"\*\*\* TZ TOO LONG"),
}

s = serial.Serial(port, 115200, timeout=1)
counts = collections.Counter()
heap, cities_ok, errors, ip = [], collections.Counter(), [], None
t0 = time.time()
print(f"soaking {minutes:.1f} min on {port} -- Ctrl-C to stop early\n")

try:
    while time.time() - t0 < DUR:
        line = s.readline().decode("utf-8", "replace").rstrip()
        if not line:
            continue
        t = time.time() - t0
        for name, pat in PAT.items():
            m = pat.search(line)
            if not m:
                continue
            counts[name] += 1
            if name == "heap":
                heap.append((t, int(m.group(1))))
            elif name == "ok":
                cities_ok[m.group(3)] += 1
                print(f"  {t:7.1f}s  OK  {m.group(3)}: {m.group(1)} {m.group(2)}", flush=True)
            elif name == "wifiup":
                ip = m.group(1)
                print(f"  {t:7.1f}s  WiFi up, IP {ip}", flush=True)
            elif name in ("httperr", "jsonerr"):
                errors.append((t, line))
                print(f"  {t:7.1f}s  !! {line}", flush=True)
            elif name in ("boot", "lowheap", "wifidrop", "wifiback", "tzguard"):
                print(f"  {t:7.1f}s  ** {line}", flush=True)
except KeyboardInterrupt:
    print("\ninterrupted -- reporting what we have")
finally:
    s.close()

el = time.time() - t0
print(f"\n{'='*58}\nRESULT after {el/60:.1f} min\n{'='*58}")
print(f"WiFi IP              : {ip or 'NEVER ASSOCIATED'}")
print(f"successful fetches   : {counts['ok']}  across {len(cities_ok)} cities {dict(cities_ok)}")
print(f"fetch attempts       : {counts['fetching']}   skipped (no WiFi): {counts['nowifi']}")
print(f"HTTP / JSON errors   : {counts['httperr']} / {counts['jsonerr']}")
print(f"WiFi drops / rejoins : {counts['wifidrop']} / {counts['wifiback']}")
print(f"reboots (banner)     : {max(0, counts['boot'])}   low-heap reboots: {counts['lowheap']}")
if counts["tzguard"]:
    print("*** TZ LENGTH GUARD FIRED -- a city's TZ string exceeds TZ_PADDED_LEN")

if len(heap) >= 2:
    (t1, f1), (t2, f2) = heap[0], heap[-1]
    dt, df = t2 - t1, f1 - f2
    lo = min(h for _, h in heap); hi = max(h for _, h in heap)
    print(f"\nheap samples         : {len(heap)}")
    print(f"first / last free    : {f1} / {f2}")
    print(f"drift                : {df:+d} bytes over {dt:.0f}s  =>  {df/dt if dt else 0:+.2f} B/s")
    print(f"min / max / spread   : {lo} / {hi} / {hi-lo}")
    rate = df / dt if dt else 0
    # A flat heap proves nothing if the path under test never ran. The whole point of
    # this script is to exercise the fetch path, so refuse to give a verdict without it.
    if ip is None or counts["ok"] == 0:
        print("\nVERDICT: INCONCLUSIVE -- heap was flat, but WiFi never associated"
              " and/or no fetch succeeded, so the HTTP/JSON path was never exercised."
              " This run does NOT confirm the fix with the radio up.")
    elif abs(rate) < 1.0:
        print(f"\nVERDICT: FLAT with the radio up and {counts['ok']} successful fetches"
              " -- the fix holds end to end.")
    else:
        print(f"\nVERDICT: STILL DRAINING at {rate:+.2f} B/s across {counts['ok']}"
              " fetches -- investigate the fetch path (the String in getString(),"
              " HTTPClient buffers, or lwIP holding sockets).")
else:
    print("\nnot enough heap samples -- was the board running?")
