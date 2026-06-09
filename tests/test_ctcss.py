#!/usr/bin/env python3
"""Functional test for CTCSS on scheduled vs. ordinary announcements.

A scheduled announcement (one played from within the scheduledAnnouncement TCL
helper) must NOT key a CTCSS tone, while an ordinary announcement must, given a
logic configured with TX_CTCSS=ANNOUNCEMENT (and SCHEDULED omitted).

Two logics are run with identical CTCSS configuration and the same unknown-
command announcement is triggered on each. Logic2's unknown_command is
overridden (via a local event-script override) to play as a scheduled
announcement. The captured TX PCM is analysed with a Goertzel filter at the
CTCSS frequency: the ordinary announcement carries the sub-audible tone, the
scheduled one does not.

Run directly:  python3 tests/test_ctcss.py     (exit 0 = pass)
"""

import os
import sys
import time
import traceback

from harness import SvxlinkHarness, goertzel_mag, SAMPLE_RATE

CTCSS_FQ = 100      # sub-audible CTCSS tone frequency (Hz)
CMD = "55#"         # an unknown DTMF command -> the unknown_command announcement

# Local override: play this logic's unknown_command announcement as "scheduled"
# so it is classified separately for CTCSS. ${::logic_name} expands at source
# time to the logic the override file belongs to.
SCHEDULED_OVERRIDE = """
proc ::${::logic_name}::unknown_command {cmd} {
  scheduledAnnouncement {
    playMsg "unknown_command"
  }
}
"""


def _assert(cond, msg):
    if not cond:
        raise AssertionError(msg)


def _drain_fd(fd):
    """Read all currently-available bytes from a non-blocking fd."""
    out = b""
    try:
        while True:
            chunk = os.read(fd, 4096)
            if not chunk:
                break
            out += chunk
    except (BlockingIOError, OSError):
        pass
    return out


def _announce_ctcss(h, logic):
    """Trigger the unknown-command announcement on `logic`; return
    (CTCSS magnitude in the captured TX audio, number of samples captured)."""
    h.drain_all()
    h.send_dtmf(logic, CMD)
    pcm = h.capture_tx_pcm(logic, window=2.5)
    return goertzel_mag(pcm, CTCSS_FQ, SAMPLE_RATE), len(pcm)


def test_scheduled_announcement_suppresses_ctcss():
    """Ordinary announcement keys CTCSS; scheduled announcement does not."""
    h = SvxlinkHarness(
        num_logics=2,
        logic_opts={"TX_CTCSS": "ANNOUNCEMENT"},
        tx_opts={"CTCSS_FQ": str(CTCSS_FQ), "CTCSS_LEVEL": "-6"},
        local_event_tcl={"Logic2.tcl": SCHEDULED_OVERRIDE},
    )
    h.setup()
    h.add_sound_clip("en_US", "Core", "unknown_command")
    h.start()
    try:
        reg_ctcss, reg_n = _announce_ctcss(h, "Logic1")   # ordinary announcement
        sch_ctcss, sch_n = _announce_ctcss(h, "Logic2")   # scheduled announcement

        # Both announcements must actually have transmitted (keyed the TX).
        _assert(reg_n > 2000,
                f"ordinary announcement should transmit, got {reg_n} samples")
        _assert(sch_n > 2000,
                f"scheduled announcement should transmit, got {sch_n} samples")

        # The ordinary announcement carries CTCSS; the scheduled one does not.
        _assert(reg_ctcss > 12,
                f"ordinary announcement should carry CTCSS, got {reg_ctcss:.1f}")
        _assert(sch_ctcss < 8,
                f"scheduled announcement must NOT carry CTCSS, got {sch_ctcss:.1f}")
        _assert(reg_ctcss > 4 * sch_ctcss,
                f"CTCSS should be clearly stronger for the ordinary announcement "
                f"(ordinary={reg_ctcss:.1f}, scheduled={sch_ctcss:.1f})")
    finally:
        h.cleanup()


def test_scheduled_announcement_does_not_key_ctcss_encode_line():
    """The CTCSS encode-enable output line (CTCSS_PTT) is keyed for an ordinary
    announcement but not for a scheduled one.

    The encode line is configured as a PTT-style PTY output so the test can
    observe it without real GPIO hardware: PttPty writes 'T' when the line is
    keyed and 'R' when it is released."""
    h = SvxlinkHarness(
        num_logics=2,
        logic_opts={"TX_CTCSS": "ANNOUNCEMENT"},
        local_event_tcl={"Logic2.tcl": SCHEDULED_OVERRIDE},
    )
    pty1 = os.path.join(h.tmp, "ctcss_enc1")
    pty2 = os.path.join(h.tmp, "ctcss_enc2")
    h.per_tx_opts = {"Logic1": {"CTCSS_PTT": "CtcssEnc1"},
                     "Logic2": {"CTCSS_PTT": "CtcssEnc2"}}
    h.extra_sections = {"CtcssEnc1": ["PTT_TYPE=PTY", f"PTT_PTY={pty1}"],
                        "CtcssEnc2": ["PTT_TYPE=PTY", f"PTT_PTY={pty2}"]}
    h.setup()
    h.add_sound_clip("en_US", "Core", "unknown_command")
    h.start()
    try:
        fd1 = os.open(pty1, os.O_RDONLY | os.O_NONBLOCK)
        fd2 = os.open(pty2, os.O_RDONLY | os.O_NONBLOCK)
        _drain_fd(fd1)            # discard initial/startup line state
        _drain_fd(fd2)

        h.send_dtmf("Logic1", CMD)   # ordinary announcement -> should key CTCSS
        h.send_dtmf("Logic2", CMD)   # scheduled announcement -> should not
        time.sleep(2.5)

        reg = _drain_fd(fd1)
        sch = _drain_fd(fd2)
        os.close(fd1)
        os.close(fd2)

        _assert(b"T" in reg,
                f"ordinary announcement should key the CTCSS encode line, "
                f"got {reg!r}")
        _assert(b"T" not in sch,
                f"scheduled announcement must NOT key the CTCSS encode line, "
                f"got {sch!r}")
    finally:
        h.cleanup()


TESTS = [
    test_scheduled_announcement_suppresses_ctcss,
    test_scheduled_announcement_does_not_key_ctcss_encode_line,
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
