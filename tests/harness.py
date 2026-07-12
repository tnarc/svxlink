"""Virtual-hardware test harness for SvxLink.

Launches the real ``svxlink`` binary against a generated configuration that uses
only virtual hardware, so behaviour can be exercised on any machine and in CI
with no sound card and no kernel modules:

  * RX  -> ``TYPE=Local`` with ``AUDIO_DEV=udp:...`` (the test driver sends PCM)
  * TX  -> ``TYPE=Local`` with ``AUDIO_DEV=udp:...`` (the driver captures PCM);
           presence of TX packets means the logic is transmitting.
  * Squelch -> ``SQL_DET=VOX``; a station "talks" by streaming a tone into the
               RX (``start_talk``/``stop_talk``/``set_squelch``), which opens
               the VOX squelch; stopping the tone closes it.
  * Commands -> ``DTMF_CTRL_PTY`` (write digits to inject DTMF, no audio needed)
  * State -> ``STATE_PTY`` (read published state events)

The event-handler TCL is taken straight from the repository source via a
symlink farm laid out the way ``make install`` would (``events.tcl`` plus an
``events.d`` directory), so the harness always exercises the current working
tree.

Pure standard library; no third-party dependencies.
"""

import math
import os
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time

# Repository layout ----------------------------------------------------------
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SVXLINK_BIN = os.path.join(REPO, "build", "bin", "svxlink")
TCL_SRC_DIR = os.path.join(REPO, "src", "svxlink", "svxlink")

SAMPLE_RATE = 16000


def _free_udp_port():
    """Return a currently-free UDP port number on localhost."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]
    finally:
        s.close()


class Logic:
    """Bookkeeping for one virtual logic core."""

    def __init__(self, name, tmp):
        self.name = name
        self.rx_port = _free_udp_port()        # svxlink binds this (RX inject)
        self.tx_port = _free_udp_port()        # driver binds this (TX capture)
        self.dtmf_pty = os.path.join(tmp, f"{name}_dtmf")
        self.state_pty = os.path.join(tmp, f"{name}_state")
        # The driver binds the TX capture socket up front; svxlink's TX side
        # only sends (never binds), so there is no conflict.
        self.tx_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.tx_sock.bind(("127.0.0.1", self.tx_port))
        self.tx_sock.setblocking(False)
        # Persistent write fd to the DTMF control PTY. Opening and closing a
        # PTY slave on every command hangs up the master, so the fd is opened
        # once (lazily, after svxlink creates the symlink) and reused.
        self.dtmf_fd = None

    def close(self):
        for fd in (self.dtmf_fd,):
            if fd is not None:
                try:
                    os.close(fd)
                except OSError:
                    pass
        try:
            self.tx_sock.close()
        except OSError:
            pass


class LinkSpec:
    """Describes one [LinkX] section. `members` are logic names; `prefix` is
    the per-logic DTMF command prefix used to control the link."""

    def __init__(self, name, members, prefix="91", default_active=True,
                 audio_mode=None, duck_level_db=None, priority_mute_db=None,
                 priority_hangtime=None):
        self.name = name
        self.members = members
        self.prefix = prefix
        self.default_active = default_active
        self.audio_mode = audio_mode
        self.duck_level_db = duck_level_db
        self.priority_mute_db = priority_mute_db
        self.priority_hangtime = priority_hangtime

    def render(self):
        connect = ",".join(f"{m}:{self.prefix}:{m}" for m in self.members)
        out = [f"[{self.name}]", f"CONNECT_LOGICS={connect}",
               f"DEFAULT_ACTIVE={1 if self.default_active else 0}", "TIMEOUT=0"]
        if self.audio_mode:
            out.append(f"AUDIO_MODE={self.audio_mode}")
        if self.duck_level_db is not None:
            out.append(f"DUCK_LEVEL_DB={self.duck_level_db}")
        if self.priority_mute_db is not None:
            out.append(f"PRIORITY_MUTE_DB={self.priority_mute_db}")
        if self.priority_hangtime is not None:
            out.append(f"PRIORITY_HANGTIME={self.priority_hangtime}")
        out.append("")
        return out


class _ToneStreamer:
    """Streams a continuous sine tone as int16 PCM to a UDP port, paced at the
    sample rate, to simulate a station talking into a receiver."""

    def __init__(self, port, freq, rate, amplitude):
        self.addr = ("127.0.0.1", port)
        self.freq = freq
        self.rate = rate
        self.amplitude = amplitude
        self._stop = threading.Event()
        self._thread = None

    def start(self):
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        chunk = self.rate // 10            # 100 ms per datagram
        phase = 0
        while not self._stop.is_set():
            buf = struct.pack(
                f"<{chunk}h",
                *[int(self.amplitude *
                      math.sin(2 * math.pi * self.freq * (phase + i) / self.rate))
                  for i in range(chunk)])
            phase += chunk
            try:
                sock.sendto(buf, self.addr)
            except OSError:
                break
            self._stop.wait(0.08)          # slightly faster than real time
        sock.close()

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2)


def goertzel_mag(samples, freq, rate):
    """Return the per-sample magnitude of `freq` in `samples` (Goertzel).
    Dependency-free; used to measure how loud a given tone is in captured TX
    audio without an FFT library."""
    n = len(samples)
    if n == 0:
        return 0.0
    k = round(n * freq / rate)
    w = 2.0 * math.pi * k / n
    coeff = 2.0 * math.cos(w)
    s1 = s2 = 0.0
    for x in samples:
        s0 = x + coeff * s1 - s2
        s2 = s1
        s1 = s0
    power = s1 * s1 + s2 * s2 - coeff * s1 * s2
    return math.sqrt(max(power, 0.0)) / n


class SvxlinkHarness:
    def __init__(self, num_logics=3, link_prefix="91", logic_type="Simplex",
                 links=None, logic_opts=None, per_logic_opts=None,
                 tx_opts=None, local_event_tcl=None, per_tx_opts=None,
                 extra_sections=None):
        self.num_logics = num_logics
        self.link_prefix = link_prefix
        self.logic_type = logic_type
        # Extra "KEY=VALUE" lines added to every logic section.
        self.logic_opts = logic_opts or {}
        # Extra "KEY=VALUE" lines added to one named logic section, e.g.
        # {"Logic2": {"ANNOUNCE_ALL_EXCLUDE": "1"}}.
        self.per_logic_opts = per_logic_opts or {}
        # Extra "KEY=VALUE" lines added to every transmitter section, e.g.
        # {"CTCSS_FQ": "100", "CTCSS_LEVEL": "-6"}.
        self.tx_opts = tx_opts or {}
        # Local event-script overrides, written to events.d/local/, e.g.
        # {"Logic2.tcl": "proc Logic::unknown_command {cmd} { ... }"}.
        self.local_event_tcl = local_event_tcl or {}
        # Extra "KEY=VALUE" lines added to one named transmitter section, e.g.
        # {"Logic2": {"CTCSS_PTT": "CtcssEnc2"}}.
        self.per_tx_opts = per_tx_opts or {}
        # Arbitrary extra config sections, e.g.
        # {"CtcssEnc2": ["PTT_TYPE=PTY", "PTT_PTY=/path"]}.
        self.extra_sections = extra_sections or {}
        self.tmp = tempfile.mkdtemp(prefix="svxtest_")
        self.logics = [Logic(f"Logic{i + 1}", self.tmp)
                       for i in range(num_logics)]
        self.by_name = {l.name: l for l in self.logics}
        # Default: a single inactive link covering all logics (Tier-1 style).
        if links is None:
            links = [LinkSpec("TestLink", [l.name for l in self.logics],
                              prefix=link_prefix, default_active=False)]
        self.links = links
        self.cfg_path = os.path.join(self.tmp, "svxlink.conf")
        self.log_path = os.path.join(self.tmp, "svxlink.log")
        self.share_dir = os.path.join(self.tmp, "share")
        self.proc = None
        self.logfile = None
        self._streamers = {}

    # -- setup ---------------------------------------------------------------
    def _build_event_tree(self):
        """Create <share>/events.tcl + <share>/events.d/*.tcl as symlinks to
        the repository source, matching the installed layout."""
        os.makedirs(self.share_dir, exist_ok=True)
        events_d = os.path.join(self.share_dir, "events.d")
        os.makedirs(events_d, exist_ok=True)
        os.symlink(os.path.join(TCL_SRC_DIR, "events.tcl"),
                   os.path.join(self.share_dir, "events.tcl"))
        for fn in os.listdir(TCL_SRC_DIR):
            if fn.endswith(".tcl") and fn != "events.tcl":
                os.symlink(os.path.join(TCL_SRC_DIR, fn),
                           os.path.join(events_d, fn))
        # Local event-script overrides (sourced after the base scripts).
        if self.local_event_tcl:
            local_d = os.path.join(events_d, "local")
            os.makedirs(local_d, exist_ok=True)
            for fn, content in self.local_event_tcl.items():
                with open(os.path.join(local_d, fn), "w") as f:
                    f.write(content)
        # A sounds tree the harness can populate with generated clips.
        self.sounds_dir = os.path.join(self.share_dir, "sounds")
        os.makedirs(self.sounds_dir, exist_ok=True)
        return os.path.join(self.share_dir, "events.tcl")

    def add_sound_clip(self, lang, context, name, samples=None):
        """Write a sound clip so a referenced playMsg produces audio. Default
        is 200 ms of low-level tone (enough to key TX). Raw 16 kHz s16le."""
        import struct
        import math
        d = os.path.join(self.sounds_dir, lang, context)
        os.makedirs(d, exist_ok=True)
        if samples is None:
            n = SAMPLE_RATE // 5
            samples = [int(8000 * math.sin(2 * math.pi * 600 * i / SAMPLE_RATE))
                       for i in range(n)]
        with open(os.path.join(d, f"{name}.raw"), "wb") as f:
            f.write(struct.pack(f"<{len(samples)}h", *samples))

    def _render_config(self, event_handler):
        lines = [
            "[GLOBAL]",
            "LOGICS=" + ",".join(l.name for l in self.logics),
            "LINKS=" + ",".join(lk.name for lk in self.links),
            f"CARD_SAMPLE_RATE={SAMPLE_RATE}",
            f"LOGIC_CORE_PATH={os.path.join(REPO, 'build', 'lib')}",
            "",
        ]
        for l in self.logics:
            rx, tx = f"Rx_{l.name}", f"Tx_{l.name}"
            lines += [
                f"[{rx}]",
                "TYPE=Local",
                f"AUDIO_DEV=udp:127.0.0.1:{l.rx_port}",
                "AUDIO_CHANNEL=0",
                # VOX squelch: opens when audio is streamed into the RX, closes
                # when it stops. Drives the squelch state used by DUCK/PRIORITY.
                "SQL_DET=VOX",
                "VOX_FILTER_DEPTH=1280",
                "VOX_THRESH=150",
                "SQL_HANGTIME=0",
                "",
                f"[{tx}]",
                "TYPE=Local",
                f"AUDIO_DEV=udp:127.0.0.1:{l.tx_port}",
                "AUDIO_CHANNEL=0",
                "PTT_TYPE=NONE",
                *[f"{k}={v}" for k, v in self.tx_opts.items()],
                *[f"{k}={v}"
                  for k, v in self.per_tx_opts.get(l.name, {}).items()],
                "",
                f"[{l.name}]",
                f"TYPE={self.logic_type}",
                f"RX={rx}",
                f"TX={tx}",
                f"CALLSIGN=TEST{l.name[-1]}",
                "SHORT_IDENT_INTERVAL=0",
                "LONG_IDENT_INTERVAL=0",
                f"EVENT_HANDLER={event_handler}",
                f"DTMF_CTRL_PTY={l.dtmf_pty}",
                f"STATE_PTY={l.state_pty}",
                "DEFAULT_LANG=en_US",
            ]
            lines += [f"{k}={v}" for k, v in self.logic_opts.items()]
            lines += [f"{k}={v}"
                      for k, v in self.per_logic_opts.get(l.name, {}).items()]
            lines.append("")
        for lk in self.links:
            lines += lk.render()
        for section, section_lines in self.extra_sections.items():
            lines += [f"[{section}]", *section_lines, ""]
        with open(self.cfg_path, "w") as f:
            f.write("\n".join(lines))

    def setup(self):
        if not os.path.exists(SVXLINK_BIN):
            raise RuntimeError(f"svxlink binary not found: {SVXLINK_BIN}")
        event_handler = self._build_event_tree()
        self._render_config(event_handler)

    # -- run -----------------------------------------------------------------
    def start(self, ready_timeout=20.0):
        self.logfile = open(self.log_path, "wb")
        env = dict(os.environ, HOME=self.tmp)
        self.proc = subprocess.Popen(
            [SVXLINK_BIN, f"--config={self.cfg_path}"],
            stdout=self.logfile, stderr=subprocess.STDOUT, env=env)
        # svxlink creates the control PTYs (as symlinks) during logic init;
        # readiness = all DTMF control PTYs exist.
        deadline = time.time() + ready_timeout
        needed = [l.dtmf_pty for l in self.logics]
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(
                    "svxlink exited early (rc=%s)\n%s"
                    % (self.proc.returncode, self.read_log()))
            if all(os.path.exists(p) for p in needed):
                time.sleep(1.0)   # let event scripts finish loading
                return
            time.sleep(0.1)
        raise RuntimeError("svxlink not ready in time\n" + self.read_log())

    def read_log(self):
        try:
            with open(self.log_path, "r", errors="replace") as f:
                return f.read()
        except OSError:
            return "<no log>"

    # -- drive ---------------------------------------------------------------
    def send_dtmf(self, logic_name, digits):
        l = self.by_name[logic_name]
        if l.dtmf_fd is None:
            l.dtmf_fd = os.open(l.dtmf_pty, os.O_WRONLY)
        os.write(l.dtmf_fd, digits.encode())

    def set_squelch(self, logic_name, is_open, freq=1000):
        """Open or close a logic's squelch by driving its VOX detector: stream
        a tone into the receiver (open) or stop it (close). The RX uses
        SQL_DET=VOX, so an open only takes effect once the VOX filter fills
        (tens of ms) -- allow a brief settle (e.g. time.sleep) before asserting
        on squelch-dependent behaviour. Thin wrapper over start_talk/stop_talk."""
        if is_open:
            self.start_talk(logic_name, freq)
        else:
            self.stop_talk(logic_name)

    # -- observe -------------------------------------------------------------
    def _drain(self, l):
        n = 0
        try:
            while True:
                l.tx_sock.recv(65536)
                n += 1
        except (BlockingIOError, OSError):
            pass
        return n

    def drain_all(self):
        """Discard any pending TX audio on all logics."""
        for l in self.logics:
            self._drain(l)

    def count_tx_after(self, action=None, window=3.0, poll=0.05):
        """Drain, run `action` (e.g. inject DTMF), then continuously poll the
        TX sockets for `window` seconds. Returns per-logic packet counts.
        Polling continuously (rather than draining once at the end) means a
        short announcement is captured whenever it arrives in the window."""
        self.drain_all()
        if action is not None:
            action()
        counts = {l.name: 0 for l in self.logics}
        end = time.time() + window
        while time.time() < end:
            for l in self.logics:
                counts[l.name] += self._drain(l)
            time.sleep(poll)
        return counts

    # -- audio (Tier 2) ------------------------------------------------------
    def start_talk(self, logic_name, freq, amplitude=10000):
        """Simulate a station talking into `logic_name`: stream a continuous
        `freq` Hz tone into its receiver, which opens the VOX squelch."""
        l = self.by_name[logic_name]
        s = _ToneStreamer(l.rx_port, freq, SAMPLE_RATE, amplitude)
        s.start()
        self._streamers[logic_name] = s

    def stop_talk(self, logic_name):
        """Stop the tone; the VOX squelch closes shortly after."""
        s = self._streamers.pop(logic_name, None)
        if s:
            s.stop()

    def capture_tx_pcm(self, logic_name, window=1.5):
        """Drain, then collect raw TX PCM from one logic for `window` seconds.
        Returns a list of int16 samples."""
        l = self.by_name[logic_name]
        self._drain(l)
        data = bytearray()
        end = time.time() + window
        while time.time() < end:
            try:
                while True:
                    data += l.tx_sock.recv(65536)
            except (BlockingIOError, OSError):
                pass
            time.sleep(0.02)
        n = len(data) // 2
        return list(struct.unpack(f"<{n}h", bytes(data[:n * 2])))

    def tone_level(self, logic_name, freq, window=1.5):
        """Capture a logic's TX and return the magnitude of `freq` in it."""
        samples = self.capture_tx_pcm(logic_name, window)
        return goertzel_mag(samples, freq, SAMPLE_RATE)

    # -- teardown ------------------------------------------------------------
    def stop(self):
        for name in list(self._streamers):
            self.stop_talk(name)
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        if self.logfile:
            self.logfile.close()
        for l in self.logics:
            l.close()

    def cleanup(self):
        self.stop()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def __enter__(self):
        self.setup()
        return self

    def __exit__(self, *exc):
        self.cleanup()
