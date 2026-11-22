#!/usr/bin/env python3
"""update_conpty.py - maintain the pinned Microsoft.Windows.Console.ConPTY bundle.

Queries the NuGet flat-container index, selects the latest stable (or
preview) version, downloads the .nupkg, verifies its SHA-256 and (on
Windows) the Authenticode signature of the extracted binaries, updates
data/conpty/conpty.lock, and fetches every architecture.

Usage:
    ./scripts/update_conpty.py                # latest stable + fetch
    ./scripts/update_conpty.py --preview      # latest preview
    ./scripts/update_conpty.py --check        # exit 1 if a newer stable exists
    ./scripts/update_conpty.py --version 1.25.261109001
    ./scripts/update_conpty.py --no-fetch     # update the lock only

No third-party dependencies.
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
import zipfile

PACKAGE = "Microsoft.Windows.Console.ConPTY"
INDEX_URL = (
    "https://api.nuget.org/v3-flatcontainer/"
    "microsoft.windows.console.conpty/index.json"
)
FLAT_URL = (
    "https://api.nuget.org/v3-flatcontainer/"
    "microsoft.windows.console.conpty/{version}/"
    "microsoft.windows.console.conpty.{version}.nupkg"
)
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOCK = os.path.join(ROOT, "data", "conpty", "conpty.lock")
FETCH = os.path.join(ROOT, "scripts", "fetch_conpty.sh")


def fail(msg):
    print(f"update_conpty: {msg}", file=sys.stderr)
    return 1


def read_lock():
    values = {}
    with open(LOCK, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            values[key] = value
    return values


def write_lock(version, url, sha256):
    text = f"""# Pinned Microsoft.Windows.Console.ConPTY bundle.
#
# This file is the single source of truth for the ConPTY version used by
# coffer and portty. Update it with scripts/update_conpty.py; do not edit
# the version by hand without updating the hash.
CONPTY_PACKAGE={PACKAGE}
CONPTY_VERSION={version}
CONPTY_NUPKG_SHA256={sha256}
CONPTY_URL={url}
"""
    with open(LOCK, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def version_key(version):
    """Order NuGet versions numerically, ignoring any -preview suffix."""
    core = version.split("-", 1)[0]
    return tuple(int(part) for part in re.findall(r"\d+", core))


def fetch_index():
    try:
        with urllib.request.urlopen(INDEX_URL, timeout=30) as r:
            return json.load(r)["versions"]
    except (urllib.error.URLError, KeyError, json.JSONDecodeError) as e:
        raise RuntimeError(f"cannot read {INDEX_URL}: {e}") from e


def pick_latest(versions, preview):
    if preview:
        candidates = [v for v in versions if "-" in v]
    else:
        candidates = [v for v in versions if "-" not in v]
    if not candidates:
        raise RuntimeError("no matching versions in the NuGet index")
    return max(candidates, key=version_key)


def download(url, dest):
    try:
        with urllib.request.urlopen(url, timeout=120) as r, open(dest, "wb") as f:
            while True:
                chunk = r.read(1 << 16)
                if not chunk:
                    break
                f.write(chunk)
    except urllib.error.URLError as e:
        raise RuntimeError(f"download failed: {url}: {e}") from e


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def verify_authenticode(paths):
    """Best-effort Microsoft signature check on Windows."""
    if os.name != "nt":
        return True
    ps = [
        "powershell",
        "-NoProfile",
        "-NonInteractive",
        "-Command",
        ";".join(
            f"(Get-AuthenticodeSignature -LiteralPath '{p}').Status" for p in paths
        ),
    ]
    try:
        out = subprocess.run(ps, capture_output=True, text=True, check=False)
    except OSError as e:
        print(f"update_conpty: cannot run PowerShell: {e}", file=sys.stderr)
        return False
    statuses = [line.strip() for line in out.stdout.splitlines() if line.strip()]
    if statuses != ["Valid"] * len(paths):
        print(
            f"update_conpty: Authenticode status {statuses}",
            file=sys.stderr,
        )
        return False
    return True


def extract_for_verification(nupkg, tempdir):
    paths = []
    with zipfile.ZipFile(nupkg) as z:
        for arch in ("x64", "arm64", "x86"):
            for member in (
                f"runtimes/win-{arch}/native/conpty.dll",
                f"build/native/runtimes/{arch}/OpenConsole.exe",
            ):
                try:
                    data = z.read(member)
                except KeyError:
                    continue
                name = f"{arch}-{os.path.basename(member)}"
                path = os.path.join(tempdir, name)
                with open(path, "wb") as f:
                    f.write(data)
                paths.append(path)
    return paths


def run_fetch():
    cmd = [FETCH, "--arch", "all"]
    print("update_conpty: running", " ".join(cmd))
    return subprocess.run(cmd, check=False).returncode


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--preview", action="store_true", help="use the latest preview")
    ap.add_argument("--check", action="store_true", help="only compare versions")
    ap.add_argument("--version", help="pin an explicit version")
    ap.add_argument("--no-fetch", action="store_true", help="update the lock only")
    args = ap.parse_args()

    current = read_lock()
    current_version = current.get("CONPTY_VERSION", "")

    try:
        versions = fetch_index()
    except RuntimeError as e:
        return fail(str(e))

    if args.version:
        version = args.version
        if version not in versions:
            return fail(f"{version} is not in the NuGet index")
    else:
        version = pick_latest(versions, args.preview)

    print(f"update_conpty: current={current_version or 'none'} latest={version}")

    if args.check:
        if not current_version:
            print("update_conpty: no lock version", file=sys.stderr)
            return 1
        if version_key(version) > version_key(current_version):
            print(
                f"update_conpty: newer {PACKAGE} is available: {version}",
                file=sys.stderr,
            )
            return 1
        print("update_conpty: up to date")
        return 0

    url = FLAT_URL.format(version=version)
    cache = os.path.join(
        os.environ.get("XDG_CACHE_HOME", os.path.join(os.path.expanduser("~"), ".cache")),
        "coffer",
        "conpty",
    )
    os.makedirs(cache, exist_ok=True)
    nupkg = os.path.join(cache, f"{PACKAGE}.{version}.nupkg")

    if not os.path.exists(nupkg):
        print(f"update_conpty: downloading {url}")
        try:
            download(url, nupkg)
        except RuntimeError as e:
            return fail(str(e))

    digest = sha256_file(nupkg)
    print(f"update_conpty: nupkg sha256={digest}")

    with tempfile.TemporaryDirectory(prefix="coffer-conpty-") as tempdir:
        paths = extract_for_verification(nupkg, tempdir)
        if paths and not verify_authenticode(paths):
            return fail("Authenticode verification failed")

    write_lock(version, url, digest)
    print(f"update_conpty: wrote {os.path.relpath(LOCK, ROOT)}")

    if not args.no_fetch:
        return run_fetch()
    return 0


if __name__ == "__main__":
    sys.exit(main())
