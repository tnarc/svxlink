#!/usr/bin/env python3
"""Regression test for GitHub issue #573: audio from a deactivated (offline)
logic core is still forwarded over the logic-linking layer.

Taking a logic offline (ONLINE_CMD + "0", e.g. via DTMF) must disconnect it
from the linking layer so its received audio no longer reaches linked logics
(most visibly, a deactivated node kept transmitting its RX audio onto a
connected SvxReflector talk group). Bringing it back online must restore the
link.

Topology: two Simplex logics on one active link. Logic1 is the source; a tone
streamed into its receiver (opening its VOX squelch) is relayed out the
listener's (Logic2) transmitter, measured with a Goertzel filter.
MUTE_RX_ON_TX/MUTE_TX_ON_RX are disabled so a Simplex listener can relay link
audio (see test_audio_modes.py). ONLINE_CMD is "80" to avoid colliding with the
link's "91" command prefix; "800" takes Logic1 offline, "801" back online.

Squelch/DTMF timing: a queued DTMF command (the offline command) only executes
while the source squelch is closed. A Local UDP RX gets no samples once the tone
stops, so its VOX squelch stays stuck open; the squelch is therefore driven
closed by streaming near-silence until the log confirms the close before the
offline command is issued.

Run:  python3 tests/test_offline_mute.py     (exit 0 = pass)
"""

import sys
import time
import traceback
from contextlib import contextmanager

import harness
from harness import SvxlinkHarness, LinkSpec, SAMPLE_RATE

F1 = 1000                                 # source tone
NO_MUTE = {"MUTE_RX_ON_TX": "0", "MUTE_TX_ON_RX": "0"}
ONLINE_CMD = "80"                         # -> "800" offline, "801" online


@contextmanager
def _harness():
    links = [LinkSpec("L", ["Logic1", "Logic2"], prefix="91",
                      default_active=True)]
    h = SvxlinkHarness(num_logics=2, logic_type="Simplex", links=links,
                       logic_opts=NO_MUTE,
                       per_logic_opts={"Logic1": {"ONLINE_CMD": ONLINE_CMD}})
    h.setup()
    h.start()
    try:
        yield h
    finally:
        h.cleanup()


def _assert(cond, msg):
    if not cond:
        raise AssertionError(msg)


def _relay_level(h):
    """Stream the source tone and return how much of it the listener relayed."""
    h.start_talk("Logic1", F1)
    time.sleep(1.0)
    level = h.tone_level("Logic2", F1, 1.2)
    h.stop_talk("Logic1")
    return level


def _close_squelch(h, logic="Logic1", timeout=8.0):
    """Drive the logic's VOX squelch closed by streaming near-silence until the
    log reports the close (a Local UDP RX never re-evaluates VOX to silence once
    streaming just stops). Required before a queued DTMF command will execute."""
    marker = f"Rx_{logic}: The squelch is CLOSED"
    before = h.read_log().count(marker)
    s = harness._ToneStreamer(h.by_name[logic].rx_port, F1, SAMPLE_RATE, 0)
    s.start()
    try:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if h.read_log().count(marker) > before:
                return
            time.sleep(0.1)
        raise RuntimeError(f"squelch for {logic} did not close within {timeout}s")
    finally:
        s.stop()


def _wait_log(h, text, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if text in h.read_log():
            return True
        time.sleep(0.1)
    return False


def test_offline_logic_stops_forwarding_audio():
    """A logic taken offline must stop relaying its RX audio to linked logics,
    and must resume once brought back online."""
    with _harness() as h:
        online = _relay_level(h)                   # baseline: online relays
        _assert(online > 500,
                f"online source should be relayed to the linked logic, "
                f"got {online:.0f}")

        _close_squelch(h)                          # let the DTMF command run
        h.send_dtmf("Logic1", ONLINE_CMD + "0#")   # take Logic1 offline
        _assert(_wait_log(h, "Logic1: Setting logic OFFLINE"),
                "offline command was not processed")

        offline = _relay_level(h)                   # must no longer be relayed
        _assert(offline < 150,
                f"offline source must NOT be relayed over the link "
                f"(issue #573), got {offline:.0f}")

        # The online command (ONLINE_CMD + "1") is handled directly on command
        # completion, so it does not require the squelch to be closed first.
        h.send_dtmf("Logic1", ONLINE_CMD + "1#")   # bring Logic1 back online
        _assert(_wait_log(h, "Logic1: Setting logic ONLINE"),
                "online command was not processed")

        reonline = _relay_level(h)
        _assert(reonline > 500,
                f"link should resume after the logic is back online, "
                f"got {reonline:.0f}")


TESTS = [
    test_offline_logic_stops_forwarding_audio,
]


def main():
    failures = 0
    for t in TESTS:
        try:
            t()
            print(f"PASS  {t.__name__}")
        except Exception:
            failures += 1
            print(f"FAIL  {t.__name__}")
            traceback.print_exc()
    print(f"\n{len(TESTS) - failures}/{len(TESTS)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
