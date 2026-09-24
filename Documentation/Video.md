# Video

The engine plays WebM video (VP8 and VP9) with its Opus soundtrack. Everything
video-related lives in the **Video** project (`Source/Video`, built as `Binaries\Video.dll`):
decoding, import, the editor's Video Player window and the full-screen layer the Node Graph
drives.

## Importing

The Asset Browser has a single **Import...** button (and **Import Here...** in the folder
context menus). It takes any mix of files in one multi-select and routes each by extension:

| Kind | Extensions | Importer |
| --- | --- | --- |
| Geometry | `.fbx` | System.dll, as before |
| Textures | `.png .jpg .jpeg .tga .dds .bmp .hdr` | System.dll, as before |
| Videos | `.webm .mp4 .m4v .mov .mkv .avi .wmv .mpg .mpeg .flv .ogv .3gp .ts .mts .m2ts .gif` | `VideoImport` |

All of them run on a background thread behind the import progress window, which shows how
far a video conversion has got and has a **Cancel** button. Cancel stops a conversion in
progress and skips whatever is still queued; a texture or FBX already being processed
finishes first.

A video becomes `<name>.webm` in the target folder, replacing an existing file of that name:

- A `.webm` that the engine can already play (VP8/VP9, 8-bit) is **copied untouched**, so a
  file prepared elsewhere never loses a generation of quality.
- Anything else is converted with `Binaries\ffmpeg\bin\ffmpeg.exe`: VP9 at constant quality
  (`-crf 31`), 8-bit 4:2:0, a key frame at least every 120 frames so seeking stays quick,
  and the first audio track as Opus 128 kb/s. 10/12-bit VP9 WebM files are converted too,
  because the decoder is built without high bit depth.

ffmpeg writes to `<name>.webm.importing` and the file is renamed into place only when it is
complete, so a cancelled or failed import never leaves a truncated video behind. ffmpeg runs
in a job object that kills it if the editor exits mid-import.

## Previewing in the editor

Double-click a `.webm` in the Asset Browser (or use **Play Video** in the toolbar or the
context menu; video files are tagged `[Video]` in the list). It opens the **Video Player**,
a separate window with:

| Control | Does |
| --- | --- |
| Play / Pause button, click on the picture, **Space** | Toggles playback. |
| Stop button | Pauses and rewinds to the first frame. |
| **Loop** toggle, **L** | Loops at the end (on by default). |
| Volume button, **M** | Mutes. The button shows the level, or "No audio" for a silent clip. |
| Mouse wheel, **Up** / **Down** | Volume in 5% steps. |
| Timeline | Click or drag to scrub. |
| **Left** / **Right** | Skip 5 s back / forward. |
| **Home** | Rewind. |
| **Open...**, **Ctrl+O** | Plays another file. |
| **Esc** | Closes the window. |

The window can also be opened empty from **Windows → Video Player...**.

The window runs on a thread of its own in Video.dll with its own message loop, so it plays
smoothly whatever the editor's frame rate is, and it never touches Qt or the renderer. It
paints with GDI. Opening another video from the Asset Browser reuses the window.

## Playing in the game

The Node Graph's **Video** category drives one full-screen video layer - see the node table
in `Documentation/NodeGraph.md`. A typical intro:

```
On Game Start -> Play Video (Video: "Intro", Loop: false, Fit: Letterbox)
On Video Finished -> Show UI Document ("menu.rml")
```

The layer is drawn over the scene and under the game UI, after frame generation has
captured the scene, so interpolation never smears it. Its clock is the game's frame time,
so a paused or stalled game pauses the video rather than letting it skip ahead. Stopping
play mode stops the video.

## How it works

| File | Role |
| --- | --- |
| `WebmDemuxer` | Reads the first VP8/VP9 track through libwebm's `mkvparser` (with a `_wfopen` reader, so non-ANSI paths work). Measures the frame rate from timestamps when the file does not state it. |
| `VpxDecoder` | libvpx decoder, multithreaded (tile and row threads), converting to BGRA with BT.601/BT.709 and limited/full range as the stream declares (unlabelled HD is treated as BT.709). |
| `VideoPlayer` | Decodes on a worker thread a few frames ahead of a playhead the owner advances with `Update(dt)`. Seeks land on the exact frame (decode from the key frame, show the frame covering the target). Looping is seamless: the worker wraps to the start on its own, so the next pass is already buffered. |
| `VideoAudio` | The Opus soundtrack: libopus on its own thread, played through FMOD, kept in step with the playhead (below). |
| `VideoTexture` | Streams frames into a D3D12 texture and draws it letterboxed / filled / stretched. |
| `VideoLayer` | Player + texture + visibility, the thing the renderer owns and the graph drives. |
| `VideoImport` | The import rules above. |
| `VideoPlayerWindow` | The editor preview window. |

The renderer side is small: `DX12SceneRenderer` owns a `VideoLayer`, updates it before the
graph ticks and forwards the host calls; `DX12RendererAPI` allocates its descriptors and
records it in the viewport pass.

`VideoTexture` is written around the engine's three frames in flight: uploads go through a
ring of four UPLOAD buffers, a texture replaced because the video size changed is released
four frames later, and its SRV slots (four, allocated once at startup because the shared
heap never frees) rotate on replacement instead of being rewritten while an earlier frame
may still read them.

Measured on the development machine in the Debug build: a 1920x1080 VP9 stream plays in
real time with no dropped frames, and decodes at roughly 65 fps unthrottled.

## Sound

The Opus track of the same `.webm` is decoded with libopus on its own thread and played
through FMOD. It is a second FMOD **core** system, separate from the game's FMOD Studio
one: video audio has no events, banks or 3D positioning, and the two coexist happily. It
follows the video rather than the other way round, because the video's clock is the game's:

- The stream handed to FMOD is a user stream whose timeline *is* media time, so a seek is
  just a position change and FMOD's reported position can be compared with the playhead
  directly.
- Every frame, the offset between the two is measured. Under 120 ms it is corrected by
  nudging the playback rate by up to 1%, which is far too small to hear; a loop point, a
  scrub or a long stall exceeds that and the sound is repositioned instead.
- If the decoder is ever starved, the silence it played is subtracted from the audio that
  follows, so an underrun cannot leave the sound permanently behind the picture.
- Measured with the test clip: the sound stays within about 20 ms of the picture, with no
  underruns or repositions during straight playback.

Anything but stereo is mixed down to stereo (multichannel Opus uses the Vorbis channel
order, so 5.1 is FL, C, FR, RL, RR, LFE); FMOD then maps that to whatever the output device
is. Volume is a linear 0..1 gain: **Play Video** takes one, **Set Video Volume** changes it,
and the preview window has its own control. A video whose Opus track is missing or
unplayable still plays - silently - and `VideoPlayer::HasAudio()` reports it.

`VideoPlayer::GetAudioStatus()` exposes the sound's position, how much has been decoded,
underruns and repositions, which is what the headless tests assert on.

## Building libvpx and libopus

libvpx normally builds through `configure && make` with an x86 assembler (nasm or yasm).
Neither is part of the engine's toolchain, so the Video project compiles libvpx's C sources
directly with MSBuild instead:

- `ThirdParty/libvpx_config/` holds what `configure`/`make` would have generated:
  `vpx_config.h/.c`, `vpx_version.h` and the run-time CPU dispatch headers
  (`vp8_rtcd.h`, `vp9_rtcd.h`, `vpx_dsp_rtcd.h`, `vpx_scale_rtcd.h`).
- The build treats `HAVE_X86_ASM` as off. The rtcd definitions gate every hand-written
  `.asm` routine on that flag, so what remains are the C versions plus the SSE2 / SSSE3 /
  SSE4.1 / AVX2 **intrinsics** versions, chosen at run time by CPU detection.
- `ThirdParty/libvpx_x64_asm_replacements.c` stands in for the two x64 `.asm` helpers
  libvpx still references (`vpx_clear_system_state`, an `EMMS` that has nothing to clear in a
  build without MMX code). `vpx_encoder.c`, which needs the other, is left out: this is a
  decoder-only build.
- The project compiles with `/O2` even in Debug. A VP9 decoder at `/Od` cannot keep up with
  1080p, and Debug|x64 is the configuration the engine runs. It keeps the release CRT, like
  every other project, because `std::string` crosses the DLL boundary.

To update libvpx or change its configuration, run its `configure` in a scratch directory and
regenerate (needs `node`, plus `perl` and `bash`, which Git for Windows provides):

```bash
mkdir /tmp/vpxcfg && cd /tmp/vpxcfg
/k/Ptero-Engine/Source/SDKs/libvpx-1.17.0/configure --target=x86_64-win64-vs17 --as=yasm \
  --disable-vp8-encoder --disable-vp9-encoder --disable-examples --disable-tools --disable-docs \
  --disable-unit-tests --enable-multithread --enable-runtime-cpu-detect --disable-avx512 \
  --disable-postproc --disable-vp9-postproc --disable-webm-io --disable-libyuv
node /k/Ptero-Engine/Source/Video/ThirdParty/GenerateLibvpxConfig.js /tmp/vpxcfg
```

(`--as=yasm` only stops configure looking for an assembler; none is ever run.) The script
rewrites `libvpx_config/` and `libvpx_sources.txt`; the `ClCompile` list in `Video.vcxproj`
has to be brought in line with the latter by hand.

libopus needs no configure step. `ThirdParty/GenerateOpusSources.js` reads its
`*_sources.mk` and writes `opus_sources.txt`: the portable C float build of CELT, SILK and
the Opus layer, with no SIMD and no DNN, which is ample for decoding a soundtrack. The
build defines `OPUS_BUILD`, `USE_ALLOCA`, `HAVE_LRINT`, `HAVE_LRINTF` and `PACKAGE_VERSION`
in place of libopus's generated `config.h`.
