# This fork

Timbeter's fork of librealsense. Everything here is ours; upstream has no
`fork/` directory, which is deliberate - new files never conflict on a merge,
so these notes survive every upstream merge untouched.

Consumed as an AAR by several projects: `timbeter-forest-android`,
`timbeter-3d-android`, `timbeter-lumber`, `timbeter-truck-android`. That is why
this lives here rather than in any one app's docs - it is the source of truth
for all of them.

Branch layout: `android` carries everything, including the GitHub Packages
publishing configuration. Branches intended for upstream are cut fresh from
`origin/development` and carry one topic each. See UPSTREAMING.md.

## Versioning

The artifact version is `<upstream>-<androidBuildNumber>`, currently
**2.58.1-1**. The upstream part is parsed out of
`include/librealsense2/rs.h` at configure time rather than typed into
`build.gradle`, so it cannot drift from the code after a merge - the same
source `CMake/version_config.cmake` reads. `androidBuildNumber` is the only
number to bump for an Android-side change on the same upstream.

Maven reads a trailing `-<number>` as a build number, so `2.58.1-1` sorts
*after* `2.58.1`, which a named qualifier would not.

The fork previously used its own unrelated numbering, which is why older notes
refer to versions like `2.4.0`. Those numbers say nothing about the upstream
they were built from, which is exactly why the scheme changed.

## Upstream 2.58.1: two build flags that must stay set

The fork was merged up from 2.54.2 to 2.58.1 on 2026-09-03, for the NEON align
implementation. Two settings in
`wrappers/android/librealsense/build.gradle` are load-bearing and must survive
future merges.

**`-DBUILD_ROSBAG2=OFF`.** Upstream added this option in 2.58.1 and it defaults
to **ON**, which silently breaks `.bag` recording. With it defined,
`create_writer_for_file` returns a `ros2_writer` unconditionally, ignoring the
file extension, and `ros2_writer`'s constructor throws
`"Output file must have .db3 extension"`. So every payload v1 recording fails
at record start.

Playback is unaffected - `create_reader_for_file` still routes anything that is
not `.db3` to the legacy `ros_reader` - which is what makes it easy to miss.

The flag is upstream's own compatibility switch, commented as "temporary flag,
should be removed when deprecated ROSBAG1 recording system is removed", and
`rs.cpp` already logs that ROS1 `.bag` is deprecated. So this is a deferral, not
a fix: eventually either the payload moves to `.db3` end to end, which is gated
on the CV worker's pyrealsense2 being new enough to read it, or the fork pins.

It lives in the Gradle CMake arguments rather than in
`CMake/android_config.cmake` deliberately: that file is byte-identical to
upstream, and keeping it that way means merges do not silently drop the
override.

**NEON is enabled by six removed guards.** Upstream compiles its NEON paths for
ARM64 but excludes Android, with no stated rationale. The exclusion is written
two different ways, which is easy to half-remove:

| file | form | what it gates |
| --- | --- | --- |
| `src/proc/align.cpp` | `&& !defined(ANDROID)` | align dispatch |
| `src/proc/neon/neon-align.cpp` | `&& !defined(ANDROID)` | align implementation |
| `src/proc/neon/neon-align.h` | `#ifndef ANDROID` | `align_neon` declaration |
| `src/proc/color-formats-converter.cpp` | `&& !defined(ANDROID)` | YUY2 dispatch |
| `src/proc/neon/image-neon.cpp` | `#ifndef ANDROID` | YUY2 implementation |
| `src/proc/neon/image-neon.h` | `#ifndef ANDROID` | YUY2 declarations |

Removing only the `!defined(ANDROID)` ones fails to compile: the dispatch finds
no `librealsense::align_neon`, because the declaration is behind the other
spelling.

Nothing else was needed. `__ARM_NEON` is defined automatically on `arm64-v8a`
because NEON is mandatory in ARMv8-A, and `BUILD_WITH_NEON` defaults ON and is
emitted as a define by `CMake/global_config.cmake`, which runs for Android.
`ANDROID` really was the only blocker.

`src/proc/neon/neon-pointcloud.cpp` and the CUDA pointcloud headers keep their
exclusion. Pointcloud is not on our frame path.

**Verifying it took.** The three log strings live in one `#if`/`#elif`/`#else`,
so only the selected branch survives compilation. In the shipped AAR:

```
strings jni/arm64-v8a/librealsense2.so | grep "align implementation"
```

should print only `Using NEON-optimized align implementation`, and the x86_64
library should print only the SSE one. At runtime the same line appears in
logcat under the tag `librs`, which is the quickest on-device check:

```
adb logcat -s librs | grep -i "align implementation"
```

**What it bought.** Measured on an ALGIZ RT10 (Snapdragon 480, Adreno 619),
recording a v2 payload at 10 kept fps from a 30 fps source:

| | extract mean | frame thread busy |
| --- | --- | --- |
| generic align | 50.3 - 58.1 ms | 519 - 579 ms/s |
| NEON | 10.6 - 14.7 ms | 97 - 157 ms/s |

Roughly 4x, taking the frame thread from oversubscribed to about 15% utilized.
Note that the align half of this lands in the app's own extract path, while the
YUY2 half lands in librealsense's internal thread and so does not appear in
these numbers at all - it shows up as power and thermal headroom instead.

## Open: NEON alignment is unverified

Deliberately deferred on 2026-09-04. Recorded so it is a decision rather than
an oversight.

We measured that NEON align is roughly 4x faster than the generic path. We have
not verified that its output matches. The realistic failure modes are narrow -
tail handling when the width is not a multiple of the vector width, rounding in
float to int conversion, occlusion tie-breaking - so the expected difference is
zero or a few pixels at depth discontinuities, not a wrong image. But
discontinuities are tree edges, and edges are where diameter comes from.

Two things lower the risk enough to ship and watch:

- The NEON path is not untested code. Its guard excludes only Android, so
  `align_neon` has been the live path on ARM64 **Linux** - Jetson, 64-bit
  Raspberry Pi - since 2024.
- `is_special_resolution()` in `neon-align.cpp` special-cases 640x240 to 320x180
  and 640x480 to 640x360. Neither is ours; at 1280x720 we take the general path.

**If field reports suggest depth or diameter is off**, this is the first thing
to check. The test, in order of cost:

1. **Device A/B.** Build the AAR twice, identical except `BUILD_WITH_NEON`, and
   record a v2 payload from the same staged bag played from the start with
   recording armed. Playback makes the input frames byte-identical, and both
   builds use our own PNG encoder, so `frames/NNNN/depth.png` can be compared
   byte for byte. `color.jpg` tests the NEON YUY2 path at the same time.
2. **Desktop A/B on an Apple Silicon Mac.** `__ARM_NEON` is defined and
   `ANDROID` is not, so a locally built librealsense already uses `align_neon`.
   Build twice with the flag flipped and diff aligned depth buffers over the
   same bag. Faster loop, and it is the shape of test a maintainer would want
   with an upstream pull request. Not proof for the device: macOS Clang and NDK
   Clang are different targets.

Comparing the device against the server's converter is **not** a clean byte
test - the converter uses a different PNG encoder, so that comparison has to
decode to pixels first, and it also needs the same decimation phase.

## Worth adding: expose Playback

Not required for correctness, and not urgent. But `setRealTime(false)` is the
one omission that costs real time daily: with it, frames arrive as fast as the
consumer takes them, and testing against a bag becomes bounded by how fast the
writer encodes rather than by the length of the recording.

`seek` and `getDuration` would come along with it and would make a "record
from the start of the bag" mode exact, which matters for one specific
comparison: the desktop converter decimates from the bag's first frame, so a
device recording that starts at the same point selects the same frames and its
`sensors.json` can be diffed directly against the converter's output. Starting
mid-playback picks a different decimation phase and only supports the looser,
tolerance-based comparison.

If the fork is being updated for other reasons, this is the piece to add.
