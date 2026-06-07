#!/usr/bin/env python3
"""Tier-2 audio-level tests for the non-upstream LinkManager audio modes:
AUDIO_MODE = FIRST / MIX.

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

from harness import SvxlinkHarness, LinkSpec

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


TESTS = [
    test_mix_carries_all_sources,
    test_first_carries_only_the_first_source,
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
