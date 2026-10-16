#!/usr/bin/env python3
"""Tier-2 audio-level tests for the non-upstream LinkManager audio modes:
AUDIO_MODE = FIRST / MIX / DUCK / PRIORITY, plus PRIORITY_HANGTIME.

Each test links logics with a given mode, streams distinct sine tones into
sources (which opens their VOX squelch), captures a listener logic's TX audio
over UDP, and measures the per-tone level with a Goertzel filter.

Topology note: logics are Simplex with MUTE_RX_ON_TX/MUTE_TX_ON_RX disabled so a
logic can relay link audio while its own squelch is open. A Simplex transmitter
only carries link audio, so it unkeys when the link audio stops/mutes; tests are
therefore written to measure steady-state gain (full vs reduced) while the sink
is kept keyed, not the transient after a source stops. See README "Tier 2".

Run:  python3 tests/test_audio_modes.py     (exit 0 = pass)
"""

import math
import sys
import traceback
from contextlib import contextmanager

import os

import harness
from harness import SvxlinkHarness, LinkSpec, SAMPLE_RATE

F1, F2, FLOCAL = 1000, 1600, 800     # distinct, non-harmonic test tones
NO_MUTE = {"MUTE_RX_ON_TX": "0", "MUTE_TX_ON_RX": "0"}


@contextmanager
def _harness(links):
    h = SvxlinkHarness(num_logics=3, logic_type="Simplex", links=links,
                       logic_opts=NO_MUTE)
    h.setup()
    h.start()
    try:
        yield h
    finally:
        h.cleanup()


def _assert(cond, msg):
    if not cond:
        raise AssertionError(msg)


def _db(ratio):
    return 20.0 * math.log10(ratio) if ratio > 0 else float("-inf")


def test_mix_carries_all_sources():
    """MIX: a listener hears every simultaneous source."""
    links = [LinkSpec("L", ["Logic1", "Logic2", "Logic3"],
                      default_active=True, audio_mode="MIX")]
    with _harness(links) as h:
        h.start_talk("Logic1", F1)
        h.start_talk("Logic2", F2)
        import time
        time.sleep(0.8)
        f1 = h.tone_level("Logic3", F1)
        f2 = h.tone_level("Logic3", F2)
        _assert(f1 > 500 and f2 > 500,
                f"MIX listener should carry BOTH tones, got f1={f1:.0f} f2={f2:.0f}")


def test_first_carries_only_the_first_source():
    """FIRST: a second source is ignored while the first is active."""
    import time
    links = [LinkSpec("L", ["Logic1", "Logic2", "Logic3"],
                      default_active=True, audio_mode="FIRST")]
    with _harness(links) as h:
        h.start_talk("Logic1", F1)       # first talker wins
        time.sleep(0.6)
        h.start_talk("Logic2", F2)       # arrives second -> ignored
        time.sleep(0.6)
        f1 = h.tone_level("Logic3", F1)
        f2 = h.tone_level("Logic3", F2)
        _assert(f1 > 500, f"FIRST listener should carry the first tone, got f1={f1:.0f}")
        _assert(f2 < 150, f"FIRST listener must NOT carry the second tone, got f2={f2:.0f}")


def test_duck_reduces_incoming_when_local_squelch_opens():
    """DUCK: incoming link audio drops by ~DUCK_LEVEL_DB while the sink's own
    squelch is open."""
    import time
    links = [LinkSpec("L", ["Logic1", "Logic2", "Logic3"], default_active=True,
                      audio_mode="DUCK", duck_level_db=-12)]
    with _harness(links) as h:
        h.start_talk("Logic2", F2)               # remote station on the link
        time.sleep(1.0)
        full = h.tone_level("Logic1", F2, 1.2)   # Logic1 squelch closed
        h.start_talk("Logic1", FLOCAL)           # local traffic opens Logic1 squelch
        time.sleep(1.0)
        ducked = h.tone_level("Logic1", F2, 1.2)
        _assert(full > 800, f"closed-squelch link audio should be full, got {full:.0f}")
        _assert(ducked > 0, f"link audio should still be audible (ducked), got {ducked:.0f}")
        reduction = _db(ducked / full)
        _assert(-16 < reduction < -8,
                f"DUCK should reduce by ~-12 dB, measured {reduction:.1f} dB")


def test_priority_mutes_nonpriority_sources():
    """PRIORITY: while a PRIORITY-link source transmits, a normal source is
    reduced by ~PRIORITY_MUTE_DB at a shared listener; the priority source is
    full."""
    import time
    links = [
        LinkSpec("Pri", ["Logic1", "Logic3"], prefix="91", default_active=True,
                 audio_mode="PRIORITY", priority_mute_db=-20),
        LinkSpec("Norm", ["Logic2", "Logic3"], prefix="92", default_active=True,
                 audio_mode="MIX"),
    ]
    with _harness(links) as h:
        h.start_talk("Logic2", F2)               # normal traffic
        time.sleep(1.0)
        before = h.tone_level("Logic3", F2)
        h.start_talk("Logic1", F1)               # priority source keys up
        time.sleep(1.0)
        muted = h.tone_level("Logic3", F2)
        pri = h.tone_level("Logic3", F1)
        _assert(before > 800, f"normal traffic should be full pre-priority, got {before:.0f}")
        _assert(pri > 500, f"priority source should be audible, got {pri:.0f}")
        reduction = _db(muted / before)
        _assert(-26 < reduction < -14,
                f"PRIORITY should mute normal by ~-20 dB, measured {reduction:.1f} dB")


def test_priority_with_hangtime_still_preempts():
    """PRIORITY with PRIORITY_HANGTIME configured still preempts normal traffic.
    (The release-after-hangtime timing is not asserted here: a Simplex sink
    unkeys once the link audio stops, so the hangtime window can't be observed
    through captured TX audio. Hangtime release is covered by code review.)"""
    import time
    links = [
        LinkSpec("Pri", ["Logic1", "Logic3"], prefix="91", default_active=True,
                 audio_mode="PRIORITY", priority_mute_db=-20, priority_hangtime=2000),
        LinkSpec("Norm", ["Logic2", "Logic3"], prefix="92", default_active=True,
                 audio_mode="MIX"),
    ]
    with _harness(links) as h:
        h.start_talk("Logic2", F2)
        time.sleep(1.0)
        before = h.tone_level("Logic3", F2)
        h.start_talk("Logic1", F1)
        time.sleep(1.0)
        muted = h.tone_level("Logic3", F2)
        _assert(before > 800, f"normal traffic should be full, got {before:.0f}")
        _assert(_db(muted / before) < -14,
                f"PRIORITY+hangtime should still mute normal traffic, "
                f"measured {_db(muted / before):.1f} dB")


def _close_squelch(h, logic, freq, timeout=8.0):
    """Drive the logic's VOX squelch closed by streaming near-silence until the
    log reports the close (a Local UDP RX never re-evaluates VOX to silence
    once streaming just stops)."""
    import time
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
        raise RuntimeError(f"squelch for {logic} did not close within {timeout}s")
    finally:
        s.stop()


def test_duck_not_stuck_after_repeater_taken_down_with_carrier():
    """A repeater taken down while its squelch is open must still report the
    squelch close to the link layer. Otherwise DUCK keeps ducking the link
    audio into it after it is brought back, as if it were still receiving."""
    import time
    links = [LinkSpec("L", ["Logic1", "Logic2"], default_active=True,
                      audio_mode="DUCK", duck_level_db=-20)]
    h = SvxlinkHarness(num_logics=2, logic_type="Repeater", links=links,
                       logic_opts=dict(NO_MUTE, OPEN_ON_SQL="200",
                                       OPEN_SQL_FLANK="OPEN",
                                       IDLE_TIMEOUT="1"))
    cmd_pty = os.path.join(h.tmp, "logic1_cmd")
    h.per_logic_opts = {"Logic1": {"COMMAND_PTY": cmd_pty}}
    h.setup()
    h.start()
    try:
        h.start_talk("Logic1", FLOCAL)       # local carrier brings Logic1 up
        time.sleep(1.5)
        fd = os.open(cmd_pty, os.O_WRONLY)
        try:
            os.write(fd, b"CFG Logic1 ONLINE 0\n")   # down, carrier present
            time.sleep(0.5)
            h.stop_talk("Logic1")
            _close_squelch(h, "Logic1", FLOCAL)        # carrier drops
            os.write(fd, b"CFG Logic1 ONLINE 1\n")   # back online
            time.sleep(0.5)
        finally:
            os.close(fd)

        h.start_talk("Logic2", F2)           # remote station on the link
        time.sleep(1.0)
        level = h.tone_level("Logic1", F2, 1.2)
        h.stop_talk("Logic2")
        _assert(level > 800,
                f"link audio into Logic1 must not be ducked any more after its "
                f"squelch closed, got {level:.0f}")
    finally:
        h.cleanup()


TESTS = [
    test_duck_not_stuck_after_repeater_taken_down_with_carrier,
    test_mix_carries_all_sources,
    test_first_carries_only_the_first_source,
    test_duck_reduces_incoming_when_local_squelch_opens,
    test_priority_mutes_nonpriority_sources,
    test_priority_with_hangtime_still_preempts,
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
