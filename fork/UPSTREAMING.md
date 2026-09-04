# Upstreaming the librealsense fork

A plan, not a commitment. Written 2026-09-04. Nothing here is started.

The fork exists because Intel's Android wrapper went years between meaningful
updates and we needed things it did not have. That cost is real and recurring:
merging 2.54.2 to 2.58.1 produced 23 conflicts, and every one of them was a
place where our delta overlaps a file upstream also edits. Anything we can get
accepted upstream stops being our problem permanently.

Now that the repository has moved from Intel to `realsenseai` and carries a
`CONTRIBUTING.md` that says "this project welcomes third-party code", it is
worth finding out what their bar actually is - starting with something small
enough that a rejection costs nothing.

## How to keep publishing out of a pull request

The short answer: **by branch, not by file.** This is the part that looks like
it needs a trick and does not.

Our `android` branch carries everything - the GitHub Packages publishing block,
the pinned `ndkVersion`, `-DBUILD_ROSBAG2=OFF`, all of it. A pull request branch
is cut fresh from `origin/development` and contains only the one change being
proposed. The publishing block is never in it because it was never added to it.
That is also exactly what `CONTRIBUTING.md` asks for: branch from
`development`, one topic per branch.

So no `.gitignore` games and no untracked files are needed for the PR itself.

### Reducing our own merge pain, separately

What *is* worth doing is shrinking the fork's tracked delta, because that delta
is the conflict surface on every future merge. Today
`wrappers/android/librealsense/build.gradle` differs from upstream by 77 lines,
and it conflicted in the 2.58.1 merge.

Move the publishing configuration into a **new file** that upstream does not
have:

```groovy
// wrappers/android/librealsense/build.gradle - the only publishing-related
// line that stays in a file upstream also edits
if (file("publish.gradle").exists()) { apply from: "publish.gradle" }
```

with `publish.gradle` holding the `maven-publish` plugin, the GitHub Packages
repository, the publication, and `androidBuildNumber`.

Why a new tracked file rather than an untracked one: **new files never
conflict**, because upstream has nothing at that path to disagree with. An
untracked file would also work but is easy to lose, invisible to a second
machine, and impossible to use from CI. Tracking it in the fork costs nothing
and survives every merge untouched.

The `if (file(...).exists())` guard means a branch without `publish.gradle`
still configures, so the line is harmless if it ever leaks into a PR branch.

Credentials stay where they already are - `GITHUB_USERNAME` and `GITHUB_TOKEN`
in `~/.gradle/gradle.properties`, never in the repository.

Considered and rejected: a Gradle init script under `~/.gradle/init.d/`. It
leaves a zero-line footprint in the repository, which is appealing, but it is
machine-local invisible magic that breaks silently on a new machine or in CI.

## What is ours, and what could be theirs

| change | upstreamable | note |
| --- | --- | --- |
| `BUILD_WITH_NEON` ignored on Android | yes | the strongest candidate, see below |
| `Enumerator.java` null guard on `intent.getAction()` | yes | `switch` on a null String throws |
| `UsbUtilities.java` `FLAG_CANCEL_CURRENT` | yes | stale PendingIntent on replug |
| `Intrinsic` focal length / optical center accessors | yes | still absent in 2.58.1; the values are already in the object, only the getters are missing |
| amp factor JNI binding | yes | still absent in 2.58.1; `rs2_set_amp_factor` is already public C API, just unreachable from Java |
| manifest `package=` removal | yes | AGP 8 requires `namespace` instead |
| AGP 8.12.1 / Kotlin 2.2.10 / tracked Gradle wrapper | yes | upstream is on 8.9.1 / 2.0.21 and does not track `gradle-wrapper.properties` |
| version derived from `rs.h` | maybe | a genuine improvement, but it is our versioning scheme |
| GitHub Packages publishing | no | ours |
| pinned `ndkVersion 28.2.13676358` | no | ours, machine-specific |
| `-DBUILD_ROSBAG2=OFF`, `-DBUILD_WITH_NEON=ON` | no | build choices, belong in our Gradle arguments |
| removing `DeviceWatcherActivity` | not directly | see below |

## The NEON change, restated for a pull request

Do **not** propose "add Android NEON support". Propose a narrower and more
defensible thing: `BUILD_WITH_NEON` is declared, architecture-guarded, and
defaults ON for Android - and then the sources ignore it. The build option and
the code contradict each other.

The patch that keeps upstream's effective behavior unchanged:

```cmake
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|ARM64|aarch64|AARCH64)")
    if(ANDROID)
        option(BUILD_WITH_NEON "Enable ARM64 NEON optimizations" OFF)
    else()
        option(BUILD_WITH_NEON "Enable ARM64 NEON optimizations" ON)
    endif()
endif()
```

plus removing the exclusion from all six guard sites (three spelled
`&& !defined(ANDROID)`, three spelled `#ifndef ANDROID` - see
README.md in this directory).

`option()` supplies a default and does nothing when the variable already
exists, so `-DBUILD_WITH_NEON=ON` on the configure line still wins. Verified:
a bare configure yields OFF, `-DBUILD_WITH_NEON=ON` yields ON. `ANDROID` is set
by the NDK toolchain during `project()` at CMakeLists.txt:4, before
`lrs_options.cmake` is included at line 10, so the Android branch is reached.

Adopting this shape in our fork too - with `-DBUILD_WITH_NEON=ON` added to the
Gradle CMake arguments - would make our diff identical to the submitted patch,
leaving nothing to reconcile afterwards.

**This is gated on correctness evidence we do not have.** We measured that NEON
align is roughly 4x faster. We never verified its output matches the generic
path. That is the first question a maintainer will ask, and it is the most
plausible reason the exclusion exists. The device-side check is the one already
used for alignment: a `depth.png` from a v2 payload against the converter's
output for the same frame, which is pixel-identical when alignment is correct.
For a pull request they would want it in their own suite - see
`.github/skills/testing.md` and `pytest-infra.md` in the fork.

## DeviceWatcherActivity

Our app removes it with `tools:node="remove"`, which is an application-side
decision and not upstreamable as such. The upstreamable version is a design
change - the library forcing a `USB_DEVICE_ATTACHED` entry point on every
consumer is what produces the chooser dialog when two apps embed the SDK, and
the activity's entire body is `super.onCreate(); finish();`. Making it optional,
or moving it to a separate artifact, is worth an issue and a discussion before
any code. See `docs/USB-CONNECTION.md` in `timbeter-forest-android`.

## What they require

From `CONTRIBUTING.md` and `.github/skills/git-workflow.md` in the fork:

- Pull requests target **`development`**, never `master`. Branch from
  `origin/development` after `git reset --hard origin/development`.
- Run `scripts/pr_check.sh` and `scripts/api_check.sh`; `./pr_check.sh --fix`
  resolves most findings. They enforce a license reference and copyright notice
  in every source file, spaces rather than tabs, Unix line endings, and API
  headers that compile as the first include.
- Enable GitHub Actions on the fork so every platform builds and unit tests run.
- Apache 2.0.
- Commit messages: **short, one sentence**, plain `git commit -m`. This is not
  our Conventional Commits style, and applies to upstream branches only.
- There is a `.github/pull_request_template.md` to fill in.

**`pr_check.sh` does not run on macOS.** It uses `grep -oP` in three places;
BSD grep rejects `-P`, so every file appears to fail and the script reports
thousands of phantom errors. Either `brew install grep` and put
`$(brew --prefix)/opt/grep/libexec/gnubin` first on `PATH`, or let the fork's
GitHub Actions run it.

## Suggested order

Smallest and most defensible first, so the process is learned on something
cheap:

1. `Enumerator.java` null guard and `UsbUtilities.java` PendingIntent flags -
   two small, obviously-correct fixes. Good for calibrating their review bar.
2. `Intrinsic` accessors - additive, no behavior change, immediately useful to
   anyone doing 3D work from Java.
3. The NEON flag - once correctness evidence exists.
4. Android toolchain modernization - larger, and worth attempting only after
   the first three show what they expect.
5. `DeviceWatcherActivity` - an issue and a discussion, not a patch.
