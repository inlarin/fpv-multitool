"""HMAC-SHA1 unseal trials on PTL 2021 packs.

Try a list of candidate 32-byte keys via /api/batt/mavic3/unseal_hmac.
Respects host-side 4-attempts/60s rate limit by waiting between tries.

Output: results printed line-by-line; appended to SESSION_PROTOCOL.md.
"""
import json
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IP = (ROOT / "scripts" / ".board_ip").read_text(encoding="utf-8").strip()

CANDIDATES = [
    # TI default 16-byte HMAC key, expanded various ways:
    ("TI default repeat",
     "0123456789ABCDEFFEDCBA9876543210" * 2),
    ("TI default + reverse",
     "0123456789ABCDEFFEDCBA9876543210" + "0123456789ABCDEFFEDCBA9876543210"[::-1]),
    ("TI default + zeros",
     "0123456789ABCDEFFEDCBA9876543210" + "0" * 32),
    ("TI default reversed twice",
     "0123456789ABCDEFFEDCBA9876543210"[::-1] * 2),
    # All-zero / all-FF:
    ("all zeros", "00" * 32),
    ("all FF",    "FF" * 32),
    # Common "factory reset" / weak keys:
    ("DJI ascii 'DJI Battery 1234567890ABCDEF'",
     "444A4920426174746572792031323334353637383930414243444546" + "00" * 4),
    ("MD5 IV expanded (matches auth-bypass magic 0x67452301)",
     "0123456789ABCDEFFEDCBA9876543210FEDCBA98765432100123456789ABCDEF"),
]


def try_key(name: str, hex_key: str) -> dict:
    if len(hex_key) != 64:
        return {"error": f"key length {len(hex_key)} != 64", "name": name}
    url = f"http://{IP}/api/batt/mavic3/unseal_hmac?key={hex_key}"
    req = urllib.request.Request(url, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            data = json.loads(r.read().decode())
            data["name"] = name
            return data
    except urllib.error.HTTPError as e:
        try:
            data = json.loads(e.read().decode())
            data["name"] = name
            data["http_code"] = e.code
            return data
        except Exception:
            return {"error": str(e), "name": name, "http_code": getattr(e, "code", "?")}
    except Exception as e:
        return {"error": str(e), "name": name}


def cooldown_wait():
    url = f"http://{IP}/api/batt/snapshot"
    while True:
        try:
            with urllib.request.urlopen(url, timeout=5) as r:
                d = json.loads(r.read().decode())
                cd = d.get("unsealCooldownMs", 0)
                if cd == 0:
                    return
                wait = cd // 1000 + 2
                print(f"  ... waiting {wait}s for host-side cooldown ({cd}ms)")
                time.sleep(min(wait, 35))
        except Exception:
            time.sleep(5)


def main() -> None:
    print(f"Target: {IP}")
    print(f"Candidates: {len(CANDIDATES)}")
    print()
    results = []
    for i, (name, key) in enumerate(CANDIDATES):
        # Pace: ensure cooldown clear before each attempt
        if i > 0:
            cooldown_wait()
        print(f"--- {i+1}/{len(CANDIDATES)}: {name}")
        print(f"    key32 = {key[:16]}...{key[-16:]}")
        r = try_key(name, key)
        results.append(r)
        if r.get("result") == "OK":
            print(f"    ✓ SUCCESS! sec={r.get('sec')} opStatus={r.get('operationStatus')}")
            print(f"    Working key: {key}")
            break
        challenge = r.get("challenge", "")
        op = r.get("operationStatus", "?")
        sec = r.get("sec", "?")
        result = r.get("result", r.get("error", "?"))
        print(f"    result={result}  sec={sec}  opStatus={op}")
        if challenge:
            print(f"    BMS challenge: {challenge}")

    print("\n=== SUMMARY ===")
    for r in results:
        print(f"  {r.get('name', '?')}: {r.get('result', r.get('error', '?'))}")


if __name__ == "__main__":
    main()
