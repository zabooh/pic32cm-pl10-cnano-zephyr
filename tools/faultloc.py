#!/usr/bin/env python3
"""faultloc.py - locate the source of a Zephyr Cortex-M fault dump.

What it does, in one run:
  1. Generates a disassembly listing of the ELF (build/zephyr/zephyr.lst,
     source-interleaved) with arm-zephyr-eabi-objdump.
  2. Reads the fault dump the user copied from their terminal (TeraTerm etc.)
     out of the clipboard - the block that looks like:

         ***** HARD FAULT *****
         r0/a1:  0x...   ...   r14/lr:  0x0c00171b
         ...
         Faulting instruction address (r15/pc): 0x0c0009c8

  3. Resolves the faulting instruction (r15/pc) and the return address
     (r14/lr) to function + file:line, and prints the source line of each -
     i.e. WHERE the fault happened and WHO called it.

Usage:
    python tools/faultloc.py                 # dump from clipboard, ELF auto-found
    python tools/faultloc.py --file dump.txt # dump from a file instead
    python tools/faultloc.py --elf path/to/zephyr.elf
    python tools/faultloc.py --no-listing    # skip regenerating the .lst

No third-party packages required (stdlib only; clipboard via PowerShell).
"""

import argparse
import glob
import os
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


# --------------------------------------------------------------------------- #
# locating the ELF and the toolchain

def find_elf(explicit):
    candidates = []
    if explicit:
        candidates.append(Path(explicit))
    if os.environ.get("ZEPHYR_ELF"):
        candidates.append(Path(os.environ["ZEPHYR_ELF"]))
    candidates.append(REPO_ROOT / "build" / "zephyr" / "zephyr.elf")
    candidates.append(Path.cwd() / "build" / "zephyr" / "zephyr.elf")
    for c in candidates:
        if c.is_file():
            return c.resolve()
    sys.exit("error: could not find zephyr.elf - pass it with --elf "
             "(looked in build/zephyr/zephyr.elf)")


def find_tool(name, sdk):
    """Find an arm-zephyr-eabi-<name> binary."""
    exe = f"arm-zephyr-eabi-{name}"
    search = []
    if sdk:
        search.append(Path(sdk) / "gnu" / "arm-zephyr-eabi" / "bin")
    userprofile = os.environ.get("USERPROFILE", os.path.expanduser("~"))
    search += [Path(p) for p in glob.glob(
        os.path.join(userprofile, "zephyr-sdk-*", "gnu", "arm-zephyr-eabi", "bin"))]
    for d in search:
        for cand in (d / f"{exe}.exe", d / exe):
            if cand.is_file():
                return str(cand)
    # last resort: rely on PATH
    return exe


# --------------------------------------------------------------------------- #
# clipboard / dump input

def read_clipboard():
    # PowerShell Get-Clipboard is the most reliable text path on Windows.
    try:
        out = subprocess.run(
            ["powershell", "-NoProfile", "-Command", "Get-Clipboard -Raw"],
            capture_output=True, text=True, timeout=15)
        if out.returncode == 0 and out.stdout.strip():
            return out.stdout
    except Exception:
        pass
    # Fallback: tkinter (stdlib, cross-platform).
    try:
        import tkinter
        r = tkinter.Tk()
        r.withdraw()
        text = r.clipboard_get()
        r.destroy()
        return text
    except Exception:
        return ""


def parse_dump(text):
    """Pull the pc and lr addresses out of a Zephyr ARM fault dump."""
    result = {}

    m = re.search(r"r15/pc\)?\s*:?\s*0x([0-9a-fA-F]+)", text)
    if not m:
        m = re.search(r"[Ff]aulting instruction address.*?0x([0-9a-fA-F]+)", text)
    if m:
        result["pc"] = int(m.group(1), 16)

    m = re.search(r"r14/lr\s*:?\s*0x([0-9a-fA-F]+)", text)
    if m:
        result["lr"] = int(m.group(1), 16)

    return result


# --------------------------------------------------------------------------- #
# address -> source

def addr2line(tool, elf, addr):
    """Return a list of (function, file, line) frames (outermost last)."""
    out = subprocess.run(
        [tool, "-f", "-i", "-C", "-e", str(elf), f"0x{addr:08x}"],
        capture_output=True, text=True)
    lines = [l.rstrip() for l in out.stdout.splitlines() if l.strip()]
    frames = []
    for i in range(0, len(lines) - 1, 2):
        func = lines[i]
        loc = lines[i + 1]
        if ":" in loc:
            path, _, ln = loc.rpartition(":")
            ln = ln.split()[0] if ln else ""  # strip " (discriminator N)"
        else:
            path, ln = loc, ""
        frames.append((func, path, ln))
    return frames


def show_source(path, line, context=3):
    try:
        lineno = int(line)
    except (TypeError, ValueError):
        return
    p = Path(path)
    if not p.is_file():
        return
    try:
        src = p.read_text(errors="replace").splitlines()
    except Exception:
        return
    lo = max(1, lineno - context)
    hi = min(len(src), lineno + context)
    for n in range(lo, hi + 1):
        marker = ">>" if n == lineno else "  "
        print(f"      {marker} {n:5d}  {src[n - 1]}")


def print_location(title, tool, elf, addr):
    masked = addr & ~1  # drop the Thumb bit
    print(f"\n{title} (0x{addr:08x}):")
    frames = addr2line(tool, elf, masked)
    if not frames or frames[0][0] in ("??", ""):
        print("   (no debug info for this address - is it in flash/your code?)")
        return
    # frames[0] is the innermost (possibly inlined); print all, mark source of last.
    for idx, (func, path, ln) in enumerate(frames):
        tag = "function:" if idx == 0 else "inlined into:"
        loc = f"{path}:{ln}" if ln else path
        print(f"   {tag:14s} {func}")
        print(f"   {'source:':14s} {loc}")
    # show source of the innermost frame
    func, path, ln = frames[0]
    show_source(path, ln)


# --------------------------------------------------------------------------- #
# listing file + focused disassembly

def gen_listing(objdump, elf):
    out_path = elf.with_suffix(".lst")
    print(f"Generating listing: {out_path}")
    with open(out_path, "w", errors="replace") as f:
        subprocess.run([objdump, "-d", "-S", "-l", str(elf)], stdout=f, text=True)
    return out_path


def disasm_window(objdump, elf, addr, span=0x20):
    masked = addr & ~1
    start = max(0, masked - span)
    stop = masked + span
    out = subprocess.run(
        [objdump, "-d", "--start-address", hex(start), "--stop-address", hex(stop),
         str(elf)],
        capture_output=True, text=True)
    target = f"{masked:8x}:"
    print(f"\nDisassembly around the fault (0x{masked:08x} marked '=>'):")
    for line in out.stdout.splitlines():
        if re.match(r"\s*[0-9a-fA-F]+:", line):
            mark = "=>" if line.lstrip().startswith(target.lstrip()) else "  "
            print(f"   {mark} {line.strip()}")


# --------------------------------------------------------------------------- #

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf", help="path to zephyr.elf (default: auto-detect)")
    ap.add_argument("--sdk", help="Zephyr SDK dir (default: auto-detect under %%USERPROFILE%%)")
    ap.add_argument("--file", help="read the fault dump from this file instead of the clipboard")
    ap.add_argument("--no-listing", action="store_true", help="do not (re)generate the .lst file")
    args = ap.parse_args()

    elf = find_elf(args.elf)
    objdump = find_tool("objdump", args.sdk)
    addr2line_tool = find_tool("addr2line", args.sdk)
    print(f"ELF: {elf}")

    if not args.no_listing:
        gen_listing(objdump, elf)

    if args.file:
        dump_text = Path(args.file).read_text(errors="replace")
    else:
        dump_text = read_clipboard()

    if not dump_text.strip():
        sys.exit("error: no fault dump found (clipboard empty?). Copy the HARD FAULT "
                 "block from your terminal first, or use --file.")

    info = parse_dump(dump_text)
    if "pc" not in info and "lr" not in info:
        sys.exit("error: could not find 'r15/pc' or 'r14/lr' in the clipboard text.\n"
                 "Make sure you copied the whole fault dump.")

    print("\n" + "=" * 68)
    if "pc" in info:
        print_location("FAULT at (r15/pc)", addr2line_tool, elf, info["pc"])
    if "lr" in info:
        print_location("Called from (r14/lr, return address)", addr2line_tool, elf, info["lr"])
    print("=" * 68)

    if "pc" in info:
        disasm_window(objdump, elf, info["pc"])


if __name__ == "__main__":
    main()
