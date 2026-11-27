#!/usr/bin/env python3
"""Integration test for ANNOUNCE_ALL_REMOTE.

The EchoLink module broadcasts its (de)activation announcement to every logic
core (announceOnAllLogics in EchoLink.tcl). When the module activates itself
because a remote EchoLink station connects, and deactivates itself when the
station leaves, that broadcast must only happen when ANNOUNCE_ALL_REMOTE=1.
Otherwise anyone who can send one UDP packet from a directory-listed address
could make every transmitter key up.

Logic1 hosts ModuleEchoLink, Logic2 and Logic3 are unlinked ports. A fake
directory server on 127.0.0.1:5200 lists station SM0AAA at 127.0.0.1.

  * default: an incoming connection and its BYE key only Logic1;
  * ANNOUNCE_ALL_REMOTE=1: they key Logic2 and Logic3 as well;
  * default: activating the module with a DTMF command on Logic1 is still
    broadcast.

Run directly:  python3 tests/test_announce_all_remote.py   (exit 0 = pass)
"""

import os
import socket
import struct
import sys
import threading
import time
import traceback

from harness import SvxlinkHarness, REPO


def _assert(cond, msg):
    if not cond:
        raise AssertionError(msg)


def txt(s): s = s.encode() if isinstance(s, str) else s; return bytes([len(s)]) + s
def sdes(call, name):
    rr = bytes([3 << 6, 201, 0, 1, 0, 0, 0, 0])
    items = bytes([1]) + txt("CALLSIGN") + bytes([2]) + txt("%-15s%s" % (call, name)) + bytes([3]) + txt("CALLSIGN") + bytes([4]) + txt("08:30") + b"\0\0"
    body = bytes([0, 0, 0, 0]) + items
    while (len(body) + 4) % 4: body += b"\0"
    return rr + struct.pack(">HH", (3 << 14) | 202 | (1 << 8), (len(body) + 4) // 4 - 1) + body
def bye():
    rr = bytes([3 << 6, 201, 0, 1, 0, 0, 0, 0])
    body = bytes([0, 0, 0, 0]) + txt("jan2002")
    while (len(body) + 4) % 4: body += b"\0"
    return rr + struct.pack(">HH", (3 << 14) | 203 | (1 << 8), (len(body) + 4) // 4 - 1) + body

class FakeDirectory(threading.Thread):
    LIST = b"@@@\n1\nSM0AAA\nTester [ON 12:00]\n1000\n127.0.0.1\n+++"
    def __init__(self):
        super().__init__(daemon=True)
        self.srv = socket.socket(); self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", 5200)); self.srv.listen(8)
        self.requests = []
    def run(self):
        while True:
            try: c, _ = self.srv.accept()
            except OSError: return
            try:
                c.settimeout(3); req = c.recv(4096); self.requests.append(req[:1])
                c.sendall(self.LIST if req[:1] == b"s" else b"OK2.6\n")
            except OSError:
                pass
            finally:
                c.close()

def tcl(name):
    return open(os.path.join(REPO, "src", "svxlink", "modules", "echolink", name)).read()


DIRECTORY = None


def _make_harness(logic1_opts=None):
    opts = {"MODULES": "ModuleEchoLink"}
    opts.update(logic1_opts or {})
    h = SvxlinkHarness(num_logics=3, logic_type="Simplex", links=[],
        per_logic_opts={"Logic1": opts},
        global_opts={"MODULE_PATH": os.path.join(REPO, "build", "lib")},
        extra_sections={"ModuleEchoLink": [
            "NAME=EchoLink", "ID=2", "TIMEOUT=60", "SERVERS=127.0.0.1",
            "CALLSIGN=SM0XXX-L", "PASSWORD=pw", "SYSOPNAME=Test",
            "LOCATION=Somewhere", "DESCRIPTION=test", "MAX_QSOS=2",
            "MAX_CONNECTIONS=2", "LINK_IDLE_TIMEOUT=300",
            "BIND_ADDR=127.0.0.1"]},
        local_event_tcl={"EchoLink.tcl": tcl("EchoLink.tcl"),
                         "EchoLinkCommon.tcl": tcl("EchoLinkCommon.tcl")})
    h.setup()
    # the per-connection event script is looked for next to the core scripts
    with open(os.path.join(h.share_dir, "events.d", "EchoLinkRemote.tcl"),
              "w") as f:
        f.write(tcl("EchoLinkRemote.tcl"))
    for ctx in ("Core", "EchoLink", "Default"):
        for clip in ("activating_module", "deactivating_module", "activating",
                     "deactivating", "module", "name"):
            h.add_sound_clip("en_US", ctx, clip)
    h.start()
    end = time.time() + 30
    while (time.time() < end and
           "EchoLink directory status changed to ON" not in h.read_log()):
        time.sleep(0.3)
    _assert("status changed to ON" in h.read_log(),
            "the EchoLink module did not get online with the fake directory")
    time.sleep(3.0)                       # let the station list arrive
    return h


def _remote_connect_and_leave(h):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    try:
        c1 = h.count_tx_after(
            lambda: s.sendto(sdes("SM0AAA", "Tester"), ("127.0.0.1", 5199)),
            window=5.0)
        c2 = h.count_tx_after(lambda: s.sendto(bye(), ("127.0.0.1", 5199)),
                              window=8.0)
    finally:
        s.close()
    return c1, c2


def test_remote_activation_not_broadcast_by_default():
    h = _make_harness()
    try:
        c1, c2 = _remote_connect_and_leave(h)
        _assert(c1["Logic1"] > 0, f"the EchoLink logic did not announce: {c1}")
        others = c1["Logic2"] + c1["Logic3"] + c2["Logic2"] + c2["Logic3"]
        _assert(others == 0, "unlinked ports transmitted for a remote "
                f"connection: connect {c1}, bye {c2}")
        _assert("Not broadcasting an announcement" in h.read_log(),
                "the suppressed broadcast was not logged")
    finally:
        h.stop()
        h.cleanup()


def test_remote_activation_broadcast_when_enabled():
    h = _make_harness({"ANNOUNCE_ALL_REMOTE": "1"})
    try:
        c1, c2 = _remote_connect_and_leave(h)
        _assert(c1["Logic2"] > 0 and c1["Logic3"] > 0,
                f"ANNOUNCE_ALL_REMOTE=1 but no broadcast on connect: {c1}")
        _assert(c2["Logic2"] > 0 and c2["Logic3"] > 0,
                f"ANNOUNCE_ALL_REMOTE=1 but no broadcast on bye: {c2}")
    finally:
        h.stop()
        h.cleanup()


def test_dtmf_activation_still_broadcast():
    h = _make_harness()
    try:
        c = h.count_tx_after(lambda: h.send_dtmf("Logic1", "2#"), window=5.0)
        _assert(c["Logic2"] > 0 and c["Logic3"] > 0,
                f"a DTMF activation was not broadcast: {c}")
    finally:
        h.stop()
        h.cleanup()


def test_dtmf_deactivation_still_broadcast():
    """'#' makes the module deactivate itself (deactivateMe), but on a user's
    command, so it is broadcast like the activation."""
    h = _make_harness()
    try:
        h.count_tx_after(lambda: h.send_dtmf("Logic1", "2#"), window=5.0)
        c = h.count_tx_after(lambda: h.send_dtmf("Logic1", "#"), window=5.0)
        _assert(c["Logic2"] > 0 and c["Logic3"] > 0,
                f"a DTMF deactivation was not broadcast: {c}")
    finally:
        h.stop()
        h.cleanup()


TESTS = [
    test_remote_activation_not_broadcast_by_default,
    test_remote_activation_broadcast_when_enabled,
    test_dtmf_activation_still_broadcast,
    test_dtmf_deactivation_still_broadcast,
]


def main():
    global DIRECTORY
    DIRECTORY = FakeDirectory()
    DIRECTORY.start()
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
