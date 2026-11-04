# Controlled audit reproductions

Audited commit: 63409a83575b0a55f66c8cece8fe2eb3f6b5851d.
Run under Linux/Ubuntu WSL, in an extracted directory whose contents may be used for disposable fixtures. No browser credentials are required. Do not run against a newer commit and assume identical results.

Install build prerequisites (GCC, CMake, Ninja, Python 3, OpenSSL/curl/ncurses development packages). Set the absolute clone path and build it:

```sh
export AUDIT_REPO=/absolute/path/to/ytcui-test-repo
cmake -S "$AUDIT_REPO" -B "$AUDIT_REPO/build-audit" -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build "$AUDIT_REPO/build-audit" --parallel 2
ctest --test-dir "$AUDIT_REPO/build-audit" --output-on-failure
```

From this extracted directory, compile the small probes:

```sh
g++ -std=c++17 -I"$AUDIT_REPO/include" proc_probe.cpp "$AUDIT_REPO/src/core/proc.cpp" -pthread -o proc_probe
g++ -std=c++17 -I"$AUDIT_REPO/include" config_probe.cpp "$AUDIT_REPO/src/config.cpp" -o config_probe
g++ -std=c++17 -I"$AUDIT_REPO/include" thumb_probe.cpp -o thumb_probe
g++ -std=c++17 -I"$AUDIT_REPO/include" library_probe.cpp -o library_probe
./proc_probe
mkdir -p config-home
XDG_CONFIG_HOME="$PWD/config-home" ./config_probe
python3 run_thumbnail_probe.py
python3 run_library_probe.py
python3 build_uaf_probe.py
```

The App lifetime probe reuses objects from the Debug build, replaces the SponsorBlock network fetch with a controlled completion, and instruments the actual app.cpp with ASan. Its direct symbol wrapping is specific to this Linux/GCC build. It intentionally exits with an ASan failure; app-uaf.log contains the captured evidence. The Python runner itself reports the child exit and error.

Expected buggy behavior:
- proc_probe: approximately 700 ms, timed_out=0 despite a 100 ms deadline.
- config_probe: backend=0, empty cookies, sponsorblock=1, resume=1; only height=720 survives.
- thumbnail probe: a benign marker is created by shell syntax in a fixture URL. A fake curl prevents network access. This proves unsafe interpretation, not an established remote metadata delivery chain.
- library probe: child dies with SIGXFSZ and previously valid fixture JSON becomes invalid. Only library-home/ytcui/library.json is affected.
- App probe: heap-use-after-free, child exit 1.

Test files are isolated through XDG_CONFIG_HOME, XDG_DATA_HOME and XDG_CACHE_HOME. The thumbnail runner creates its own fake curl and removes/recreates its own marker. The library runner intentionally truncates only its disposable fixture. Retained logs describe the audited revision; a successful fix should change those outcomes.
