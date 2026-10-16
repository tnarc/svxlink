#!/usr/bin/env python3
"""Integration test for transparent cross-logic module-command forwarding
(the FORWARD_MODULE_CMD feature).

A real shared module (ModuleParrot, used as a network-free stand-in for the
intended EchoLink deployment) lives on a single dedicated target logic. Two
band-port logics carry no module of their own; each declares

    FORWARD_MODULE_CMD=1:Logic3

so that dialling the module command id "1" opens a forwarding session to the
target logic, whose own command processing activates and drives the real
module. All three logics are bridged by a default-active link.

The tests assert:
  * dialling the forwarded id on a band port activates the module on the
    TARGET logic (observed via the target's published ModuleFwd:state event
    and the target keying TX for the activation announcement);
  * a second band port dialling the id while the module is in use gets a
    "busy" response (command_failed announcement) and does NOT re-activate;
  * deactivating from the owning port frees the module for every port (the
    target publishes ModuleFwd:state ... 0 and a second port can then take it);
  * a forwarding session is not left open when the target does not activate
    the module (e.g. another module is already active there), or when the
    link to the target is dropped in the middle of a session, so the band
    port never gets stuck forwarding all its DTMF to the target.

Run directly:  python3 tests/test_forward_module_cmd.py   (exit 0 = pass)
"""

import os
import sys
import time
import traceback

from harness import SvxlinkHarness, LinkSpec, REPO

MODULE_ID = "1"          # ModuleParrot's default ID
HELP_ID = "0"            # ModuleHelp's default ID
TARGET = "Logic3"        # the logic that actually hosts the module


def _assert(cond, msg):
    if not cond:
        raise AssertionError(msg)


def _module_tcl(module_dir, name):
    path = os.path.join(REPO, "src", "svxlink", "modules", module_dir,
                        f"{name}.tcl")
    with open(path) as f:
        return f.read()


def _parrot_tcl():
    """The Parrot module's event handlers, taken straight from the module
    source so the harness's symlink farm (which only covers the svxlink core
    TCL) can still load the module's event script (placed in events.d/local/).
    """
    path = os.path.join(REPO, "src", "svxlink", "modules", "parrot",
                        "Parrot.tcl")
    with open(path) as f:
        return f.read()


def _make_harness(with_help=False):
    """Three Simplex logics in one default-active link. Logic3 hosts
    ModuleParrot; Logic1 and Logic2 are band ports that forward id 1 to it.
    With with_help, Logic3 also hosts ModuleHelp (id 0), which is not
    forwarded and can be activated locally on Logic3 to keep it busy."""
    extra = {
        "ModuleParrot": ["NAME=Parrot", f"ID={MODULE_ID}", "TIMEOUT=60",
                         "FIFO_LEN=60", "REPEAT_DELAY=0"],
    }
    tcl = {"Parrot.tcl": _parrot_tcl()}
    modules = "ModuleParrot"
    if with_help:
        extra["ModuleHelp"] = ["NAME=Help", f"ID={HELP_ID}", "TIMEOUT=60"]
        tcl["Help.tcl"] = _module_tcl("help", "Help")
        modules = "ModuleHelp,ModuleParrot"
    h = SvxlinkHarness(
        num_logics=3,
        logic_type="Simplex",
        links=[LinkSpec("Bridge", ["Logic1", "Logic2", "Logic3"],
                        default_active=True)],
        per_logic_opts={
            "Logic1": {"FORWARD_MODULE_CMD": f"{MODULE_ID}:{TARGET}"},
            "Logic2": {"FORWARD_MODULE_CMD": f"{MODULE_ID}:{TARGET}"},
            "Logic3": {"MODULES": modules},
        },
        # Modules are loaded from MODULE_PATH; point it at the build tree.
        global_opts={"MODULE_PATH": os.path.join(REPO, "build", "lib")},
        # ModuleParrot config section (referenced by MODULES=ModuleParrot).
        extra_sections=extra,
        # The module's own event script is not part of the core TCL tree the
        # harness symlinks, so provide it as a local override.
        local_event_tcl=tcl,
    )
    h.setup()
    # Sound clips so the module/announcements actually key the transmitter.
    h.add_sound_clip("en_US", "Parrot", "activating_module")
    h.add_sound_clip("en_US", "Parrot", "deactivating_module")
    h.add_sound_clip("en_US", "Core", "activating_module")
    h.add_sound_clip("en_US", "Core", "deactivating_module")
    # The busy response is the command_failed event: spellWord <cmd> + the
    # "operation_failed" Core clip. Provide both so it actually keys TX.
    h.add_sound_clip("en_US", "Default", f"phonetic_{MODULE_ID}")
    h.add_sound_clip("en_US", "Core", "operation_failed")
    h.start()
    # Attach a reader to the target's STATE_PTY straight away. A PTY drops
    # anything written before a reader is attached, so the published
    # ModuleFwd:state events would otherwise be lost.
    h.read_state(TARGET)
    return h


def _module_active(h):
    """Latest known busy state of the forwarded module, from the target's
    published ModuleFwd:state events. Returns True/False/None(unknown)."""
    state = h.read_state(TARGET)
    last = None
    for line in state.splitlines():
        if "ModuleFwd:state" in line:
            parts = line.split()
            # "<ts> ModuleFwd:state <id> <active>"
            if len(parts) >= 4 and parts[-2] == MODULE_ID:
                last = (parts[-1] == "1")
    return last


def test_forward_activates_module_on_target():
    """Dialling the forwarded id on a band port activates the module on the
    TARGET logic (not on the band port)."""
    h = _make_harness()
    try:
        h.read_state(TARGET)            # discard any startup state
        counts = h.count_tx_after(lambda: h.send_dtmf("Logic1", MODULE_ID + "#"),
                                  window=3.0)
        active = _module_active(h)
        _assert(active is True,
                f"target should report the module active, state seen={active}")
        _assert(counts[TARGET] > 0,
                f"target should key TX for the activation announcement, "
                f"got {counts}")
    finally:
        h.cleanup()


def test_second_port_gets_busy():
    """While the module is in use (owned by Logic1), Logic2 dialling the id
    gets a busy response and does not steal/re-activate the module."""
    h = _make_harness()
    try:
        h.read_state(TARGET)
        h.count_tx_after(lambda: h.send_dtmf("Logic1", MODULE_ID + "#"), window=2.5)
        _assert(_module_active(h) is True, "module should be active after L1")

        # Logic2 attempts the same id -> should be told busy (command_failed
        # announcement keys Logic2's TX) without a second activation.
        h.read_state(TARGET)            # clear; watch for NEW activations
        counts = h.count_tx_after(lambda: h.send_dtmf("Logic2", MODULE_ID + "#"),
                                  window=2.5)
        _assert(counts["Logic2"] > 0,
                f"Logic2 should key TX for the busy announcement, got {counts}")
        # No new activation event should have been published by the target in
        # response to Logic2 (it stays owned by Logic1 / already active).
        reactivated = "ModuleFwd:state" in h.read_state(TARGET) and \
            _module_active(h) is False
        _assert(not reactivated,
                "Logic2's busy attempt must not deactivate/re-cycle the module")
    finally:
        h.cleanup()


def test_deactivate_frees_for_all():
    """Deactivating from the owning port frees the module for every port: the
    target reports idle and a second port can then activate it."""
    h = _make_harness()
    try:
        h.read_state(TARGET)
        h.count_tx_after(lambda: h.send_dtmf("Logic1", MODULE_ID + "#"), window=2.5)
        _assert(_module_active(h) is True, "module should be active after L1")

        # Owning port deactivates (empty module command == '#'), forwarded to
        # the target which deactivates the real module.
        h.read_state(TARGET)
        h.count_tx_after(lambda: h.send_dtmf("Logic1", "#"), window=2.5)
        _assert(_module_active(h) is False,
                "target should report the module idle after deactivation")

        # Now a different port can take the freed module.
        h.read_state(TARGET)
        counts = h.count_tx_after(lambda: h.send_dtmf("Logic2", MODULE_ID + "#"),
                                  window=2.5)
        _assert(_module_active(h) is True,
                "Logic2 should be able to activate the freed module")
        _assert(counts[TARGET] > 0,
                f"target should key TX for Logic2's activation, got {counts}")
    finally:
        h.cleanup()


def _wait_log(h, text, timeout=5.0):
    end = time.time() + timeout
    while time.time() < end:
        if text in h.read_log():
            return True
        time.sleep(0.1)
    return False


def test_session_not_left_open_when_target_busy():
    """If the target does not activate the forwarded module because another
    module is already active there, the band port must not be left in a
    forwarding session."""
    h = _make_harness(with_help=True)
    try:
        # Keep the target busy with a module of its own
        h.count_tx_after(lambda: h.send_dtmf(TARGET, HELP_ID + "#"),
                         window=2.0)
        _assert(_wait_log(h, f"{TARGET}: Activating module Help"),
                "the Help module should be active on the target")

        h.count_tx_after(lambda: h.send_dtmf("Logic1", MODULE_ID + "#"),
                         window=2.5)
        _assert(_wait_log(h, "Logic1: Ending forwarding session to " + TARGET),
                "Logic1 must not stay in a forwarding session when the target "
                "did not activate the module")
    finally:
        h.cleanup()


def test_session_ends_when_link_dropped():
    """If the link between the band port and the target is dropped during a
    forwarding session, the band port must leave forwarding mode and process
    its DTMF locally again."""
    h = _make_harness()
    try:
        h.read_state(TARGET)
        h.count_tx_after(lambda: h.send_dtmf("Logic1", MODULE_ID + "#"),
                         window=2.5)
        _assert(_module_active(h) is True, "module should be active after L1")

        # Another port drops the link
        h.count_tx_after(lambda: h.send_dtmf("Logic2", "91#"), window=1.5)
        _assert(_wait_log(h, "Deactivating link Bridge"),
                "the link should have been deactivated")

        # The next digits on Logic1 must not be forwarded any more
        h.count_tx_after(lambda: h.send_dtmf("Logic1", "5#"), window=1.5)
        _assert(_wait_log(h, "Logic1: Ending forwarding session to " + TARGET),
                "Logic1 must leave forwarding mode once it is no longer "
                "linked to the target")
    finally:
        h.cleanup()


TESTS = [
    test_forward_activates_module_on_target,
    test_second_port_gets_busy,
    test_deactivate_frees_for_all,
    test_session_not_left_open_when_target_busy,
    test_session_ends_when_link_dropped,
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
