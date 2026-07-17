"""
hold_reset.py - hold the PIC32CM PL10 in HARDWARE reset via pyOCD (nEDBG/CMSIS-DAP).

    python hold_reset.py           # assert nRESET, hold until Ctrl-C
    python hold_reset.py 10        # hold ~10 s, then release automatically

Asserts nRESET through the on-board debugger and keeps it asserted for as long as
this process runs. Verified on this board: while held the core/bus is unreachable
(TransferError on any read) - the whole target is in reset - so the target draws
its reset-floor current. Handy as a zero reference for a current measurement.
Releasing (Ctrl-C / timeout / exit) deasserts nRESET and the target boots again.

Note: only one pyOCD may hold the probe at a time - stop any gdbserver/west flash
first. Uses connect_mode=attach so opening the session doesn't reset/run the core.
"""
import sys
import time

from pyocd.core.helpers import ConnectHelper

TARGET = "pic32cm6408pl10048"
FREQ = 100000

hold_s = float(sys.argv[1]) if len(sys.argv) > 1 else None

session = ConnectHelper.session_with_chosen_probe(
    target_override=TARGET, frequency=FREQ, connect_mode="attach",
    options={"resume_on_disconnect": False})
session.open()
probe = session.probe

probe.assert_reset(True)
time.sleep(0.2)
try:
    session.target.read32(0x0c000000)
    print("WARNING: core still responds after assert - this probe may only pulse nRESET.")
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
    probe.assert_reset(False)
    session.close()
    print("released - target running again.")
