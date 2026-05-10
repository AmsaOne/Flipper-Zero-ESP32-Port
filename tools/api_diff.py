#!/usr/bin/env python3
"""
Diff upstream Flipper Zero api_symbols.csv against the port's firmware_api.c.

Classifies symbols missing from the port into:
  - tier1.txt: name appears as a definition in port sources (likely just needs hashtable entry)
  - tier3.txt: known hardware-absent prefixes (ibutton, rfid emulate, hid u2f, vibro)
  - tier2.txt: everything else (needs implementation, translation, or libc-resolved at link)

Usage:
    python3 tools/api_diff.py            # write tier{1,2,3}.txt under tools/ + summary
    python3 tools/api_diff.py --report   # summary only
"""
import argparse
import csv
import os
import re
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parent
PORT_API = ROOT / "components" / "flipper_application" / "flipper_application" / "firmware_api.c"
UPSTREAM_CSV = ROOT / "targets" / "f7" / "api_symbols.csv"

SCAN_ROOTS = [ROOT / "components", ROOT / "applications", ROOT / "applications_user"]

TIER3_PREFIXES = (
    "furi_hal_ibutton_",
    "furi_hal_rfid_comp_",
    "furi_hal_rfid_tim_emulate",
    "furi_hal_hid_u2f_",
    "LL_",
    "HAL_",
    "stm32wb",
    "ble_glue_",
    "hci_",
    "SHCI_",
    "shci_",
)

# libc / newlib / compiler-builtin names resolved at link by ESP-IDF
# These are in upstream's CSV but not part of the Flipper API surface per se.
LIBC_PREFIXES = (
    "__", "abs", "atof", "atoi", "atol", "bsearch", "calloc", "free", "malloc",
    "memchr", "memcmp", "memcpy", "memmove", "memset", "qsort", "rand",
    "snprintf", "sprintf", "sscanf", "strcasecmp", "strcat", "strchr", "strcmp",
    "strcpy", "strcspn", "strdup", "strerror", "strlcat", "strlcpy", "strlen",
    "strncasecmp", "strncat", "strncmp", "strncpy", "strpbrk", "strrchr",
    "strspn", "strstr", "strtod", "strtof", "strtok", "strtok_r", "strtol",
    "strtoul", "strtoull", "vprintf", "vsnprintf", "vsprintf",
)


def parse_port_table(path):
    names = set()
    pat = re.compile(r'\.hash\s*=\s*0x[0-9a-fA-F]+.*?/\*\s*(\S+)\s*\*/')
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = pat.search(line)
            if m:
                names.add(m.group(1))
    return names


def parse_upstream_csv(path):
    names = set()
    with open(path, newline='', encoding='utf-8') as f:
        r = csv.reader(f)
        next(r, None)
        for row in r:
            if len(row) >= 3 and row[0] == "Function" and row[1] == "+":
                names.add(row[2])
    return names


def is_libc(name):
    return any(name.startswith(p) for p in LIBC_PREFIXES)


def build_defined_set(roots):
    """Set of identifiers appearing in <ident>( position across .c/.h files."""
    found = set()
    pat = re.compile(r'\b([a-zA-Z_][a-zA-Z0-9_]*)\s*\(')
    for root in roots:
        if not root.exists():
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames
                           if not d.startswith(("build", "managed_components", ".", "cmake"))]
            for fn in filenames:
                if not fn.endswith((".c", ".h", ".cpp", ".hpp")):
                    continue
                try:
                    with open(os.path.join(dirpath, fn), encoding='utf-8', errors='replace') as f:
                        for line in f:
                            for m in pat.finditer(line):
                                found.add(m.group(1))
                except OSError:
                    pass
    return found


def classify(missing, defined):
    tier1, tier2, tier3, libc = [], [], [], []
    for name in sorted(missing):
        if is_libc(name):
            libc.append(name)
        elif any(name.startswith(p) for p in TIER3_PREFIXES):
            tier3.append(name)
        elif name in defined:
            tier1.append(name)
        else:
            tier2.append(name)
    return tier1, tier2, tier3, libc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--report", action="store_true",
                    help="Print summary only, don't write tier files")
    args = ap.parse_args()

    if not UPSTREAM_CSV.exists():
        print(f"ERROR: missing {UPSTREAM_CSV}", file=sys.stderr)
        return 1

    upstream = parse_upstream_csv(UPSTREAM_CSV)
    port = parse_port_table(PORT_API)
    overlap = port & upstream
    missing = upstream - port
    extra = port - upstream

    print(f"Upstream symbols (Function,+): {len(upstream)}")
    print(f"Port symbols (firmware_api.c): {len(port)}")
    print(f"  ...present in upstream:      {len(overlap)}")
    print(f"  ...port-only (custom adds):  {len(extra)}")
    print(f"Missing from port:             {len(missing)}")
    print(f"Coverage of upstream surface:  {len(overlap) / len(upstream) * 100:.1f}%")

    if args.report:
        return 0

    print("\nScanning sources for existing definitions...")
    defined = build_defined_set(SCAN_ROOTS)
    tier1, tier2, tier3, libc = classify(missing, defined)

    for tag, lst in [("tier1", tier1), ("tier2", tier2), ("tier3", tier3), ("libc", libc)]:
        out = SCRIPT_DIR / f"{tag}.txt"
        with open(out, "w", encoding='utf-8') as f:
            for n in lst:
                f.write(n + "\n")
        print(f"  {out.name}: {len(lst)} symbols")

    print(f"\nTier 1 (driver exists, expose only):  {len(tier1)}")
    print(f"Tier 2 (translate or implement):      {len(tier2)}")
    print(f"Tier 3 (hardware-absent stubs):       {len(tier3)}")
    print(f"libc/newlib (link-time, ignore):      {len(libc)}")

    actionable = len(upstream) - len(libc)
    actionable_covered = len(overlap)
    print(f"\nActionable upstream surface:     {actionable}")
    print(f"Actionable coverage:             {actionable_covered / actionable * 100:.1f}%")

    return 0


if __name__ == "__main__":
    sys.exit(main())
