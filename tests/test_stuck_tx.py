#!/usr/bin/env python3
"""Regression test: the transmitter of a logic must unkey when the over it is
carrying from a linked logic ends, also when the audio routing into it changes
in the middle of that over.

LinkManager swaps the selector (FIRST) / mixer (MIX, DUCK, PRIORITY) that
drives a logic's audio input when connections change. An Async audio source
that is unregistered from its sink does not flush it, so a swap in the middle
of an over used to leave the logic input without an end of stream and the
transmitter keyed forever. Two ways to trigger such a swap are covered, for
every audio mode:

  * the link is deactivated (DTMF on the sink logic) while audio flows;
  * a module that mutes linking (MUTE_LOGIC_LINKING, e.g. Parrot) is activated
    on the sink logic while audio flows.

The same is checked with PRIORITY_SOURCES set and with overlapping links
(PRIORITY over MIX or FIRST on the same pair, and a PRIORITY + MIX chain via
a third logic), where the mode of the connection into the sink is decided by
more than one link and may change in the middle of the over.

Logic1 is the sink, Logic2 the talker. Keyed/unkeyed is taken from the
"Tx_Logic1: Turning the transmitter ON/OFF" log lines.

Run directly:  python3 tests/test_stuck_tx.py     (exit 0 = pass)
"""

import os
import sys
import time
import traceback

import harness
from harness import SvxlinkHarness, LinkSpec, SAMPLE_RATE, REPO

NO_MUTE = {"MUTE_RX_ON_TX": "0", "MUTE_TX_ON_RX": "0"}
MODES = ("FIRST", "MIX", "DUCK", "PRIORITY")
TX_ON = "Tx_Logic1: Turning the transmitter ON"
TX_OFF = "Tx_Logic1: Turning the transmitter OFF"


def _assert(cond, msg):
    if not cond:
        raise AssertionError(msg)


def _close_squelch(h, logic, freq, timeout=8.0):
    """Stream silence into the RX until the VOX squelch has closed."""
    marker = f"Rx_{logic}: The squelch is CLOSED"
    before = h.read_log().count(marker)
    s = harness._ToneStreamer(h.by_name[logic].rx_port, freq, SAMPLE_RATE, 0)
    s.start()
    try:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if h.read_log().count(marker) > before:
                return
            time.sleep(0.1)
        raise RuntimeError("squelch did not close")
    finally:
        s.stop()


def _tx_keyed(h):
    log = h.read_log()
    return log.count(TX_ON) > log.count(TX_OFF)


def _wait_unkeyed(h, timeout=6.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not _tx_keyed(h):
            return True
        time.sleep(0.2)
    return False


def _parrot_opts():
    with open(os.path.join(REPO, "src", "svxlink", "modules", "parrot",
                           "Parrot.tcl")) as f:
        tcl = f.read()
    return dict(
        per_logic_opts={"Logic1": {"MODULES": "ModuleParrot"}},
        global_opts={"MODULE_PATH": os.path.join(REPO, "build", "lib")},
        extra_sections={"ModuleParrot": ["NAME=Parrot", "ID=1", "TIMEOUT=600",
                                         "FIFO_LEN=60", "REPEAT_DELAY=1000"]},
        local_event_tcl={"Parrot.tcl": tcl},
    )


def _run(mode, action, links=None, num_logics=2, dtmf=("Logic1", "91#")):
    """Logic2 talks into Logic1 and `action` happens mid-over: "none",
    "deactivate" (send DTMF `dtmf` = (logic, digits)) or "module" (activate
    Parrot on Logic1). `links` defaults to one link L of the given mode."""
    if links is None:
        links = [LinkSpec("L", ["Logic1", "Logic2"], default_active=True,
                          audio_mode=mode)]
    extra = _parrot_opts() if action == "module" else {}
    h = SvxlinkHarness(num_logics=num_logics, logic_type="Simplex",
                       links=links, logic_opts=NO_MUTE, **extra)
    h.setup()
    h.start()
    try:
        h.start_talk("Logic2", 1000)
        time.sleep(1.5)
        _assert(_tx_keyed(h), f"{mode}: Logic1 should transmit the over")
        if action == "deactivate":
            h.send_dtmf(*dtmf)              # deactivate a link mid-over
        elif action == "module":
            h.send_dtmf("Logic1", "1#")     # activate Parrot mid-over
        time.sleep(1.0)
        h.stop_talk("Logic2")
        _close_squelch(h, "Logic2", 1000)
        _assert(_wait_unkeyed(h),
                f"{mode}/{action}: Logic1 transmitter still keyed after the "
                f"over ended")
    finally:
        h.cleanup()


def _make(mode, action):
    def t():
        _run(mode, action)
    t.__name__ = f"test_{mode.lower()}_{action}"
    return t


TESTS = [_make(m, a) for a in ("none", "deactivate", "module") for m in MODES]


# The same with PRIORITY_SOURCES and with links that overlap, so that the
# mode of the connection from Logic2 into Logic1 is decided by more than one
# link and may change in the middle of the over.
def _pri(name, members, prefix, sources):
    return LinkSpec(name, members, prefix=prefix, default_active=True,
                    audio_mode="PRIORITY", priority_sources=sources)


def _lnk(name, members, prefix, mode):
    return LinkSpec(name, members, prefix=prefix, default_active=True,
                    audio_mode=mode)


SCENARIOS = {
      # The talker is the priority source / only a sink of the PRIORITY link
    "prisrc_talker": dict(links=[_pri("L", ["Logic1", "Logic2"], "91",
                                      ["Logic2"])]),
    "prisrc_sink": dict(links=[_pri("L", ["Logic1", "Logic2"], "91",
                                    ["Logic1"])]),
      # PRIORITY over MIX on the same pair; the PRIORITY link goes away
    "pri_over_mix": dict(links=[_pri("P", ["Logic1", "Logic2"], "91",
                                     ["Logic2"]),
                                _lnk("M", ["Logic1", "Logic2"], "92", "MIX")]),
      # PRIORITY over FIRST on the same pair: deactivating the PRIORITY link
      # moves the connection from the mixer to the selector mid-over
    "pri_over_first": dict(links=[_pri("P", ["Logic1", "Logic2"], "91",
                                       ["Logic2"]),
                                  _lnk("F", ["Logic1", "Logic2"], "92",
                                       "FIRST")]),
      # Logic2 only reaches Logic1 via Logic3 (PRIORITY + MIX chain); the MIX
      # link into Logic1 goes away
    "pri_mix_chain": dict(num_logics=3,
                          links=[_pri("P", ["Logic2", "Logic3"], "92",
                                      ["Logic2"]),
                                 _lnk("M", ["Logic3", "Logic1"], "91",
                                      "MIX")]),
      # The same, but the PRIORITY link at the talker's end goes away
    "pri_mix_chain_remote": dict(num_logics=3,
                                 links=[_pri("P", ["Logic2", "Logic3"], "92",
                                             ["Logic2"]),
                                        _lnk("M", ["Logic3", "Logic1"], "91",
                                             "MIX")],
                                 dtmf=("Logic3", "92#")),
}


def _make_scenario(name, action):
    def t():
        _run(name, action, **SCENARIOS[name])
    t.__name__ = f"test_{name}_{action}"
    return t


TESTS += [_make_scenario(n, a) for n in SCENARIOS
          for a in ("none", "deactivate", "module")
          if not (n == "pri_mix_chain_remote" and a != "deactivate")]


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
