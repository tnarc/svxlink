#!/usr/bin/env python3
"""Functional tests for deferring scheduled announcements until the first
silence (SCHEDULED_ANNOUNCEMENT_DEFER / SCHEDULED_ANNOUNCEMENT_MAX_DELAY).

A scheduled announcement triggered while the channel is busy with an active
transmission must be held and replayed at the first silence (when the squelch
closes), rather than played immediately. When it is finally played it must key
the CTCSS tone even though scheduled announcements are normally tone-less, so
that listeners filtering on CTCSS still hear the cut-in. If the channel never
goes idle within SCHEDULED_ANNOUNCEMENT_MAX_DELAY, the announcement interrupts
the active traffic and plays anyway (with CTCSS).

How the behaviour is observed
-----------------------------
* "Busy" is simulated by streaming a tone into the receiver (VOX squelch
  opens). To let the squelch close again the receiver is fed silence (a UDP
  receiver that simply stops getting packets never sees its level drop, so the
  squelch would otherwise stay open forever).
* The announcement is triggered squelch-independently via the logic's
  COMMAND_PTY: an ``EVENT`` command runs the TCL event immediately, whereas a
  DTMF command would be queued until the squelch closes.
* The only reliable thing to measure in the captured TX audio is the CTCSS
  tone (the voice-band announcement audio is attenuated by the TX path). That
  is exactly the feature under test: a deferred announcement is the one that
  keys CTCSS. So CTCSS presence is used as the "played, and audible to CTCSS
  users" signal, and its absence as "held / ordinary tone-less scheduled".
* The interrupt-after-max-delay case is only observable on a repeater, which
  transmits while its receiver squelch is open; a simplex logic will not key
  its transmitter while receiving, so it can never truly cut in.

Run directly:  python3 tests/test_scheduled_defer.py     (exit 0 = pass)
"""

import math
import os
import socket
import struct
import sys
import time
import traceback

from harness import SvxlinkHarness, LinkSpec, goertzel_mag, SAMPLE_RATE

CTCSS_FQ = 100      # sub-audible CTCSS tone frequency (Hz)
ANNOUNCE_FQ = 600   # frequency of the generated announcement clip (Hz)
TALK_FQ = 1500      # frequency streamed into the RX to hold the squelch open

# Magnitude thresholds for the CTCSS tone in captured TX audio (calibrated:
# a keyed announcement measures ~8, a tone-less one ~0.5).
CTCSS_PRESENT = 4.0
CTCSS_ABSENT = 2.0
TRANSMITTED_SAMPLES = 8000   # a played announcement keys TX for ~1 s @ 16 kHz

# A 1 s announcement clip; long enough that the keyed TX (hence the CTCSS tone)
# is easy to detect across a multi-second capture window.
ANNOUNCE_CLIP = [int(8000 * math.sin(2 * math.pi * ANNOUNCE_FQ * i / SAMPLE_RATE))
                 for i in range(SAMPLE_RATE)]

# The scheduled announcement under test, defined as a custom event so it can be
# fired on demand via the COMMAND_PTY while the channel is busy. ${::logic_name}
# expands at source time to the logic the override file belongs to.
SCHED_EVENT_TCL = """
proc ::${::logic_name}::sched_announce {} {
  scheduledAnnouncement {
    playMsg "unknown_command"
  }
}
"""


def _assert(cond, msg):
    if not cond:
        raise AssertionError(msg)


def _build(logic_type, defer, max_delay=None):
    """Build and start a harness whose Logic1 plays a scheduled announcement on
    the 'sched_announce' event, with the defer settings under test. Returns
    (harness, command_pty_fd)."""
    opts = {"TX_CTCSS": "ANNOUNCEMENT"}
    if defer:
        opts["SCHEDULED_ANNOUNCEMENT_DEFER"] = "1"
    if max_delay is not None:
        opts["SCHEDULED_ANNOUNCEMENT_MAX_DELAY"] = str(max_delay)
    h = SvxlinkHarness(
        num_logics=2,
        logic_type=logic_type,
        links=[LinkSpec("L", ["Logic1", "Logic2"], prefix="91",
                        default_active=False)],
        logic_opts=opts,
        tx_opts={"CTCSS_FQ": str(CTCSS_FQ), "CTCSS_LEVEL": "-6"},
        local_event_tcl={"Logic1.tcl": SCHED_EVENT_TCL},
    )
    cmd_path = os.path.join(h.tmp, "Logic1_cmd")
    h.per_logic_opts = {"Logic1": {"COMMAND_PTY": cmd_path}}
    h.setup()
    h.add_sound_clip("en_US", "Core", "unknown_command", samples=ANNOUNCE_CLIP)
    h.start()
    return h, os.open(cmd_path, os.O_WRONLY)


def _trigger(cmd_fd):
    """Fire the scheduled announcement on Logic1 via its COMMAND_PTY."""
    os.write(cmd_fd, b"EVENT sched_announce\n")


def _capture(h, logic_name, window, action=None, hush=False):
    """Drain pending TX, optionally run `action`, then capture TX PCM
    continuously for `window` seconds. If `hush`, stream silence into the RX
    throughout so the VOX squelch closes (and stays closed). Returns
    (sample_count, ctcss_magnitude)."""
    l = h.by_name[logic_name]
    h._drain(l)
    ssock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    chunk = SAMPLE_RATE // 10
    silence = struct.pack(f"<{chunk}h", *([0] * chunk))
    if action is not None:
        action()
    data = bytearray()
    end = time.time() + window
    while time.time() < end:
        if hush:
            ssock.sendto(silence, ("127.0.0.1", l.rx_port))
        try:
            while True:
                data += l.tx_sock.recv(65536)
        except (BlockingIOError, OSError):
            pass
        time.sleep(0.05)
    ssock.close()
    n = len(data) // 2
    pcm = list(struct.unpack(f"<{n}h", bytes(data[:n * 2])))
    return n, goertzel_mag(pcm, CTCSS_FQ, SAMPLE_RATE)


def test_defer_holds_then_replays_with_ctcss():
    """Simplex: a scheduled announcement fired while busy is held silent during
    the traffic, then replayed at the first silence WITH the CTCSS tone keyed
    (forced on, even though an ordinary scheduled announcement is tone-less)."""
    h, fd = _build("Simplex", defer=True)
    try:
        h.start_talk("Logic1", TALK_FQ)        # busy channel (squelch open)
        time.sleep(0.6)                        # let VOX open
        busy_n, busy_ct = _capture(h, "Logic1", 1.5,
                                   action=lambda: _trigger(fd))
        h.stop_talk("Logic1")
        replay_n, replay_ct = _capture(h, "Logic1", 2.5, hush=True)

        # Control: the same scheduled announcement fired while idle plays at
        # once and stays tone-less (forcing is specific to the deferred path).
        idle_n, idle_ct = _capture(h, "Logic1", 2.5,
                                   action=lambda: _trigger(fd), hush=True)

        _assert(busy_n < TRANSMITTED_SAMPLES and busy_ct < CTCSS_ABSENT,
                f"announcement must be held while busy "
                f"(samples={busy_n}, ctcss={busy_ct:.1f})")
        _assert(replay_n > TRANSMITTED_SAMPLES,
                f"announcement should transmit once idle, got {replay_n} samples")
        _assert(replay_ct > CTCSS_PRESENT,
                f"deferred announcement should key CTCSS, got {replay_ct:.1f}")
        _assert(idle_n > TRANSMITTED_SAMPLES,
                f"idle control should transmit, got {idle_n} samples")
        _assert(idle_ct < CTCSS_ABSENT,
                f"ordinary scheduled announcement must stay tone-less, "
                f"got {idle_ct:.1f}")
        _assert(replay_ct > 4 * idle_ct,
                f"CTCSS clearly stronger on the deferred replay "
                f"(replay={replay_ct:.1f}, idle={idle_ct:.1f})")
    finally:
        os.close(fd)
        h.cleanup()


def test_interrupts_busy_channel_after_max_delay():
    """Repeater: with a max delay set, a deferred announcement cuts in while the
    channel is still busy once the delay elapses, keying CTCSS so everyone
    hears it (a repeater transmits while its squelch is open)."""
    h, fd = _build("Repeater", defer=True, max_delay=1)
    try:
        h.start_talk("Logic1", TALK_FQ)
        time.sleep(0.6)
        # Keep the channel busy for the whole window, well past the 1 s delay.
        n, ct = _capture(h, "Logic1", 3.0, action=lambda: _trigger(fd))
        h.stop_talk("Logic1")
        _assert(n > TRANSMITTED_SAMPLES,
                f"announcement should interrupt and transmit, got {n} samples")
        _assert(ct > CTCSS_PRESENT,
                f"interrupting announcement should key CTCSS, got {ct:.1f}")
    finally:
        os.close(fd)
        h.cleanup()


def test_holds_indefinitely_without_max_delay():
    """Repeater control: without a max delay, a deferred announcement keeps
    waiting while the channel stays busy and never cuts in."""
    h, fd = _build("Repeater", defer=True)
    try:
        h.start_talk("Logic1", TALK_FQ)
        time.sleep(0.6)
        n, ct = _capture(h, "Logic1", 3.0, action=lambda: _trigger(fd))
        h.stop_talk("Logic1")
        _assert(ct < CTCSS_ABSENT,
                f"announcement must stay held while busy (no max delay), "
                f"got ctcss={ct:.1f}")
    finally:
        os.close(fd)
        h.cleanup()


TESTS = [
    test_defer_holds_then_replays_with_ctcss,
    test_interrupts_busy_channel_after_max_delay,
    test_holds_indefinitely_without_max_delay,
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
