# SvxLink virtual-hardware tests

Integration tests that run the real `svxlink` binary against generated
configurations using only virtual hardware — no sound card, no GPIO, no kernel
modules — so they work on any Linux machine and in CI.

## Requirements

* The `svxlink` binary and the logic-core plugins must be built:

  ```sh
  cmake -S src -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build --target svxlink SimplexLogic RepeaterLogic \
        ReflectorLogic ReflectorV2Logic -j"$(nproc)"
  ```

  The harness looks for `build/bin/svxlink` and `build/lib/*Logic.so` relative
  to the repository root.

* Python 3 (standard library only — no third-party packages).

## How it works

`harness.py` launches `svxlink` against a generated config in a temp dir and
maps every piece of "hardware" onto something a test driver can control:

| Hardware | Virtualised as |
| --- | --- |
| RX audio in | `TYPE=Local`, `AUDIO_DEV=udp:127.0.0.1:<port>` — driver *sends* PCM |
| TX audio out | `TYPE=Local`, `AUDIO_DEV=udp:127.0.0.1:<port>` — driver *receives* PCM |
| Squelch | `SQL_DET=VOX` — streaming a tone opens it, silence closes it |
| DTMF / commands | `DTMF_CTRL_PTY` — driver writes digits (no audio needed) |
| PTT / TX state | a logic's TX UDP port emits packets only while transmitting |
| Event scripts | the repo's `src/svxlink/svxlink/*.tcl`, symlinked into the
  installed `events.tcl` + `events.d/` layout, so the current working tree is
  exercised |

A logic that transmits emits UDP packets on its TX port; an idle logic emits
none. Short announcements key the transmitter even without sound packs because
the announcement procs include `playSilence`/tones; where a specific clip is
needed the harness writes a throwaway clip with `add_sound_clip()`.

The UDP TX/RX sockets carry raw 16 kHz signed-16-bit mono PCM; the
measurement helpers are `harness.start_talk` / `stop_talk`,
`harness.tone_level`, and `harness.goertzel_mag`.
