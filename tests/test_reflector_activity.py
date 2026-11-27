#!/usr/bin/env python3
"""A reflector logic reports its incoming talker audio to the logic linking
layer, so the DUCK and PRIORITY link audio modes act on traffic from the
reflector just like on the squelch of a Simplex or Repeater logic.

A local svxreflector is started on the loopback interface with two nodes:

  * Node A, the node under test: Simplex logics Logic1 and Logic2 and a
    reflector logic Refl. Link Pri (AUDIO_MODE=PRIORITY,
    PRIORITY_SOURCES=Refl) joins Refl and Logic1, link Norm (MIX) joins
    Logic1 and Logic2. Logic1 is the listener.
  * Node B, the remote talker: Simplex logic Logic1 linked to its own
    reflector logic. A tone into its receiver is sent to the reflector.

While a remote talker is heard from the reflector, the normal traffic from
Logic2 into Logic1 must be reduced by PRIORITY_MUTE_DB, and it must be
restored when the remote talker stops, or when the reflector connection is
lost in the middle of the over. Both the Reflector (protocol V3) and the
ReflectorV2 logic core types are covered.

Run directly:  python3 tests/test_reflector_activity.py   (exit 0 = pass,
77 = skipped since svxreflector or the reflector logics are not built)
"""

import math
import os
import shutil
import subprocess
import sys
import tempfile
import time
import traceback

import harness
from harness import SvxlinkHarness, LinkSpec, SAMPLE_RATE, REPO

SVXREFLECTOR_BIN = os.path.join(REPO, "build", "bin", "svxreflector")
NO_MUTE = {"MUTE_RX_ON_TX": "0", "MUTE_TX_ON_RX": "0"}
F_REMOTE, F_NORMAL = 1000, 1600
MUTE_DB = -20
AUTH_KEY = "test-auth-key"


def _assert(cond, msg):
    if not cond:
        raise AssertionError(msg)


def _db(ratio):
    return 20.0 * math.log10(ratio) if ratio > 0 else float("-inf")


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
        raise RuntimeError(f"squelch for {logic} did not close")
    finally:
        s.stop()


class Reflector:
    """A svxreflector on the loopback interface accepting two nodes."""

    def __init__(self):
        self.tmp = tempfile.mkdtemp(prefix="svxrefl_")
        self.port = harness._free_udp_port()
        self.cfg_path = os.path.join(self.tmp, "svxreflector.conf")
        self.log_path = os.path.join(self.tmp, "svxreflector.log")
        self.proc = None
        self.logfile = None
        with open(self.cfg_path, "w") as f:
            f.write("\n".join([
                "[GLOBAL]",
                f"LISTEN_PORT={self.port}",
                "TG_FOR_V1_CLIENTS=999",
                f"CERT_PKI_DIR={os.path.join(self.tmp, 'pki')}",
                "[ROOT_CA]",
                "[ISSUING_CA]",
                "[SERVER_CERT]",
                "COMMON_NAME=localhost",
                "SUBJECT_ALT_NAME=IP:127.0.0.1,DNS:localhost",
                "[USERS]",
                "SM0AAA=Nodes",
                "SM0BBB=Nodes",
                "[PASSWORDS]",
                f'Nodes="{AUTH_KEY}"',
                ""]))

    def start(self):
        self.logfile = open(self.log_path, "wb")
        self.proc = subprocess.Popen(
            [SVXREFLECTOR_BIN, f"--config={self.cfg_path}"],
            stdout=self.logfile, stderr=subprocess.STDOUT,
            env=dict(os.environ, HOME=self.tmp))
        deadline = time.time() + 30
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("svxreflector exited early\n" +
                                   self.read_log())
            if "Starting main application" in self.read_log():
                return
            time.sleep(0.1)
        raise RuntimeError("svxreflector not ready\n" + self.read_log())

    def read_log(self):
        try:
            with open(self.log_path, "r", errors="replace") as f:
                return f.read()
        except OSError:
            return "<no log>"

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        if self.logfile:
            self.logfile.close()
            self.logfile = None

    def cleanup(self):
        self.stop()
        shutil.rmtree(self.tmp, ignore_errors=True)


def _node(refl, refl_type, callsign, links, num_logics):
    h = SvxlinkHarness(num_logics=num_logics, links=links, logic_opts=NO_MUTE,
                       extra_logics=["Refl"])
    h.extra_sections["Refl"] = [
        f"TYPE={refl_type}",
        "HOSTS=127.0.0.1",
        f"HOST_PORT={refl.port}",
        f"CALLSIGN={callsign}",
        f'AUTH_KEY="{AUTH_KEY}"',
        f"CERT_PKI_DIR={os.path.join(refl.tmp, 'pki_' + callsign)}",
        "DEFAULT_TG=1",
        "MONITOR_TGS=1",
        "MUTE_FIRST_TX_LOC=0",
        "MUTE_FIRST_TX_REM=0",
        f"EVENT_HANDLER={os.path.join(h.share_dir, 'events.tcl')}",
    ]
    h.setup()
    h.start()
    return h


def _wait_log(h, text, timeout=20.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if text in h.read_log():
            return
        time.sleep(0.1)
    raise RuntimeError(f"'{text}' not seen in the log\n" + h.read_log())


def _run(refl_type, end_of_over):
    refl = Reflector()
    a = b = None
    try:
        refl.start()
        b = _node(refl, refl_type, "SM0BBB",
                  [LinkSpec("Bridge", ["Logic1", "Refl"])], 1)
        a = _node(refl, refl_type, "SM0AAA", [
            LinkSpec("Pri", ["Refl", "Logic1"], prefix="91",
                     audio_mode="PRIORITY", priority_mute_db=MUTE_DB,
                     priority_sources=["Refl"]),
            LinkSpec("Norm", ["Logic1", "Logic2"], prefix="92",
                     audio_mode="MIX"),
        ], 2)
        _wait_log(a, "Refl: Connected nodes")
        _wait_log(b, "Refl: Connected nodes")
        time.sleep(1.0)

            # Normal traffic into the listener while the reflector is quiet.
            # It also reaches the reflector, so end it before the remote
            # talker starts.
        a.start_talk("Logic2", F_NORMAL)
        time.sleep(1.0)
        before = a.tone_level("Logic1", F_NORMAL)
        a.stop_talk("Logic2")
        _close_squelch(a, "Logic2", F_NORMAL)
        time.sleep(1.5)
        _assert(before > 800,
                f"{refl_type}: normal traffic should be heard, got "
                f"{before:.0f}")

            # A remote talker on the reflector, then normal traffic again
        b.start_talk("Logic1", F_REMOTE)
        time.sleep(1.5)
        remote = a.tone_level("Logic1", F_REMOTE)
        _assert(remote > 500,
                f"{refl_type}: the remote talker should be heard, got "
                f"{remote:.0f}")
        a.start_talk("Logic2", F_NORMAL)
        time.sleep(1.0)
        during = a.tone_level("Logic1", F_NORMAL)
        reduction = _db(during / before)
        _assert(MUTE_DB - 6 < reduction < MUTE_DB + 6,
                f"{refl_type}: the remote talker should pre-empt normal "
                f"traffic by ~{MUTE_DB} dB, measured {reduction:.1f} dB")

            # The remote over ends, or the reflector connection is lost
        if end_of_over == "talker_stop":
            b.stop_talk("Logic1")
            _close_squelch(b, "Logic1", F_REMOTE)
        else:
            refl.stop()
        time.sleep(1.5)
        after = a.tone_level("Logic1", F_NORMAL)
        _assert(_db(after / before) > -3,
                f"{refl_type}/{end_of_over}: normal traffic should be "
                f"restored, measured {_db(after / before):.1f} dB")
    finally:
        for h in (a, b):
            if h is not None:
                h.cleanup()
        refl.cleanup()


def _make(refl_type, end_of_over):
    def t():
        _run(refl_type, end_of_over)
    t.__name__ = f"test_{refl_type.lower()}_{end_of_over}"
    return t


TESTS = [_make(t, e) for t in ("Reflector", "ReflectorV2")
         for e in ("talker_stop", "disconnect")]


def main():
    needed = [SVXREFLECTOR_BIN] + [
        os.path.join(REPO, "build", "lib", f"{t}Logic.so")
        for t in ("Simplex", "Reflector", "ReflectorV2")]
    missing = [p for p in needed if not os.path.exists(p)]
    if missing:
        print("SKIP: not built: " + ", ".join(missing))
        return 77
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
