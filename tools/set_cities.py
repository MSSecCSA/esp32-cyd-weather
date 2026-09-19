#!/usr/bin/env python3
"""Pick the cities for the weather station, then flash it.

Prompts for a city, a US ZIP, or a "city, country", checks each one actually has
weather data, works out the POSIX timezone string the firmware needs, rewrites the
cities[] array in src/main.cpp, and offers to upload.

    python3 tools/set_cities.py [--port /dev/cu.usbserial-110] [--no-flash]

Why this exists: the firmware needs POSIX TZ strings ("EST5EDT,M3.2.0,M11.1.0"), but
every geocoder on earth returns IANA names ("America/New_York"). Getting from one to the
other by hand is tedious and easy to get subtly wrong -- and a wrong DST rule shows a
plausible but incorrect clock rather than failing loudly.

Stdlib only. No pip install.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.parse
import urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAIN = os.path.join(REPO, "src", "main.cpp")
BEGIN = "// === CITIES:BEGIN"
END = "// === CITIES:END ==="

# Matches TZ_PADDED_LEN in src/main.cpp. A longer string is silently truncated by
# setTimezone(), and a truncated POSIX rule still parses -- just to a *different* rule.
TZ_MAX = 31
# The city name is drawn in font 2 into a 191px column. Measured on hardware:
# "Arusha, Tanzania" (16 chars) renders 104px, so ~6.5px per character.
NAME_SOFT_MAX = 26

GEOCODE = "https://geocoding-api.open-meteo.com/v1/search"
FORECAST = "https://api.open-meteo.com/v1/forecast"
ZIPPO = "https://api.zippopotam.us"


def get_json(url, params=None, timeout=15):
    if params:
        url = f"{url}?{urllib.parse.urlencode(params)}"
    req = urllib.request.Request(url, headers={"User-Agent": "cyd-weather-setup"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)


def posix_tz(iana):
    """POSIX TZ string for an IANA zone, read from the TZif file's own footer.

    Every TZif v2+ file ends with the POSIX rule as its final line -- the same string
    the C library would use. Reading it beats maintaining a mapping table, and it comes
    from the same tzdata the host already trusts.
    """
    for root in ("/usr/share/zoneinfo", "/var/db/timezone/zoneinfo",
                 "/usr/lib/zoneinfo", "/etc/zoneinfo"):
        path = os.path.join(root, iana)
        if os.path.isfile(path):
            with open(path, "rb") as f:
                data = f.read()
            if not data.startswith(b"TZif"):
                continue
            tail = data.rstrip(b"\n").split(b"\n")[-1]
            try:
                s = tail.decode("ascii")
            except UnicodeDecodeError:
                continue
            # A v1-only file has no footer; the last "line" is then binary noise.
            if s and all(32 <= ord(c) < 127 for c in s):
                return s
    return None


def check_weather(lat, lon):
    """Confirm Open-Meteo actually serves this point. Returns (iana_tz, summary)."""
    try:
        d = get_json(FORECAST, {
            "latitude": f"{lat:.4f}", "longitude": f"{lon:.4f}",
            "current": "temperature_2m", "timezone": "auto",
            "temperature_unit": "fahrenheit",
        })
        t = d["current"]["temperature_2m"]
        return d["timezone"], f"{t:.0f}F right now"
    except Exception as e:
        return None, f"no data ({e})"


def search(query, count=8):
    try:
        d = get_json(GEOCODE, {"name": query, "count": count,
                               "language": "en", "format": "json"})
        return d.get("results", []) or []
    except Exception as e:
        print(f"    ! geocoder error: {e}")
        return []


def zip_lookup(code, country="us"):
    try:
        d = get_json(f"{ZIPPO}/{country}/{code}")
        p = d["places"][0]
        return {
            "name": p["place name"],
            "admin1": p.get("state", ""),
            "admin1_code": p.get("state abbreviation", ""),
            "country_code": d.get("country abbreviation", country.upper()),
            "latitude": float(p["latitude"]),
            "longitude": float(p["longitude"]),
        }
    except Exception:
        return None


def label(r):
    bits = [r["name"]]
    if r.get("admin1"):
        bits.append(r["admin1"])
    if r.get("country_code"):
        bits.append(r["country_code"])
    return ", ".join(bits)


def default_display_name(r):
    """US places get 'City, ST'; everywhere else 'City, Country'."""
    if r.get("country_code") == "US":
        st = r.get("admin1_code") or r.get("admin1", "")
        return f"{r['name']}, {st}" if st else r["name"]
    cc = r.get("country_code", "")
    return f"{r['name']}, {cc}" if cc else r["name"]


def ask(prompt, default=None):
    suffix = f" [{default}]" if default else ""
    try:
        v = input(f"{prompt}{suffix}: ").strip()
    except (EOFError, KeyboardInterrupt):
        print()
        sys.exit(1)
    return v or (default or "")


def choose(results):
    """Show matches and let the user pick one, or reject them all."""
    print()
    for i, r in enumerate(results, 1):
        print(f"    {i}. {label(r):<44} {r['latitude']:>8.3f},{r['longitude']:>9.3f}")
    print("    n. none of these")
    while True:
        c = ask("  pick", "1")
        if c.lower() == "n":
            return None
        if c.isdigit() and 1 <= int(c) <= len(results):
            return results[int(c) - 1]
        print("    ? enter a number, or n")


def resolve_one():
    """Resolve one location to a firmware entry, or None to stop."""
    raw = ask("\n  City, US ZIP, or 'City, Country'  (blank when finished)")
    if not raw:
        return None

    hit = None
    if re.fullmatch(r"\d{5}", raw):
        print(f"    looking up ZIP {raw}...")
        hit = zip_lookup(raw)
        if hit:
            print(f"    -> {label(hit)}")
        else:
            print("    ! that ZIP did not resolve.")

    if hit is None:
        results = search(raw)
        if not results:
            # This is the "ask for a nearby major city" path.
            print(f"    ! nothing found for '{raw}'.")
            alt = ask("    name a nearby major city instead (blank to skip)")
            if not alt:
                return "skip"
            results = search(alt)
            if not results:
                print(f"    ! '{alt}' did not resolve either. Skipping.")
                return "skip"
        hit = choose(results)
        if hit is None:
            alt = ask("    name a nearby major city instead (blank to skip)")
            if not alt:
                return "skip"
            results = search(alt)
            if not results:
                print("    ! no match. Skipping.")
                return "skip"
            hit = choose(results)
            if hit is None:
                return "skip"

    lat, lon = hit["latitude"], hit["longitude"]

    # Validate that the forecast API really serves this point, and take its timezone --
    # the same timezone=auto the firmware asks for, so the two agree by construction.
    print("    checking weather availability...")
    iana, note = check_weather(lat, lon)
    if not iana:
        print(f"    ! {note} -- skipping.")
        return "skip"
    print(f"    ok: {note}, timezone {iana}")

    tz = posix_tz(iana)
    if not tz:
        print(f"    ! no POSIX rule found for {iana} in the system tz database.")
        tz = ask("    enter one by hand (e.g. EST5EDT,M3.2.0,M11.1.0)")
        if not tz:
            return "skip"
    if len(tz) > TZ_MAX:
        print(f"    ! POSIX string is {len(tz)} chars, over the {TZ_MAX} limit: {tz}")
        tz = ask("    shorten it by hand")
        if not tz or len(tz) > TZ_MAX:
            print("    ! still too long. Skipping.")
            return "skip"
    print(f"    tz: {tz}  ({len(tz)}/{TZ_MAX} chars)")

    name = ask("    display name", default_display_name(hit))
    while len(name) > NAME_SOFT_MAX:
        print(f"    ! {len(name)} chars may overflow the {NAME_SOFT_MAX}-char column.")
        shorter = ask(f"    shorten it (blank to keep '{name}' anyway)")
        if not shorter:
            break
        name = shorter

    return {"lat": lat, "lon": lon, "name": name, "tz": tz}


def render(cities):
    lines = [
        "// Edit this list to taste -- it is the single source of truth for which cities",
        "// are fetched and which local times are shown. `tz` is a POSIX TZ string, NOT an",
        "// IANA name (\"America/New_York\" will not work here); every one must be",
        "// <= TZ_PADDED_LEN characters, which checkTimezoneLengths() verifies at boot.",
        "// Generated by tools/set_cities.py -- rerun it, or edit by hand.",
        "City cities[] = {",
    ]
    w = max(len(c["name"]) for c in cities) + 2
    for i, c in enumerate(cities):
        # comma goes INSIDE the padding, or the fields run together and C++ silently
        # concatenates the two adjacent string literals into one
        nm = ('"' + c["name"].replace('\\', '\\\\').replace('"', '\\"') + '",').ljust(w + 1)
        comma = "," if i < len(cities) - 1 else ""
        lines.append(f'  {{{c["lat"]:9.4f}, {c["lon"]:10.4f}, {nm} "{c["tz"]}"}}{comma}')
    lines.append("};")
    return "\n".join(lines) + "\n"


def current_cities(src):
    inner = src[src.index(BEGIN):src.index(END)]
    return re.findall(r'\{\s*-?[\d.]+,\s*-?[\d.]+,\s*"([^"]+)"', inner)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="serial port, e.g. /dev/cu.usbserial-110")
    ap.add_argument("--no-flash", action="store_true", help="write the file, do not upload")
    args = ap.parse_args()

    src = open(MAIN).read()
    if BEGIN not in src or END not in src:
        sys.exit(f"markers not found in {MAIN} -- is this the right checkout?")

    print("=" * 66)
    print("  CYD Weather Station -- city setup")
    print("=" * 66)
    print("\n  currently on the board:")
    for n in current_cities(src):
        print(f"    - {n}")
    print("\n  Add the cities you want. These REPLACE the list above.")

    cities = []
    while True:
        r = resolve_one()
        if r is None:
            break
        if r == "skip":
            continue
        cities.append(r)
        print(f"    added: {r['name']}   ({len(cities)} so far)")

    if not cities:
        print("\n  nothing added; leaving src/main.cpp untouched.")
        return

    print("\n" + "=" * 66)
    for i, c in enumerate(cities, 1):
        print(f"  {i}. {c['name']:<26} {c['lat']:>9.4f},{c['lon']:>10.4f}  {c['tz']}")
    print("=" * 66)
    if ask("\n  write these to src/main.cpp? (y/n)", "y").lower() not in ("y", "yes"):
        print("  aborted, nothing written.")
        return

    backup = MAIN + ".bak"
    shutil.copy2(MAIN, backup)
    head = src[:src.index(BEGIN)]
    tail = src[src.index(END):]
    marker = src[src.index(BEGIN):src.index("\n", src.index(BEGIN)) + 1]
    open(MAIN, "w").write(head + marker + render(cities) + tail)
    print(f"  written. previous version saved as {os.path.basename(backup)}")

    if args.no_flash:
        print("\n  --no-flash given; build with:  pio run --target upload")
        return
    if ask("\n  build and upload to the board now? (y/n)", "y").lower() not in ("y", "yes"):
        print("  skipped. Upload later with:  pio run --target upload")
        return

    cmd = [sys.executable, "-m", "platformio", "run", "--target", "upload"]
    if args.port:
        cmd += ["--upload-port", args.port]
    print(f"\n  $ {' '.join(cmd)}\n")
    rc = subprocess.call(cmd, cwd=REPO)
    if rc == 0:
        print("\n  done. Watch it with:  pio device monitor")
        print("  The boot log reports whether any name or TZ string overflows its limit.")
    else:
        print(f"\n  upload failed (exit {rc}). src/main.cpp is still updated;")
        print(f"  restore it with:  mv {os.path.basename(backup)} main.cpp")
        sys.exit(rc)


if __name__ == "__main__":
    main()
