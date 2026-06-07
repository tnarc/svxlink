#!/usr/bin/env python3
"""Tier-1 behavioural tests for the "announce on all logics" feature.

Each test launches a fresh 3-logic SvxLink instance on virtual hardware and
asserts which ports transmit (detected by TX UDP-packet activity) in response
to a driven action.

The feature under test: announcements wrapped in the ``announceOnAllLogics``
TCL helper (link up/down, EchoLink enable, ...) must play on EVERY logic core,
while ordinary local announcements must stay on the originating logic only.

Run directly:  python3 tests/test_announce_all.py
Exit code 0 = all passed, 1 = a failure.
"""

import sys
import traceback
from contextlib import contextmanager

from harness import SvxlinkHarness

# Sound clips referenced by the announcements under test. Their presence makes
# the playMsg produce audio so the transmitter keys; the exact content is
# irrelevant (TX-keying is the observable).
LINK_CLIPS = ["activating_link_to", "deactivating_link_to", "link_not_active_to"]


@contextmanager
def _harness():
    h = SvxlinkHarness(num_logics=3)
    h.setup()
    for clip in LINK_CLIPS:
        h.add_sound_clip("en_US", "Core", clip)
    h.start()
    try:
        yield h
    finally:
        h.cleanup()


def _assert(cond, msg):
    if not cond:
        raise AssertionError(msg)


def test_link_up_broadcasts_to_all_ports():
    """Activating a link from one logic announces on every logic."""
    with _harness() as h:
        _assert(all(v == 0 for v in h.count_tx_after(window=1.0).values()),
                "ports should be idle before any action")
        counts = h.count_tx_after(lambda: h.send_dtmf("Logic1", "911#"))
        _assert(all(v > 0 for v in counts.values()),
                f"link-up should key ALL ports, got {counts}")


def test_link_down_broadcasts_to_all_ports():
    """Deactivating a link from one logic announces on every logic."""
    with _harness() as h:
        h.count_tx_after(lambda: h.send_dtmf("Logic1", "911#"))   # link up
        counts = h.count_tx_after(lambda: h.send_dtmf("Logic1", "91#"))  # down
        _assert(all(v > 0 for v in counts.values()),
                f"link-down should key ALL ports, got {counts}")


def test_excluded_logic_is_skipped_by_broadcast():
    """A logic with ANNOUNCE_ALL_EXCLUDE=1 is skipped by an announceOnAllLogics
    broadcast, while the originating logic and the other (non-excluded) logics
    still key."""
    h = SvxlinkHarness(num_logics=3,
                       per_logic_opts={"Logic2": {"ANNOUNCE_ALL_EXCLUDE": "1"}})
    h.setup()
    for clip in LINK_CLIPS:
        h.add_sound_clip("en_US", "Core", clip)
    h.start()
    try:
        counts = h.count_tx_after(lambda: h.send_dtmf("Logic1", "911#"))
        _assert(counts["Logic1"] > 0,
                f"originating port should key, got {counts}")
        _assert(counts["Logic3"] > 0,
                f"non-excluded port should receive the broadcast, got {counts}")
        _assert(counts["Logic2"] == 0,
                f"ANNOUNCE_ALL_EXCLUDE port must NOT key, got {counts}")
    finally:
        h.cleanup()


def test_local_announcement_stays_on_one_port():
    """Negative control: an announcement NOT wrapped in announceOnAllLogics
    (link_not_active, from deactivating an already-inactive link) keys only the
    originating logic. Guards against the broadcast firing for everything."""
    with _harness() as h:
        counts = h.count_tx_after(lambda: h.send_dtmf("Logic1", "91#"))
        _assert(counts["Logic1"] > 0,
                f"originating port should key, got {counts}")
        _assert(counts["Logic2"] == 0 and counts["Logic3"] == 0,
                f"other ports must NOT key for a local announcement, got {counts}")


TESTS = [
    test_link_up_broadcasts_to_all_ports,
    test_link_down_broadcasts_to_all_ports,
    test_excluded_logic_is_skipped_by_broadcast,
    test_local_announcement_stays_on_one_port,
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
