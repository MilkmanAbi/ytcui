# Audit fixes

The original README is a historical review of commit 63409a8. This change implements the following corrections; its findings should not be read as the current implementation status.

| Findings | Correction | Verification |
| --- | --- | --- |
| AUD-01, AUD-10 | Owned SponsorBlock worker joined during cleanup; accepted-request tracking retries a new video after an overlapping request; results are consumed only for their video; quality restarts retain matching segments. Signal handlers request normal shutdown. | `test_app_lifetime`, including controlled pending fetch destruction and A-to-B transitions; AddressSanitizer CI |
| AUD-02, AUD-08 | Unified runtime resolver for video, audio and audio-loop; explicit failover setting; actual backend recorded; per-Player current-video ladder cleared on failure. Native live HLS is handed to mpv. | `test_player_we`: three backends x three modes, exact argv, public Player/mpv dispatch for both subprocess backends, failover and stale-ladder checks |
| AUD-03, AUD-04, AUD-15 | Both executables built beside each other and installed to bin; build/portable resources under vendor/ejs; installed assets under share/ytcui-we/ejs. libcurl is required. | Mandatory companion smoke, installed_bundle, unpacked package `--check-runtime` |
| AUD-05 | Thumbnail download and chafa rendering use argv, never metadata-derived shell programs; cache IDs are constrained; directory creation uses filesystem APIs. | `test_runtime` with malicious URL, quote/space-containing XDG paths and traversal ID |
| AUD-06 | Deadline covers process lifetime after pipe EOF and kills the whole process group. | `test_runtime` pipe-closing child and ordinary output/exit capture |
| AUD-07 | Active config loads/saves all options, including backend/cookies/SponsorBlock/resume; control-key tokens round-trip; startup backend display matches config. | `test_runtime` config round-trip |
| AUD-09 | Explicit browser selection imports readable auth cookies to a private Netscape file; native search and playback and WE use that file. UI reports imported/unverified, never claims browser selection verified a login. Logout clears app-owned session state. | Synthetic cookie-domain acceptance/rejection and clearing in `test_app_lifetime`; real browser profiles are not read by tests |
| AUD-11 | No skipped backend integration. Test uses the exact built companion, verifies the structured WE bridge with deterministic children, checks resolution/cookie/audio forwarding and rejects malformed output. | Mandatory `test_player_we` and companion_smoke |
| AUD-12, AUD-16 | Auto-tag explicitly calls reusable release workflow; release invokes the same complete reusable CI matrix, including all BSDs and sanitizer tests. Publishing alone gets contents-write. | actionlint; GitHub CI after push |
| AUD-13 | Separate macOS arm64 and x86_64 runners and named archives; lipo validates main and companion architecture. | Required architecture checks in release packaging |
| AUD-14 | Checked temporary-file write, fsync and atomic rename for library/history/playlists and config. Failed library saves reach the status bar; the old file is preserved. | `test_runtime` failed-write fault injection |

Build and test:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 3
ctest --test-dir build --output-on-failure
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DYTCUI_SANITIZE=ON
cmake --build build-asan --parallel 2 --target test_app_lifetime test_runtime
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 ctest --test-dir build-asan --output-on-failure -R '^(test_app_lifetime|test_runtime)$'
```

Install OpenSSL, zlib, ncurses, curl development packages, CMake/Ninja and a C++17 compiler. For player challenges, install Node 24+ and run `build/ytcui-we --check-runtime`. Playback requires mpv; selecting yt-dlp requires a separately installed yt-dlp. On systems without a self-executable-path API, set `YTCUI_WE_BINARY` to the absolute companion path; CTest does this automatically.

The deterministic suite tests integration contracts without accessing user credentials or depending on YouTube availability. It does not establish successful playback for every live or authenticated video. macOS/ BSD execution and release publication require their GitHub jobs; local Linux results alone are not evidence that those jobs passed.

## Local validation results

- Clean GCC 13.3 / Ubuntu 24.04 Debug build: all 13 CTest tests passed; none skipped.
- Clean AddressSanitizer build: both runtime/storage and App lifetime regressions passed, with no sanitizer errors.
- Installed bundle smoke passed. EJS helpers loaded and executed using Node 25.6 with its permission flag.
- Real public-video probe (`jNQXAC9IVRw`): ytcui-we resolved video/audio and fetched a media byte with HTTP 206; native ytcui-dl resolved and fetched both video and audio bytes with HTTP 206. Its 240p/144p ladder matched the available low-resolution source.
- actionlint 1.7.12 accepted CI, release and auto-tag workflows.

These local checks do not substitute for the remote macOS/BSD/compiler matrix or prove every authenticated/live video works. No real browser credentials were used in validation.

Remote CI exposed a NetBSD installed-companion loader failure (`libcurl.so.4` under pkgsrc). Installation now retains linked external dependency directories in RPATH, rather than depending on the build-tree RPATH or a test-only library-path override.
