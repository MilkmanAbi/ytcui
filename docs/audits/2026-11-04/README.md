# ytcui-test-repo audit

Historical baseline: see [implemented fixes and regression coverage](fixes.md) for the follow-up changes.

Audited commit: [`63409a83575b0a55f66c8cece8fe2eb3f6b5851d`](https://github.com/MilkmanAbi/ytcui-test-repo/tree/63409a83575b0a55f66c8cece8fe2eb3f6b5851d), current default branch `main` (rechecked before publication), application version **5.0.4**. Published artifacts inspected: **v5.0.2**. Audit date: 4 November 2026, Singapore time.

**16 findings: four P1 issues and twelve P2 issues.** Fix AUD-01 through AUD-04 before treating the three-backend integration and release bundles as complete. Green CI currently misses important runtime paths.

The current source really does call a ytcui-we subprocess for video playback. Its separate-executable boundary avoids linking the two incompatible header implementations. The remaining problems concern discovery, deployed resources, dispatch, lifetime, state and coverage.

## Validation performed

- Cloned the default branch and froze the review at the commit above. Tracked source files were left unchanged; no commits or pushes were made.
- Configured and built the full project with CMake/Ninja, GCC 13.3 and Debug settings under Ubuntu 24.04 in WSL.
- Ran CTest: **eight passed, one skipped**. The skipped test was `test_player_we`, despite the companion being successfully built.
- Instrumented the actual `src/app.cpp` with AddressSanitizer and controlled only the SponsorBlock fetch completion. Destroying the App before completion produced **heap-use-after-free**, exit 1. No real SponsorBlock request was required.
- Ran isolated process-timeout, configuration round-trip, thumbnail-command and failed-storage-write probes.
- Ran the existing integration test against a deliberately non-resolving fake companion: it reported **all checks passed**.
- Downloaded and inspected both published v5.0.2 archives, inspected the macOS executable architecture, and installed the local build into an isolated prefix.
- Reviewed active playback/search/download/authentication paths, state and storage code, subprocess handling, companion integration and release workflows. Pure core tests and critical vendored-header consistency were checked as part of this review.

This is a source and runtime audit, not a claim of exhaustive verification. I did not conduct a complete interactive terminal tour, a live matrix of authenticated YouTube videos, or a fresh line-by-line audit of nlohmann/json and qrcodegen. Browser/profile data was not used by the probes. Test state and fault-injection files were isolated from the user's actual library.

## Findings

### AUD-01 — P1: detached SponsorBlock worker accesses a destroyed App

**Evidence: reproduced with AddressSanitizer.** [`src/app.cpp:1818`](https://github.com/MilkmanAbi/ytcui-test-repo/blob/63409a83575b0a55f66c8cece8fe2eb3f6b5851d/src/app.cpp#L1818) starts a detached thread capturing `this`. After fetching, it locks `sb_mutex_`, assigns `sb_segs_`/`sb_video_id_` and updates `sb_fetching_`. [`App::~App():135`](https://github.com/MilkmanAbi/ytcui-test-repo/blob/63409a83575b0a55f66c8cece8fe2eb3f6b5851d/src/app.cpp#L135) only stops the player and shuts down the TUI; it does not join or otherwise preserve the worker's state.

Quitting while a fetch is in flight frees the object before those writes. The controlled completion probe produced an ASan read-after-free in the worker's vector assignment, inside a freed App allocation.

**Fix:** own and join/cancel the worker, or move its state into an independently owned lifetime object. Add a test that destroys App while a controlled fetch is pending and completes that fetch afterwards.

### AUD-02 — P1: the runtime backend selector does not dispatch all playback modes

**Evidence: active compiled control flow.** In [`src/player.cpp:575`](https://github.com/MilkmanAbi/ytcui-test-repo/blob/63409a83575b0a55f66c8cece8fe2eb3f6b5851d/src/player.cpp#L575), every non-ytcui-we video selection calls the native ytcui-dl resolver when `USE_YTCUIDL` is defined. CMake always defines it. Selecting `BackendKind::YtDlp` therefore still uses ytcui-dl when native resolution succeeds, and the subsequent log can label those streams `yt-dlp`.

Additionally, [`Player::play():60`](https://github.com/MilkmanAbi/ytcui-test-repo/blob/63409a83575b0a55f66c8cece8fe2eb3f6b5851d/src/player.cpp#L60) routes audio modes to `play_piped()`, whose compiled native branch resolves directly through the ytcui-dl singleton, without consulting `opts_.backend`. Selecting ytcui-we does not select it for audio playback.

**Fix:** use one runtime dispatch function for video and audio, carrying mode, resolution and authentication inputs. Track the backend that actually returned the result separately from the requested backend. Test all three backends in every supported mode with controlled resolvers.

### AUD-03 — P1: a normal source build cannot discover its own companion

**Evidence: clean build and skipped integration test.** [`find_ytcuiwe_binary():22`](https://github.com/MilkmanAbi/ytcui-test-repo/blob/63409a83575b0a55f66c8cece8fe2eb3f6b5851d/src/player_we.cpp#L22) searches beside the running executable, then PATH. The build produces `build-audit/ytcui`, but the companion is at `build-audit/third_party/ytcui-we-src/ytcui-we`. There is no sibling companion in the directory discovery searches.

On a machine without another installed copy, selecting ytcui-we therefore throws “executable not found” and falls back to ytcui-dl. The local integration test skipped for exactly this reason even though the companion build succeeded.

**Fix:** produce an explicit build bundle with sibling executables, or configure discovery with the built target's path. Make the test depend on and invoke that exact built companion rather than an arbitrary external PATH installation.

### AUD-04 — P1: release and install layouts put EJS outside the resolver's search paths

**Evidence: published archive inspection and source.** Both v5.0.2 archives contain `./ytcui-we` and `./ejs/yt.solver.{core,lib}.js`. The resolver in [`we_application.h:103`](https://github.com/MilkmanAbi/ytcui-test-repo/blob/63409a83575b0a55f66c8cece8fe2eb3f6b5851d/third_party/ytcui-we-src/cli/we_application.h#L103) searches `vendor/ejs` beside the executable, `../vendor/ejs`, then `../share/ytcui-we/ejs`.

The release workflow copies to `dist/ejs` at [line 154](https://github.com/MilkmanAbi/ytcui-test-repo/blob/63409a83575b0a55f66c8cece8fe2eb3f6b5851d/.github/workflows/release.yml#L154), and its install instructions place it under `/usr/local/share/ytcui/ejs`. CMake installs it under `share/ytcui/vendor/ejs` at [line 47](https://github.com/MilkmanAbi/ytcui-test-repo/blob/63409a83575b0a55f66c8cece8fe2eb3f6b5851d/third_party/ytcui-we-src/CMakeLists.txt#L47). None matches discovery. The OIS adapter does not pass `--ejs-dir`.

This fails when an uncached player challenge needs the helper files. Clients/videos that do not require a challenge can appear to work, so a successful `--version` smoke test cannot catch it.

**Fix:** choose one resource layout shared by CMake install, release packaging and discovery. Test loading the solver from an unpacked archive with a fresh cache and compatible Node runtime; document that runtime dependency.

### AUD-05 — P2: thumbnail metadata is interpreted as shell syntax

**Evidence: controlled command-execution probe.** [`include/thumbs.h:120`](https://github.com/MilkmanAbi/ytcui-test-repo/blob/63409a83575b0a55f66c8cece8fe2eb3f6b5851d/include/thumbs.h#L120) concatenates the thumbnail URL and cache paths into a single-quoted command and executes `sh -c`. Embedded single quotes are not escaped.

Using a synthetic URL containing a closing quote and a benign `touch` command created an audit-owned marker. A fake curl executable prevented actual network access. This demonstrates the input-to-shell sink; I have not established a remote-attacker delivery chain through normal YouTube metadata. Similar unescaped directory command construction exists in configuration/library helpers and fails for paths containing apostrophes.

**Fix:** execute curl with argv and handle the fallback in C++; use filesystem APIs for directories. Validate video IDs before deriving cache paths. Test quotes, shell characters, spaces and Unicode in metadata and filesystem paths.

### AUD-06 — P2: subprocess deadlines stop being enforced after pipe EOF

**Evidence: timed reproduction.** [`src/core/proc.cpp:155`](https://github.com/MilkmanAbi/ytcui-test-repo/blob/63409a83575b0a55f66c8cece8fe2eb3f6b5851d/src/core/proc.cpp#L155) checks the deadline only while stdout/stderr remain open. After both close, [line 220](https://github.com/MilkmanAbi/ytcui-test-repo/blob/63409a83575b0a55f66c8cece8fe2eb3f6b5851d/src/core/proc.cpp#L220) performs a blocking `waitpid`.

Running a child that closes both streams and sleeps for 0.7 seconds with a 100 ms deadline returned after **705 ms**, `timed_out=false`. A child that never exits can hang this call indefinitely, including synchronous UI resolution.

**Fix:** monitor child completion and the deadline together, using nonblocking wait after EOF. Keep process-group cleanup active until the child is reaped. Add closed-pipe, silent-child and descendant-process tests.

### AUD-07 — P2: active config serialization drops the new settings

**Evidence: configuration round-trip probe.** [`src/config.cpp:88`](https://github.com/MilkmanAbi/ytcui-test-repo/blob/63409a83575b0a55f66c8cece8fe2eb3f6b5851d/src/config.cpp#L88) and [line 161](https://github.com/MilkmanAbi/ytcui-test-repo/blob/63409a83575b0a55f66c8cece8fe2eb3f6b5851d/src/config.cpp#L161) load/save legacy fields plus preferred height, but omit backend, cookies file, SponsorBlock settings, resume and other newly exposed fields. The different v5 serializer in `src/core/config.cpp` is excluded from the application build.

After setting ytcui-we, a cookie-file path, SponsorBlock off, resume off and height 720, then saving/loading, the probe returned native backend `0`, an empty cookie path, SponsorBlock on and resume on. Height 720 did survive.

**Fix:** round-trip every supported setting through the active serializer and initialize UI/player state from it. Include manually supplied JSON fields in load tests, not just save-then-load tests.

### AUD-08 — P2: ytcui-we resolution menus use absent or stale native format data

**Evidence: static data flow.** [`Player::resolution_rungs():696`](https://github.com/MilkmanAbi/ytcui-test-repo/blob/63409a83575b0a55f66c8cece8fe2eb3f6b5851d/src/player.cpp#L696) only reads the global `g_last_formats_`, populated by native ytcui-dl resolution. The successful ytcui-we branch at [line 564](https://github.com/MilkmanAbi/ytcui-test-repo/blob/63409a83575b0a55f66c8cece8fe2eb3f6b5851d/src/player.cpp#L564) only copies selected URLs and User-Agent; it does not populate a ladder or clear old native data.

Starting with ytcui-we produces no resolution data for the popup. Switching to it after native playback can show the previous native video's ladder.

**Fix:** return a ladder DTO from each backend and key it by backend/video, clearing it on failed or new resolution. Test native-video-A to we-video-B transitions and first playback with we.

### AUD-09 — P2: the login picker reports authentication without wiring it to native resolution

**Evidence: static control flow.** [`src/app.cpp:834`](https://github.com/MilkmanAbi/ytcui-test-repo/blob/63409a83575b0a55f66c8cece8fe2eb3f6b5851d/src/app.cpp#L834) saves a browser name and immediately sets `logged_in=true`. `Auth::ytdlp_cookie_args()` only builds yt-dlp arguments. The compiled native search/download functions explicitly ignore those arguments, and native player resolution never loads a cookie jar from this picker.

For the default backend, an installed/configured browser is presented as a working account session even though the resolver remains anonymous. The we cookie-file option is a separate setting and is also affected by AUD-07.

**Fix:** distinguish browser configuration from authenticated-session state. Wire an explicit export/check flow to the selected backend and only report success after verifying a usable session. Use synthetic fixtures for tests.

### AUD-10 — P2: late SponsorBlock results can be applied to the wrong video

**Evidence: static race/interleaving analysis.** At [`src/app.cpp:466`](https://github.com/MilkmanAbi/ytcui-test-repo/blob/63409a83575b0a55f66c8cece8fe2eb3f6b5851d/src/app.cpp#L466), the loop records the new video as loaded before calling `start_sb_fetch`. That function refuses overlapping fetches. If A is fetching when B starts, B's fetch is refused, then A's result is committed. [`tick_sponsorblock():1847`](https://github.com/MilkmanAbi/ytcui-test-repo/blob/63409a83575b0a55f66c8cece8fe2eb3f6b5851d/src/app.cpp#L1847) uses the stored segments without matching `sb_video_id_` to the current video.

The loop has already marked B loaded, so it also misses the retry after A completes. B can receive A's skip times.

**Fix:** attach a generation/video identity to each request, discard stale completion, and mark loading only when dispatch succeeds. Check identity at consumption too. This remains necessary after fixing the worker's lifetime.

### AUD-11 — P2: the required backend integration test can skip or pass a non-resolver

**Evidence: existing test executed in two controlled conditions.** [`CMakeLists.txt:163`](https://github.com/MilkmanAbi/ytcui-test-repo/blob/63409a83575b0a55f66c8cece8fe2eb3f6b5851d/CMakeLists.txt#L163) accepts exit 77 as a skip. That happened in the complete local build because of AUD-03.

With a fake sibling companion that prints a help line and a constant JSON URL, `test_player_we` reported all checks passed. Its adapter call uses height zero; the separate quality invocation calls the executable directly and accepts nearly any exit status. A successful invalid-ID resolve only produces a warning. These checks do not establish that backend selection reaches the right resolver or that the adapter forwards the requested quality correctly.

**Fix:** require the built companion, separate its smoke test from deterministic adapter tests, capture exact argv, verify parsed video/audio/headers, and fail forbidden fallback. Exercise the public Player dispatch as well as the adapter helper.

### AUD-12 — P2: automatic tagging no longer forms a working release chain

**Evidence: workflow configuration and GitHub's event rules.** [`.github/workflows/auto-tag.yml:9`](https://github.com/MilkmanAbi/ytcui-test-repo/blob/63409a83575b0a55f66c8cece8fe2eb3f6b5851d/.github/workflows/auto-tag.yml#L9) watches `main`, which is the current default branch as rechecked before publication. The branch filter currently matches; the remaining defect is the token-triggered release chain below.

[Line 46](https://github.com/MilkmanAbi/ytcui-test-repo/blob/63409a83575b0a55f66c8cece8fe2eb3f6b5851d/.github/workflows/auto-tag.yml#L46) pushes using checkout's default GITHUB_TOKEN. GitHub documents that such push events do not start another workflow. Giving that job contents-write permission does not remove this event restriction. [GitHub documentation](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/trigger-a-workflow).

At review time, source VERSION was 5.0.4 while the latest published release was v5.0.2; a v5.0.4 tag also existed. That observed lag alone is not proof of how the tag was created, but the token-triggered workflow defect above is independently present.

**Fix:** invoke shared release jobs or explicitly dispatch the release workflow after creating the tag. Keep the publisher's existing contents-write permission narrowly scoped.

### AUD-13 — P2: the macOS release is arm64-only despite the universal build label

**Evidence: published executable inspected.** [`.github/workflows/release.yml:164`](https://github.com/MilkmanAbi/ytcui-test-repo/blob/63409a83575b0a55f66c8cece8fe2eb3f6b5851d/.github/workflows/release.yml#L164) calls the job “universal” but builds on a single runner without a multi-architecture build or lipo assembly. The published v5.0.2 `ytcui` has a thin Mach-O header and CPU type `0x0100000c` (arm64), rather than a fat/universal header.

Intel users cannot run that executable. The generic macOS archive name does not identify the restriction.

**Fix:** publish separately named Intel and Apple Silicon bundles, or build and validate genuinely universal executables and dependencies. Verify both the main and companion architectures before publishing.

### AUD-14 — P2: failed library writes corrupt the previously valid data file

**Evidence: isolated failed-write reproduction.** [`include/library.h:355`](https://github.com/MilkmanAbi/ytcui-test-repo/blob/63409a83575b0a55f66c8cece8fe2eb3f6b5851d/include/library.h#L355), [line 387](https://github.com/MilkmanAbi/ytcui-test-repo/blob/63409a83575b0a55f66c8cece8fe2eb3f6b5851d/include/library.h#L387) and [line 431](https://github.com/MilkmanAbi/ytcui-test-repo/blob/63409a83575b0a55f66c8cece8fe2eb3f6b5851d/include/library.h#L431) truncate the real library/history/playlist files before writing replacements, with no checked finalization or backup.

The probe started with a valid 72-byte bookmark file and limited only the child writer to 256 bytes. The failed save terminated with SIGXFSZ and left 256 bytes of invalid JSON in place of the valid file. This models a failed-write boundary; no user library was involved.

**Fix:** write and verify a same-directory temporary file, then atomically replace the original, reporting errors to the UI. Consider a last-known-good backup. The core watch-state serializer already uses a temp/rename pattern that can inform a shared implementation.

### AUD-15 — P2: CMake install installs the companion but omits the application

**Evidence: actual isolated installation.** The root [`CMakeLists.txt`](https://github.com/MilkmanAbi/ytcui-test-repo/blob/63409a83575b0a55f66c8cece8fe2eb3f6b5851d/CMakeLists.txt) has no install rule for `ytcui`. The child installs `ytcui-we` and assets. Running `cmake --install build-audit --prefix <audit-prefix>` installed only `bin/ytcui-we` plus the misplaced EJS tree; no `bin/ytcui` was installed.

**Fix:** install both executables and the canonical resources/licenses, then make release packaging and install smoke tests consume that same tree. A CMake install smoke test should execute both binaries from outside the build directory.

### AUD-16 — P2: the release gate does not include the complete CI platform set

**Evidence: workflow dependency graph.** CI includes NetBSD and OpenBSD jobs. The release workflow duplicates a smaller test matrix containing Linux, macOS and FreeBSD. [`release-linux:130`](https://github.com/MilkmanAbi/ytcui-test-repo/blob/63409a83575b0a55f66c8cece8fe2eb3f6b5851d/.github/workflows/release.yml#L130), `release-macos` and the publisher's [needs list at line 202](https://github.com/MilkmanAbi/ytcui-test-repo/blob/63409a83575b0a55f66c8cece8fe2eb3f6b5851d/.github/workflows/release.yml#L202) do not depend on those missing platform checks or on the separate CI run.

A tag can therefore publish despite a NetBSD/OpenBSD regression. This contradicts the workflow comment that all CI jobs gate publication.

**Fix:** call one reusable CI workflow from both branch CI and release, with an explicit required job set. Test the gate graph and avoid masking required companion checks with `|| true`.

## Additional maintenance observations

- Both CI and release still request `macos-14`. GitHub's runner documentation says those images retired on 2 November 2026. The inspected latest CI run nevertheless reported success for that label; I am not claiming an observed startup failure. Migrate the unsupported label rather than relying on its continued availability. [Official runner-image status](https://github.com/actions/runner-images).
- The Debug build emitted many C++20-designated-initializer warnings while the project declares C++17. The tested compiler accepted them; this audit did not reproduce a build failure from them.
- The native playback adapter does not adopt the standalone downloader CLI's live-HLS selection flow. A live playback/quality regression matrix is still needed before claiming equal live behavior across all three backends; that was not promoted to a reproduced finding here.
- The current publisher explicitly declares contents-write access. I found no evidence that adding blanket write permissions would solve the release-chain issues above.

## Recommended sequence

1. Fix App/worker lifetime, metadata-to-shell execution and process deadlines.
2. Fix backend dispatch, companion discovery, runtime resource layout and installation together.
3. Repair settings/authentication/ladder state and stale SponsorBlock request handling.
4. Make deterministic integration and unpacked-package tests mandatory, then consolidate the CI/release pipeline and architecture outputs.

The accompanying reproduction bundle contains source and Linux/WSL instructions for the controlled probes. It contains no user browser data or credentials.
