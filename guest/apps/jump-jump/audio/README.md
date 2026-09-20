# Jump Jump sound

`sfx.json` is the sole source of sound parameters. Its `effects` section defines
Host Tone feedback; its App-specific `recordings` section defines the independently synthesized
compressed tracks. The generic `micropixel package` pipeline checks the Tone
profiles and generates `jump-jump_sfx_profiles.hpp` under `build/`.

`land` is the reference effect, with an elevated target for device audibility.
Takeoff adds no separate thump; center hits climb through eight
pitches, saturating at the last one and resetting after a broken streak. Shop
rewards use a two-note door chime, cube rewards use three short mechanical
clicks, drains play filtered water/bubbling noise, and failure uses a descending
phrase. These are approximations, not the original recordings. There is no App-wide volume adjustment.

Charge plays short ascending triangle-wave notes with a shallow high-register
taper so later notes remain audible when judging jump strength. The four charge
profiles contain one sequence; the hold profile repeats its final pitch.
The 10 ms audio timer schedules the generated
profiles independently of drawing. A late event skips stale notes instead of
bursting them together. Only two charge voices overlap.

Release and Cancel stop future scheduling and let the current notes finish
their sample-level release envelopes (at most 110 ms). They do not call
StopAll, destroy compressed playback mid-wave, or add a low takeoff tone.
Failure, restart and session shutdown still clear all voices. These rules keep
input timing independent of sound decay.

The record platform starts an independently synthesized music-box arrangement
of the traditional carol “We Wish You a Merry Christmas” (about 30 seconds)
when its idle bonus triggers. The composition is traditional/public domain;
no third-party recording is embedded. Music stops when charging starts, following the classic input behavior; system
pause/resume retains its position. The renderer receives actual playback state:
while the record plays, two notes float upwards and fade in a staggered cycle
and highlights rotate on the disc. Completion, charging and restart stop the
animation. Record/drain handles are explicitly released on interruption.

Regenerate the compressed source assets after changing recording parameters:

```sh
python3 guest/apps/jump-jump/audio/build_tracks.py
python3 guest/apps/jump-jump/audio/build_tracks.py --check
python3 -m unittest tools.tests.test_analyze_sfx -v
```

This requires ffmpeg with libopus. The generator measures the actual rendered
PCM with the shared analyzer's peak, transient, high-frequency and momentary
constraints, and refuses export on failure. It writes Ogg Opus source assets
to `assets/`; WAV intermediates and measurements stay in `build/`. Runtime
uses generated AssetIds and unity per-playback gain. A Bundle validates the
compressed asset envelopes through the normal resource pipeline.

For S31 listening, compare ordinary landing with Snake at identical system
volume, including 1%, 5%, 10% and normal listening level. Check a full charge,
holds beyond the pitch-rise window, music-box dwell, music stopping on charge,
takeoff interruption, input cancellation and rapid restart. Digital checks
cannot establish speaker loudness or subjective comfort; device A/B listening
and reference-game calibration remain necessary.

## Reference and fidelity

The [WeChat team’s February 2018 explanation](https://www.sohu.com/a/220552605_455313)
confirms Christmas music for the record and a shop entrance sound with quiet
voices. [Contemporary gameplay reporting](https://www.9game.cn/news/2080227.html)
identifies the carol; a [first-hand account](https://zhongce.sina.com.cn/iframe/article/view/6581/)
describes ascending charge notes used to judge distance. These establish the
event categories, not exact waveforms, pitch tables or timing. Our note table,
arrangement, envelope and mix remain tunable approximations. The shop currently
omits background speech. Original-recording A/B comparison is still required
before claiming an exact match.

A [third-party December 2017 archive](https://github.com/JohnWong/WeChat-minigame-hack/blob/7323e544d6fa/%E8%B7%B3%E4%B8%80%E8%B7%B3/game.js)
also records the 2-second dwell, stopping block music on press, and two staggered
floating-note animations. Its charge recordings were used only as local analysis
references to correct register, harmonic character and high-note decay; they are
not shipped with the App. This archive is not an official current release.
