"""
hold_reset.py - hold the PIC32CM PL10 in HARDWARE reset via pyOCD (nEDBG/CMSIS-DAP).

    python tools/hold_reset.py        # assert nRESET, hold until Ctrl-C
    python tools/hold_reset.py 10     # hold ~10 s, then release automatically

Drives nRESET low through the on-board debugger and keeps it asserted for as long
as this process runs. Verified on this board: `probe.assert_reset(True)` holds
nRESET *statically* (unlike the pyOCD commander's `set nreset 0`, which only
pulses it) - while held, the whole target is in reset and draws its reset-floor
current, a clean zero reference for a current measurement. On release the target
comes out of reset and runs its firmware again.

Only one pyOCD client can hold the probe at a time - stop any gdbserver /
`west flash` / other pyocd first.
"""
import subprocess
import sys
import time

from pyocd.core.helpers import ConnectHelper

TARGET = "pic32cm6408pl10048"
FREQ = 100000


def boot_target():
    """Reset the target and let it run - via a fresh `pyocd` subprocess, because an
    in-process reset over the same held session leaves the console SERCOM dead. Uses
    `<this python> -m pyocd`, which is guaranteed to work: this interpreter already
    imported pyocd above, so it can always run it as a module (no PATH / .exe hunt)."""
    subprocess.run([sys.executable, "-m", "pyocd", "cmd", "-t", TARGET, "-f", str(FREQ),
                    "-M", "halt", "-c", "reset", "-c", "go"], capture_output=True)

hold_s = float(sys.argv[1]) if len(sys.argv) > 1 else None

session = ConnectHelper.session_with_chosen_probe(
    target_override=TARGET, frequency=FREQ, connect_mode="attach",
    options={"resume_on_disconnect": False})
session.open()
probe = session.probe
target = session.target

probe.assert_reset(True)
time.sleep(0.2)
try:
    target.read32(0x0c000000)  # bus is dead while the core is in reset
    print("WARNING: core still responds - this probe may only pulse nRESET.")
except Exception:
    print("nRESET asserted - target is HELD in reset (core/bus unreachable).")

try:
    if hold_s is None:
        print("Holding until Ctrl-C ...")
        while True:
            time.sleep(1)
    else:
        print(f"Holding ~{hold_s:g} s ...")
        time.sleep(hold_s)
except KeyboardInterrupt:
    pass
finally:
    # Release nRESET, then make sure the core RUNS again. Opening the session with
    # connect_mode=attach halts the core, so without an explicit resume the target
    # is left reachable-but-halted (firmware not running, console dead).
    probe.assert_reset(False)
    session.close()
    boot_target()  # reset+run in a fresh pyocd so the firmware/console come back
    print("released - target running again.")
