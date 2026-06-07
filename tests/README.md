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

## Running

```sh
python3 tests/test_audio_modes.py      # LinkManager AUDIO_MODE behaviour
```

Each test exits 0 on success, 1 on failure.

There is also a C++ unit test for `LinkManager` (built with the project):

```sh
cmake --build build --target LinkManagerTest
./build/svxlink/svxlink/LinkManagerTest
```

It drives the gain logic directly (fake logics, no audio) and asserts the
per-connection valve state and mixer gains for the audio modes.

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

## What is covered (audio-level)

`test_audio_modes.py` exercises the non-upstream `LinkManager` audio modes by
streaming distinct sine tones into source logics and measuring per-tone level
(via a Goertzel filter) in a listener's captured TX audio:

* **MIX** — a listener carries every simultaneous source (both tones present)
* **FIRST** — a second source is ignored while the first is active (one tone)
* **DUCK** — incoming link audio drops by ~`DUCK_LEVEL_DB` when the sink's own
  squelch opens (measured to ~-12 dB)
