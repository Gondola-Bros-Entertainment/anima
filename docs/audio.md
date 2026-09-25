# Native audio

Anima owns the PCM mixer, buffered WAV decoder, voice state, resampling, buses,
fades and stereo spatialization. These are part of dependency-free `anima::core`.
The optional `anima::audio_output` adapter uses SDL to send mixed samples to a device.
No audio middleware is used.

```cpp
#include <anima/audio.hpp>
#include <anima/audio_output.hpp>

anima::Audio audio; // 48 kHz stereo, up to 256 owned voices
auto effects = audio.bus();
effects.volume(.8F);
auto clip = anima::AudioClip::wav(wav_bytes);
auto sound = audio.sound(clip, effects);
sound.position({2, 0, 3});
sound.spatial(true);
sound.play();
anima::AudioOutput device(audio);

// On the application thread, regularly and after updating audio parameters:
device.pump();
```

Enable `ANIMA_BUILD_AUDIO_OUTPUT=ON` and link `anima::audio_output` for device
playback. It requires an SDL 3.2+ runtime package or the existing pinned
`ANIMA_FETCH_SDL` source configuration. Assets, desktop/Vulkan, physics, UI and
the SDL input converter are independently optional. The output option defaults
off; enabling desktop does not enable or link audio output. The desktop preset
explicitly enables both for local qualification. Public audio-output headers
contain no SDL types. Call `anima_copy_sdl_runtime(your_executable)` after linking
to copy a shared SDL runtime beside a Windows executable.

`AudioClip::pcm` accepts finite normalized mono or stereo float samples.
`AudioClip::wav` decodes little-endian RIFF/WAVE PCM16 and IEEE float32. Both
accept sample rates from 8 to 192 kHz and at most 32 million samples. WAV input
is bounded to 128 MiB; truncated chunks, duplicate format/data chunks, invalid
alignment/rates and unsupported encodings reject. Clips own immutable samples
and can be shared by independent voices. File IO remains with the caller; a WAV
importer can use the [asset reimport registry](asset-reimport.md).

`Sound` is move-only ownership of a voice. Destroying it stops that voice and
releases its slot. `play` resumes a paused voice or restarts a completed one;
`pause` preserves its cursor; `stop` rewinds and cancels a pending fade while
preserving its current gain. `seek` accepts seconds within the clip, including
the end. Loops interpolate across the last/first frame boundary. Pitch is a
positive playback-rate multiplier from 0.01 to 8; this also changes duration.

Gains range from zero to sixteen. Master, ancestor bus and voice gains multiply.
Muting a bus silences its descendants without stopping their clocks. A voice
retains its bus hierarchy even when application bus handles are released. Bus
parents are fixed at creation, must belong to the same mixer, and are limited to
16 levels. Voice capacity is fixed at mixer construction (1 to 4096); exhaustion
throws instead of stealing a live voice.

Mono panning uses equal-power left/right gains, with a -3 dB center. Nonspatial
stereo keeps its original channels at center and reduces the opposite channel
when panned. Spatial voices downmix to mono, pan relative to the listener's right
vector, and attenuate linearly between their minimum and maximum distances. The
default listener faces +Z with +Y up; reversing it reverses left/right. There is
no distance attenuation inside the minimum radius and silence beyond the maximum.
Positions and orientations update at the next rendered block. Listener orientation
rejects zero or parallel forward/up vectors.

`fade(target_gain, seconds)` changes voice gain linearly over rendered output
frames, independent of pitch. Pausing a voice pauses its fade. A direct volume
change cancels it. Rendering is synchronous and single-threaded: callers must
serialize voice/bus/listener changes and rendering. Mixing linearly resamples clips
to the output rate and hard-clips the final stereo result to [-1, 1]. It starts
no devices, callbacks or worker threads. Offline output uses the same
`render(span<float>)` path and is invariant under output-buffer partitioning on
the same platform. Cross-platform bitwise float determinism is not promised.

`AudioOutput` retains the mixer context and owns one SDL playback stream. Its
construction acquires its own SDL audio subsystem reference without initializing
video, creating a window or owning an event pump. Destruction releases that
reference and the stream, preserving other outputs and caller-owned SDL audio
references. The application must not call global `SDL_Quit` while an output lives.
Outputs are move-only; moved-from operations reject. Mixer wrapper movement or
destruction does not invalidate an output. Its
5–250 ms queue target defaults to 50 ms. `pump` fills at most the missing target
frames observed at entry; a full queue does not advance the mixer. SDL may do
final conversion to the hardware's format. Call `pump` frequently enough to
avoid underflow. Playback controls affect newly mixed samples, so already queued
audio adds latency. The mixer's cursor describes rendered audio, not an exact
hardware playhead. `paused` pauses the device; `clear` discards queued samples
without rewinding voices. A device submission error throws and may follow an
already advanced mixer block. Use one output/render driver per mixer.

This API currently covers buffered sound playback. Compressed streaming, effects
such as reverb, Doppler/HRTF, microphone capture, device-loss recovery policies and
editor preview are not implemented. Game sound selection and
gameplay authority belong to the application.

Verification includes offline sample-level comparisons for panning, resampling,
pitch, loops, nested bus gain/mute, fades, output limiting and lifetime/capacity;
WAV fixtures and truncation rejection; output-buffer partitioning; and an SDL
dummy-device test for queue bounds, clearing, moves, retained mixer lifetime and
SDL subsystem ownership. A copied independent audio-output consumer builds with
assets, desktop, input, physics and UI off and uses no SDL headers. Copied consumers
configure separate builds and do not inherit the parent build's sanitizer flags;
report their results separately from instrumented engine tests. The
dummy test produces no physical sound and does not establish hardware latency.

## Scene listeners and sources

`<anima/audio_scene.hpp>` in `anima::assets` adds an `AudioListener` marker and an
`AudioSource` that owns one mixer voice. These components need no SDL, Vulkan,
physics backend, device or game assets. Clip selection and playback decisions
remain application data. The copied independent consumer exercises this API.

```cpp
anima::Audio audio;
anima::Scene scene;
auto listener = scene.create("listener");
listener.add_component<anima::AudioListener>();
auto emitter = scene.create("emitter");
emitter.set_position({2, 0, 3});
anima::AudioSourceSettings settings;
settings.spatial = true;
settings.looping = true;
auto source = emitter.add_component<anima::AudioSource>(audio, clip, settings);
source->play();

// After application/Scene updates and before offline render or device.pump():
anima::synchronize_audio(scene, audio);
```

The application drives synchronization explicitly. It neither runs `Scene` phases
nor advances audio time. The same `synchronize_audio` function accepts a SceneSet
to validate and synchronize all its scenes together. Use one Scene or SceneSet
driver per mixer; standalone sounds may share it. Every selected source must
belong to that mixer, even if disabled. Entry during selected-scene callbacks or
construction rejects; set synchronization also rejects during set loading/retirement.
Sources retain the mixer context and clip, so destroying or moving an `Audio`
wrapper cannot leave dangling pointers. `Audio::owns` checks voice identity after
moves. Removing a source, destroying its object, or tearing down its scene releases
its voice and capacity. Checked component handles do not keep voices alive; a
currently executing component access retains its value until the expression ends.

One active listener supplies world position and transformed local +Z/+Y axes.
The axes are normalized; nonuniform scale is allowed, zero or parallel axes reject.
Spatial sources follow their world positions, including parents. Source scale does
not scale attenuation distances. More than one active listener across the selection rejects; none
resets the mixer to origin/+Z/+Y, rather than retaining a removed listener's pose.
Nonspatial sources do not use their transforms. The complete listener/source
snapshot is validated before publishing any listener, source pose or enablement
change. Invalid poses, ambiguous listeners and foreign mixers preserve the previous
mixer state. Bounds match the core mixer: finite positions/axes within 1e9 units.

Sources start stopped. `play()` requests playback at the next enabled synchronization;
`play_on_start` requests that once after construction/loading. Synchronizing again
does not restart completed one-shots. Disabling a source pauses it on the next
synchronization; re-enabling resumes only previously playing or requested playback.
`pause()` and `stop()` immediately cancel pending/resumed playback, including while
disabled. Stop rewinds; pause retains the cursor. Seek follows the core clip bounds.
`playing()` reports actual mixer state, excluding a pending play request. Queued SDL
samples still obey the device latency described above.

`configure` validates all source settings before applying them: volume, pitch, pan,
attenuation, looping and spatial mode use the core ranges. Configuration applies
immediately to the voice; synchronize before rendering after spatial/transform
changes. Changing `play_on_start` only changes the configuration used by future
persisted copies; use `play()` for the existing source. Component enablement is
separate from configuration. Inherited object activation and component enablement
jointly determine participation at `synchronize_audio`: inactive sources pause and
inactive listeners are excluded. Reactivation resumes only previously playing or
requested playback; authored flags and completed one-shots are preserved.

## Component persistence

Register the explicit codecs with caller-owned clip keys and resolvers:

```cpp
anima::ComponentCodecs codecs;
anima::add_audio_component_codecs(codecs, audio,
    name_clip, resolve_clip, effects_bus); // Bus is optional; defaults to master.
auto prefab = anima::Prefab::capture(emitter, codecs);
auto json = prefab.serialize({}); // No mesh resolver needed for these empty objects.
```

`anima.audio-listener.v1` stores an empty object. `anima.audio-source.v1` stores
exactly `clip`, `volume`, `pitch`, `pan`, `minimum_distance`, `maximum_distance`,
`looping`, `spatial` and `play_on_start`. The scene/prefab envelope stores component
enablement and object transforms. Payloads reject unknown/missing fields, wrong
types, invalid ranges, duplicate keys (including escaped duplicates), depth beyond
16 and input beyond 64 KiB. Clip keys must be nonempty, at most 4096 bytes and
contain no NUL; missing/null resolutions reject. Resolvers must not mutate scenes,
components or the mixer during decoding.

Persistence stores configuration, not game saves or a live audio checkpoint.
It excludes cursor, play/pause state, pending requests, mixer/bus gains and device
queues. Each restored source gets its own stopped voice at zero and shares the
resolved immutable clip. Its authored start request waits for synchronization and
enablement. All decoded sources use the caller's codec bus, allowing the same
prefab to be routed differently in another consumer. Bus hierarchy/settings are
not embedded in documents. Codecs retain their mixer context/bus by ownership,
without borrowing the lifetime or address of the original `Audio` wrapper.
Registration rejects missing callbacks and duplicate codecs without partial changes.

Malformed payloads, resolver failures and voice-capacity exhaustion roll back the
new scene/prefab objects and release their voices. Existing objects/voices remain
untouched. Clip replacement/reimport adoption is explicit: create a replacement
source from the accepted clip snapshot. Streaming, DSP, multiple-listener mixing,
device recovery and editor preview are
outside this bounded component contract.
