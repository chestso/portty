#!/usr/bin/env python3
"""verify_conpty_bundle.py - sanity-check the fetched ConPTY bundles.

Checks, for every architecture that is present (or that `--arch` names):

  * conpty.dll and OpenConsole.exe exist and are Windows PE images whose
    COFF machine type matches the directory (x64, arm64, x86);
  * SHA256SUMS matches the two files;
  * VERSION, when present, agrees with conpty.lock.

This is what keeps `make dist` from shipping an arm64 bundle that is
really an x64 DLL, or a stale SHA256SUMS, on a machine that cannot run
the binaries.  No third-party dependencies.
"""

import argparse
import hashlib
import os
import sys

ARCHES = ("x64", "arm64", "x86")
MACHINES = {
    "x64": 0x8664,
    "arm64": 0xAA64,
    "x86": 0x14C,
}
MACHINE_NAMES = {v: k for k, v in MACHINES.items()}
FILES = ("conpty.dll", "OpenConsole.exe")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def fail(msg):
    print(f"verify_conpty_bundle: {msg}", file=sys.stderr)
    return 1


def read_lock(path):
    values = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            values[key] = value
    return values


def read_version(path):
    values = {}
    if not os.path.exists(path):
        return values
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            values[key] = value
    return values


def pe_machine(path):
    """Return the COFF machine type of a PE image, or None."""
    with open(path, "rb") as f:
        dos = f.read(0x40)
        if len(dos) < 0x40 or dos[:2] != b"MZ":
            return None
        offset = int.from_bytes(dos[0x3C:0x40], "little")
        f.seek(offset)
        pe = f.read(6)
        if len(pe) < 6 or pe[:4] != b"PE\0\0":
            return None
        return int.from_bytes(pe[4:6], "little")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def check_arch(base, arch, expected_version):
    directory = os.path.join(base, arch)
    errors = []

    for name in FILES:
        path = os.path.join(directory, name)
        if not os.path.isfile(path):
            errors.append(f"{arch}/{name} is missing")
            continue
        machine = pe_machine(path)
        if machine is None:
            errors.append(f"{arch}/{name} is not a PE image")
        elif machine != MACHINES[arch]:
            got = MACHINE_NAMES.get(machine, f"0x{machine:04x}")
            errors.append(
                f"{arch}/{name} is a {got} binary, expected {arch}"
            )

    sums_path = os.path.join(directory, "SHA256SUMS")
    if not os.path.isfile(sums_path):
        errors.append(f"{arch}/SHA256SUMS is missing")
    else:
        with open(sums_path, encoding="utf-8") as f:
            seen = {}
            for line in f:
                line = line.strip()
                if not line:
                    continue
                parts = line.split(None, 1)
                if len(parts) != 2:
                    errors.append(f"{arch}/SHA256SUMS: malformed line: {line}")
                    continue
                seen[parts[1].lstrip("*")] = parts[0].lower()
            for name in FILES:
                path = os.path.join(directory, name)
                if not os.path.isfile(path):
                    continue
                if name not in seen:
                    errors.append(f"{arch}/SHA256SUMS does not list {name}")
                elif seen[name] != sha256_file(path):
                    errors.append(f"{arch}/{name} does not match SHA256SUMS")

    version_path = os.path.join(base, "VERSION")
    version = read_version(version_path).get("version")
    if expected_version and version and version != expected_version:
        errors.append(
            f"VERSION says {version}, conpty.lock pins {expected_version}"
        )

    return errors


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--dir",
        default=os.path.join(ROOT, "data", "conpty"),
        help="bundle root (default: data/conpty)",
    )
    ap.add_argument(
        "--arch",
        action="append",
        choices=ARCHES + ("all",),
        help="architecture to check; repeatable (default: every present one)",
    )
    args = ap.parse_args()

    if not os.path.isdir(args.dir):
        return fail(f"{args.dir} does not exist")

    lock_path = os.path.join(args.dir, "conpty.lock")
    expected_version = None
    if os.path.isfile(lock_path):
        expected_version = read_lock(lock_path).get("CONPTY_VERSION")
    else:
        print(f"verify_conpty_bundle: no {lock_path}; skipping version check",
              file=sys.stderr)

    if args.arch:
        arches = []
        for arch in args.arch:
            if arch == "all":
                arches = list(ARCHES)
                break
            if arch not in arches:
                arches.append(arch)
    else:
        arches = [a for a in ARCHES if os.path.isdir(os.path.join(args.dir, a))]
        if not arches:
            return fail(f"no architecture directories under {args.dir}")

    errors = []
    for arch in arches:
        arch_errors = check_arch(args.dir, arch, expected_version)
        if arch_errors:
            errors.extend(arch_errors)
        else:
            print(f"verify_conpty_bundle: {arch} ok")

    if errors:
        for error in errors:
            print(f"verify_conpty_bundle: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
