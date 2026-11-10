#!/usr/bin/env python3
"""NTSCRT doctor: check an NTSCRT install and collect everything needed to debug it into one zip.

  python3 ntscrt_doctor.py                  # checks + logs  -> ntscrt-diagnostics-<date>.zip on the Desktop
  python3 ntscrt_doctor.py --resolve-tests  # also run the DaVinci Resolve test suite (Resolve Studio; quit Resolve first)
  python3 ntscrt_doctor.py --out DIR

Get it with:
  curl -fsSL https://raw.githubusercontent.com/anthonymendez/NTSCRT-Plugins/main/scripts/ntscrt_doctor.py -o ntscrt_doctor.py
  (Windows PowerShell: irm https://raw.githubusercontent.com/anthonymendez/NTSCRT-Plugins/main/scripts/ntscrt_doctor.py -OutFile ntscrt_doctor.py)

Windows and macOS, Python 3.8+ standard library only. Read-only: it installs, changes and uploads
nothing. The zip holds system info (OS, CPU, GPU), the NTSCRT install state, the plug-in's own log,
the NTSCRT/OpenFX lines of Resolve's log (not the whole log: it names your projects), recent crash
reports of Resolve / Final Cut / NTSCRT, installer logs, and the test report when --resolve-tests ran.
Look through it before sending it on if you like: everything is plain text.
"""
import argparse
import datetime
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import time
import traceback
import urllib.request
import zipfile
from pathlib import Path

REPO = os.environ.get("NTSCRT_REPO", "anthonymendez/NTSCRT-Plugins")
WIN = sys.platform == "win32"
MAC = sys.platform == "darwin"
HOME = Path.home()
DAYS = 7  # crash reports / log lines this recent


def run(cmd, timeout=60):
    """Command output as text (stdout + stderr), never raises."""
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, errors="replace")
        return (r.stdout + (("\n[stderr]\n" + r.stderr) if r.stderr.strip() else "")).strip()
    except Exception as e:
        return f"[{' '.join(map(str, cmd))} failed: {e}]"


def ps(script):
    return run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script])


class Report:
    def __init__(self, out):
        self.out = out
        self.findings = []  # (level, text)
        self.sections = []

    def note(self, level, text):
        self.findings.append((level, text))
        print(f"  [{level}] {text}", flush=True)

    def section(self, title, body):
        self.sections.append((title, body if isinstance(body, str) else json.dumps(body, indent=2, default=str)))

    def copy(self, src, name=None, filter_re=None, max_bytes=4 << 20):
        """Copy a log into the bundle; with filter_re keep only matching lines (plus a count)."""
        src = Path(src)
        if not src.is_file():
            return False
        dst = self.out / "files" / (name or src.name)
        dst.parent.mkdir(parents=True, exist_ok=True)
        try:
            data = src.read_bytes()[-max_bytes:]
            if filter_re:
                lines = data.decode("utf-8", "replace").splitlines()
                keep = [l for l in lines if re.search(filter_re, l, re.I)]
                dst.write_text(f"# {src} — {len(keep)} of {len(lines)} lines matching /{filter_re}/\n" +
                               "\n".join(keep[-5000:]) + "\n", encoding="utf-8")
            else:
                dst.write_bytes(data)
            return True
        except Exception as e:
            dst.with_suffix(dst.suffix + ".error.txt").write_text(f"couldn't read {src}: {e}\n", encoding="utf-8")
            return False


# ------------------------------------------------------------------ checks --
def system_info(r):
    info = {"platform": platform.platform(), "machine": platform.machine(), "python": sys.version.split()[0],
            "when": datetime.datetime.now().isoformat(timespec="seconds")}
    r.section("System", info)
    if WIN:
        r.section("Windows: CPU / RAM / GPU", ps(
            "Get-CimInstance Win32_Processor | Select Name,NumberOfCores | Format-List; "
            "'RAM GB: ' + [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory/1GB,1); "
            "Get-CimInstance Win32_VideoController | Select Name,DriverVersion,AdapterRAM | Format-List"))
    elif MAC:
        r.section("macOS: version", run(["sw_vers"]))
        r.section("macOS: hardware / GPU", run(["system_profiler", "SPHardwareDataType", "SPDisplaysDataType",
                                                "-detailLevel", "mini"], timeout=120))


def app_versions(r):
    apps = {}
    if WIN:
        exe = Path(r"C:\Program Files\Blackmagic Design\DaVinci Resolve\Resolve.exe")
        apps["DaVinci Resolve"] = ps(f"(Get-Item '{exe}').VersionInfo.ProductVersion") if exe.exists() else None
    elif MAC:
        for name, plist in (("DaVinci Resolve", "/Applications/DaVinci Resolve/DaVinci Resolve.app/Contents/Info.plist"),
                            ("Final Cut Pro", "/Applications/Final Cut Pro.app/Contents/Info.plist"),
                            ("Motion", "/Applications/Motion.app/Contents/Info.plist"),
                            ("NTSCRT.app", "/Applications/NTSCRT.app/Contents/Info.plist")):
            apps[name] = run(["defaults", "read", plist, "CFBundleShortVersionString"]) if Path(plist).exists() else None
    r.section("Applications", apps)
    for name, v in apps.items():
        if name == "NTSCRT.app":
            continue
        r.note("info", f"{name}: {v or 'not found'}")
    if apps.get("DaVinci Resolve") is None and apps.get("Final Cut Pro") is None:
        r.note("warn", "neither DaVinci Resolve nor Final Cut Pro found in the standard location")
    return apps


def bundle_state(r, path, label):
    p = Path(path)
    st = {"path": str(p), "exists": p.exists()}
    if p.exists():
        v = p / "Contents" / "Resources" / "VERSION"
        st["version"] = v.read_text().strip() if v.exists() else None
        files = [f for f in p.rglob("*") if f.is_file()]
        st["files"] = len(files)
        st["bytes"] = sum(f.stat().st_size for f in files)
        st["has_shaders"] = (p / "Contents" / "Resources" / "shaders").is_dir()
        st["binaries"] = [str(f.relative_to(p)) for f in files if f.suffix in (".ofx", ".dll", ".dylib")
                          or f.parent.name in ("MacOS",)]
        if MAC:
            st["xattr"] = run(["xattr", "-lr", str(p)])[:4000]
            st["codesign"] = run(["codesign", "-dv", "--verbose=2", str(p)])
            st["codesign_verify"] = run(["codesign", "--verify", "--deep", "--strict", str(p)]) or "valid"
            if "com.apple.quarantine" in st["xattr"]:
                r.note("FAIL", f"{label}: quarantine flag set (macOS will block it): run the installer again, or "
                               f"sudo xattr -dr com.apple.quarantine '{p}'")
        if not st["has_shaders"] and p.suffix == ".bundle":
            r.note("FAIL", f"{label}: shaders folder missing (partial copy?): reinstall")
        if st.get("version"):
            r.note("ok", f"{label}: installed, version {st['version']}")
        else:
            r.note("warn", f"{label}: installed, but an old build without a version number: reinstall it")
    else:
        r.note("info", f"{label}: not installed ({p})")
    return st


def install_state(r):
    st = {}
    ofx = Path(r"C:\Program Files\Common Files\OFX\Plugins") if WIN else Path("/Library/OFX/Plugins")
    st["ofx_dir"] = str(ofx)
    st["ofx_entries"] = sorted(x.name for x in ofx.iterdir()) if ofx.is_dir() else None
    st["resolve_plugin"] = bundle_state(r, ofx / "NTSCRT.ofx.bundle", "Resolve plug-in")
    left = [n for n in (st["ofx_entries"] or []) if n.startswith("NTSCRT.ofx.bundle.") or n.startswith(".NTSCRT.ofx.bundle.")]
    if left:
        r.note("info", f"leftover install folders (harmless, removed by the next install): {', '.join(left)}")
    if os.environ.get("OFX_PLUGIN_PATH"):
        st["OFX_PLUGIN_PATH"] = os.environ["OFX_PLUGIN_PATH"]
        r.note("info", f"OFX_PLUGIN_PATH is set: {os.environ['OFX_PLUGIN_PATH']}")
    if MAC:
        st["fcp_app"] = bundle_state(r, "/Applications/NTSCRT.app", "Final Cut app")
        pk = run(["pluginkit", "-mAvvv", "-p", "FxPlug"])
        st["pluginkit_fxplug"] = pk
        if "ntscrt" in pk.lower():
            r.note("ok", "Final Cut: NTSCRT renderer is registered with pluginkit")
        elif Path("/Applications/NTSCRT.app").exists():
            r.note("FAIL", "Final Cut: NTSCRT.app is installed but its renderer isn't registered: open "
                           "/Applications/NTSCRT.app once, then restart Final Cut")
        tmpl = HOME / "Movies" / "Motion Templates.localized" / "Effects.localized" / "NTSCRT"
        st["fcp_template"] = sorted(str(x.relative_to(tmpl)) for x in tmpl.rglob("*")) if tmpl.exists() else None
        if Path("/Applications/NTSCRT.app").exists() and not tmpl.exists():
            r.note("warn", f"Final Cut: effect template missing ({tmpl}): open /Applications/NTSCRT.app once")
    if WIN:
        st["installed_programs"] = ps("Get-ItemProperty HKLM:\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\* "
                                      "| ? DisplayName -like '*NTSCRT*' | Select DisplayName,DisplayVersion,InstallDate | Format-List")
    r.section("NTSCRT install", st)
    return st


def resolve_running():
    if WIN:
        return "Resolve.exe" in run(["tasklist", "/FI", "IMAGENAME eq Resolve.exe", "/NH"])
    return subprocess.run(["pgrep", "-x", "Resolve"], capture_output=True).returncode == 0


# --------------------------------------------------------------------- logs --
def plugin_log_paths():
    tmp = Path(tempfile.gettempdir())
    if WIN:
        base = Path(os.environ.get("LOCALAPPDATA", HOME / "AppData" / "Local")) / "NTSCRT"
    else:
        base = HOME / "Library" / "Logs" / "NTSCRT"
    paths = [base / "ntscrt-ofx.log", base / "ntscrt-ofx.old.log", tmp / "ntscrt-ofx-dev" / "ntscrt-ofx.log"]
    if os.environ.get("NTSCRT_OFX_LOG"):
        paths.insert(0, Path(os.environ["NTSCRT_OFX_LOG"]))
    if MAC:  # the Final Cut renderer is sandboxed: its log lives in its container
        # (macOS 14+ may ask whether Terminal may "access data from other apps": allow it)
        try:
            for d in sorted((HOME / "Library" / "Containers").glob("*NTSCRT*/Data/Library/Logs/NTSCRT")):
                paths += [d / "ntscrt-fxplug.log", d / "ntscrt-fxplug.old.log"]
        except OSError:
            pass
    return paths


def log_name(p):
    """Name in the zip: plugin-<folder>-<file>, plus the container for Final Cut logs."""
    parts = Path(p).parts
    if "Containers" in parts and len(parts) > parts.index("Containers") + 1:
        return f"plugin-{parts[parts.index('Containers') + 1]}-{Path(p).name}"
    return f"plugin-{Path(p).parent.name}-{Path(p).name}"


def resolve_log_dirs():
    if WIN:
        return [Path(os.environ.get("APPDATA", HOME / "AppData" / "Roaming")) / "Blackmagic Design" / "DaVinci Resolve" / "Support" / "logs"]
    return [HOME / "Library" / "Application Support" / "Blackmagic Design" / "DaVinci Resolve" / "logs",
            Path("/Library/Application Support/Blackmagic Design/DaVinci Resolve/logs")]


def collect_logs(r):
    found = []
    for p in plugin_log_paths():
        if r.copy(p, log_name(p)):
            found.append(str(p))
            text = p.read_text("utf-8", "replace")
            fails = [l for l in text.splitlines() if "FAILED" in l or "error=" in l or "] error: " in l]
            if fails:
                r.note("FAIL", f"plug-in log {p.name} has {len(fails)} failures, last: {fails[-1][:200]}")
            if " debug render #" in text or "] debug render #" in text:
                r.note("info", f"{p.name}: includes Debug logging output")
    if not found:
        r.note("info", "no NTSCRT plug-in log yet (the plug-in writes one once Resolve / Final Cut loads it)")
    r.section("Plug-in logs found", found or "none")
    if MAC:
        # [NTSCRT] lines the Final Cut renderer sent to the system log (load, failures).
        out = run(["log", "show", "--last", "24h", "--style", "compact",
                   "--predicate", 'eventMessage CONTAINS "[NTSCRT]"'], timeout=180)
        dst = r.out / "files" / "macos-system-log-ntscrt-24h.txt"
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(out[-(4 << 20):] + "\n", encoding="utf-8")
    # Resolve: only NTSCRT / OpenFX / error lines (the full log names your projects)
    pat = r"ntscrt|openfx|\bofx\b|fusion.*error|crash|exception|gpu|cuda|metal|opencl"
    for d in resolve_log_dirs():
        for name in ("davinci_resolve.log", "ResolveDebug.txt", "resolve_graphics_log.txt"):
            r.copy(d / name, f"resolve-{name}.filtered.txt", filter_re=pat)
    # crash reports
    cutoff = time.time() - DAYS * 86400
    crash_dirs = ([HOME / "Library" / "Logs" / "DiagnosticReports", Path("/Library/Logs/DiagnosticReports")] if MAC else
                  [Path(os.environ.get("LOCALAPPDATA", "")) / "CrashDumps",
                   Path(os.environ.get("PROGRAMDATA", r"C:\ProgramData")) / "Microsoft" / "Windows" / "WER" / "ReportArchive"])
    crashes = []
    for d in crash_dirs:
        if not d.is_dir():
            continue
        for f in d.rglob("*"):
            n = f.name.lower()
            if f.is_file() and f.stat().st_mtime > cutoff and any(k in n for k in ("resolve", "ntscrt", "final cut", "finalcut", "fxplug", "motion")):
                if f.suffix.lower() in (".dmp",):  # minidumps: list only (large, binary)
                    crashes.append(f"{f} ({f.stat().st_size} bytes, not copied)")
                else:
                    r.copy(f, "crash-" + f.name)
                    crashes.append(str(f))
    if crashes:
        r.note("warn", f"{len(crashes)} crash report(s) in the last {DAYS} days (included)")
    r.section("Crash reports", crashes or "none")
    if WIN:
        r.section("Windows event log: application errors mentioning Resolve/NTSCRT (7 days)", ps(
            "Get-WinEvent -FilterHashtable @{LogName='Application'; Level=1,2; StartTime=(Get-Date).AddDays(-7)} "
            "-ErrorAction SilentlyContinue | ? { $_.Message -match 'Resolve|NTSCRT' } | Select -First 20 TimeCreated,Id,ProviderName,Message | Format-List"))
    # installer logs
    for p in ([Path("/tmp/ntscrt-install.log")] if MAC else []) + list(Path(tempfile.gettempdir()).glob("ntscrt*.log")):
        r.copy(p, "installer-" + p.name)
    if MAC:
        r.copy("/var/log/install.log", "macos-install.log.filtered.txt", filter_re=r"ntscrt")


# -------------------------------------------------------------- Resolve tests --
def fetch_suite(dest, version):
    """The test suite and the two repo files it reads, from the tag matching the installed version
    (or main). Returns the suite path."""
    refs = [f"v{version}"] if version else []
    refs.append("main")
    for ref in refs:
        try:
            for rel in ("tests/resolve/run_resolve_tests.py", "core/params/schema.json", "VERSION"):
                url = f"https://raw.githubusercontent.com/{REPO}/{ref}/{rel}"
                (dest / rel).parent.mkdir(parents=True, exist_ok=True)
                with urllib.request.urlopen(url, timeout=30) as resp:
                    (dest / rel).write_bytes(resp.read())
            return dest / "tests/resolve/run_resolve_tests.py", ref
        except Exception:
            continue
    raise RuntimeError(f"couldn't download the test suite from github.com/{REPO}")


def resolve_tests(r, install, args):
    here = Path(__file__).resolve().parent.parent
    suite = here / "tests" / "resolve" / "run_resolve_tests.py"
    ref = "local checkout"
    if not suite.exists():
        version = (install.get("resolve_plugin") or {}).get("version")
        suite, ref = fetch_suite(Path(tempfile.mkdtemp(prefix="ntscrt-suite-")), version)
    out = r.out / "resolve-tests"
    cmd = [sys.executable, str(suite), "--out", str(out)]
    if resolve_running():
        r.note("info", "Resolve is running: testing in it (don't touch Resolve until the tests finish). "
                       "Quit Resolve first to run headless instead.")
    else:
        cmd.append("--headless")
    if args.quick:
        cmd.append("--quick")
    print(f"\nRunning the Resolve tests ({ref}) — this takes a few minutes…\n", flush=True)
    t0 = time.time()
    p = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    (r.out / "files").mkdir(parents=True, exist_ok=True)
    (r.out / "files" / "resolve-tests-output.txt").write_text(p.stdout + "\n[stderr]\n" + p.stderr, encoding="utf-8")
    rep = out / "report.json"
    if rep.exists():
        doc = json.loads(rep.read_text(encoding="utf-8"))
        s = doc["summary"]
        for t in doc["results"]:
            if t["status"] == "fail":
                r.note("FAIL", f"Resolve test {t['test']}: {t['notes']}")
        r.note("ok" if not s["fail"] else "FAIL",
               f"Resolve tests: {s['pass']} passed, {s['fail']} failed, {s['skip']} skipped ({time.time() - t0:.0f}s)")
    else:
        tail = (p.stdout + p.stderr).strip().splitlines()[-3:]
        r.note("FAIL", "Resolve tests didn't run: " + " | ".join(tail))


# --------------------------------------------------------------------- main --
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", help="where to write the zip (default: Desktop)")
    ap.add_argument("--resolve-tests", action="store_true", help="also run the DaVinci Resolve test suite")
    ap.add_argument("--quick", action="store_true", help="with --resolve-tests: fewer frames")
    a = ap.parse_args()

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    dest = Path(a.out) if a.out else (HOME / "Desktop" if (HOME / "Desktop").is_dir() else Path.cwd())
    work = Path(tempfile.mkdtemp(prefix="ntscrt-doctor-")) / f"ntscrt-diagnostics-{stamp}"
    work.mkdir(parents=True)
    r = Report(work)
    print(f"NTSCRT doctor ({platform.system()} {platform.release()})\n", flush=True)
    install = {}
    for step in (system_info, app_versions, lambda rr: install.update(install_state(rr) or {}), collect_logs):
        try:
            step(r)
        except Exception:
            r.note("warn", f"a check failed: {traceback.format_exc().splitlines()[-1]}")
            r.section("doctor error", traceback.format_exc())
    if a.resolve_tests:
        try:
            resolve_tests(r, install, a)
        except Exception as e:
            r.note("FAIL", f"Resolve tests: {e}")
            r.section("doctor error (tests)", traceback.format_exc())

    lines = [f"NTSCRT diagnostics {stamp}", ""]
    lines += [f"[{lvl}] {txt}" for lvl, txt in r.findings]
    for title, body in r.sections:
        lines += ["", "=" * 78, title, "=" * 78, body]
    (work / "summary.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")

    dest.mkdir(parents=True, exist_ok=True)
    zpath = dest / f"{work.name}.zip"
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for f in work.rglob("*"):
            if f.is_file() and f.suffix.lower() != ".png" or (f.is_file() and "diff" in f.name):
                z.write(f, f.relative_to(work.parent))
    shutil.rmtree(work.parent, ignore_errors=True)
    fails = sum(1 for lvl, _ in r.findings if lvl == "FAIL")
    print(f"\n{'No problems found.' if not fails else f'{fails} problem(s) found (see above).'}")
    print(f"Diagnostics: {zpath}\nSend that file along with a description of what went wrong.")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
