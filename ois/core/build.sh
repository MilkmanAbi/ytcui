#!/bin/sh
# OIS v4 -- core/build.sh
# The build engine.
#
# shellcheck disable=SC2153  # OIS_OS et al are assigned in system.sh
# ---------------------------------------------------------------------

ois_build_detect() {
    [ "$OIS_BUILD_SYSTEM" != "auto" ] && return 0
    if   [ -f CMakeLists.txt ]; then OIS_BUILD_SYSTEM="cmake"
    elif [ -f Makefile ] || [ -f makefile ] || [ -f GNUmakefile ]; then OIS_BUILD_SYSTEM="make"
    elif [ -f meson.build ];  then OIS_BUILD_SYSTEM="meson"
    elif [ -f Cargo.toml ];   then OIS_BUILD_SYSTEM="cargo"
    elif [ -f go.mod ];       then OIS_BUILD_SYSTEM="go"
    elif [ -f build.zig ];    then OIS_BUILD_SYSTEM="zig"
    else
        ois_fail E-CONF "cannot detect a build system in $(pwd)" \
            "no CMakeLists.txt, Makefile, meson.build, Cargo.toml, go.mod, or build.zig" \
            "set [build] system= in ois.conf" \
            "or set [build] custom= to your own build command"
        return 1
    fi
    ois_dbg "build system: $OIS_BUILD_SYSTEM (auto-detected)"
    return 0
}

# Wire per-formula Homebrew flags (CPPFLAGS, LDFLAGS, PKG_CONFIG_PATH).
# Delegates to ois_deps_enrich_env, which uses the stable brew opt/
# symlinks and only wires deps that are actually installed. Idempotent,
# so calling it here (in addition to deps_check) is safe and covers the
# case where the build runs without a preceding deps_check.
_ois_build_brew_env() {
    [ "$OIS_OS" = "macos" ] || return 0
    command -v ois_deps_enrich_env >/dev/null 2>&1 && ois_deps_enrich_env
}

_ois_build_env() {
    case "$OIS_OS" in
        macos|freebsd|openbsd|netbsd|dragonfly)
            : "${CC:=clang}" ; : "${CXX:=clang++}" ;;
        *)  : "${CC:=cc}"   ; : "${CXX:=c++}" ;;
    esac
    export CC CXX

    # BSD: /usr/local headers and libs not searched by default
    case "$OIS_OS" in freebsd|openbsd|netbsd|dragonfly)
        CPPFLAGS="-I/usr/local/include${CPPFLAGS:+ $CPPFLAGS}"
        LDFLAGS="-L/usr/local/lib${LDFLAGS:+ $LDFLAGS}"
        export CPPFLAGS LDFLAGS ;;
    esac

    # MacPorts: /opt/local (or custom prefix) include/lib
    if [ "$OIS_PM" = "macports" ]; then
        _mpe="${OIS_PORT_PREFIX:-/opt/local}"
        CPPFLAGS="-I$_mpe/include${CPPFLAGS:+ $CPPFLAGS}"
        LDFLAGS="-L$_mpe/lib${LDFLAGS:+ $LDFLAGS}"
        export CPPFLAGS LDFLAGS
    fi

    # Homebrew keg-only: wire per-dep flags
    _ois_build_brew_env

    OIS_JOBS="$OIS_BUILD_JOBS"
    [ "$OIS_JOBS" = "auto" ] && OIS_JOBS="$(ois_cpu_count)"
    case "$OIS_JOBS" in ''|*[!0-9]*) OIS_JOBS=1 ;; esac
}

_ois_build_locate() {
    _bl_stamp="$1"
    if [ -n "$_bl_stamp" ]; then
        for _bl_p in "$OIS_BUILD_OUT" "./$OIS_BUILD_OUT" \
                     ".ois-build/$OIS_BUILD_OUT" "build/$OIS_BUILD_OUT" \
                     "target/release/$OIS_BUILD_OUT" "zig-out/bin/$OIS_BUILD_OUT"; do
            if [ ! -f "$_bl_p" ] || [ ! -x "$_bl_p" ]; then continue; fi
            if ois_newer_than "$_bl_p" "$_bl_stamp"; then
                OIS_BUILT="$_bl_p" ; return 0
            fi
        done
        if command -v find >/dev/null 2>&1; then
            _bl_found="$(find . -name "$OIS_BUILD_OUT" -type f \
                -newer "$_bl_stamp" 2>/dev/null | head -n 1)"
            if [ -n "$_bl_found" ] && [ -x "$_bl_found" ]; then
                OIS_BUILT="$_bl_found" ; return 0
            fi
        fi
        ois_dbg "no fresh artifact; accepting an existing one (build exited 0)"
    fi
    for _bl_p in "$OIS_BUILD_OUT" "./$OIS_BUILD_OUT" \
                 ".ois-build/$OIS_BUILD_OUT" "build/$OIS_BUILD_OUT" \
                 "target/release/$OIS_BUILD_OUT" "zig-out/bin/$OIS_BUILD_OUT"; do
        if [ -f "$_bl_p" ] && [ -x "$_bl_p" ]; then
            OIS_BUILT="$_bl_p" ; return 0
        fi
    done
    if command -v find >/dev/null 2>&1; then
        _bl_found="$(find . -name "$OIS_BUILD_OUT" -type f 2>/dev/null | head -n 1)"
        if [ -n "$_bl_found" ] && [ -x "$_bl_found" ]; then
            OIS_BUILT="$_bl_found" ; return 0
        fi
    fi
    return 1
}

ois_build_run() {
    _bd_log="${1:-${TMPDIR:-/tmp}/ois-build.$$.log}"
    _ois_build_env
    _bd_stamp="$(ois_tmpfile)" || _bd_stamp=""

    _bd_rc=0
    case "${OIS_BUILD_CUSTOM:+custom}${OIS_BUILD_CUSTOM:-$OIS_BUILD_SYSTEM}" in
        custom)
            ois_info "custom build: $OIS_BUILD_CUSTOM"
            ois_run_logged "$_bd_log" "custom build" sh -c "$OIS_BUILD_CUSTOM" || _bd_rc=$? ;;
        make)
            ois_need_tool "$OIS_MAKE" || return 1
            # shellcheck disable=SC2086
            ois_run_logged "$_bd_log" "make" \
                "$OIS_MAKE" -j"$OIS_JOBS" $OIS_BUILD_MAKE_OPTS \
                ${OIS_BUILD_TARGET:+"$OIS_BUILD_TARGET"} || _bd_rc=$? ;;
        cmake)
            ois_need_tool cmake || return 1
            # shellcheck disable=SC2086
            ois_run_logged "$_bd_log" "cmake configure" \
                cmake -S . -B .ois-build -DCMAKE_BUILD_TYPE=Release \
                $OIS_BUILD_CMAKE_OPTS || {
                _bd_rc=$?
                [ -n "$_bd_stamp" ] && rm -f "$_bd_stamp"
                ois_fail E-BUILD "cmake configuration failed (exit $_bd_rc)" \
                    "the project's CMakeLists.txt rejected this configuration" \
                    "read the log above -- missing dependencies appear here" \
                    "declare missing libraries under [deps] in ois.conf" \
                    "extra options can be passed via [build] cmake_opts ="
                return 1
            }
            ois_run_logged "$_bd_log" "cmake build" \
                cmake --build .ois-build -j "$OIS_JOBS" \
                ${OIS_BUILD_TARGET:+--target "$OIS_BUILD_TARGET"} || _bd_rc=$? ;;
        meson)
            ois_need_tool meson || return 1
            ois_run_logged "$_bd_log" "meson setup" sh -c \
                'meson setup .ois-build 2>/dev/null || meson setup --wipe .ois-build' \
                || _bd_rc=$?
            [ "$_bd_rc" = 0 ] && ois_run_logged "$_bd_log" "meson compile" \
                meson compile -C .ois-build \
                ${OIS_BUILD_TARGET:+"$OIS_BUILD_TARGET"} || _bd_rc=$? ;;
        cargo)
            ois_need_tool cargo || return 1
            ois_run_logged "$_bd_log" "cargo" cargo build --release || _bd_rc=$? ;;
        go)
            ois_need_tool go || return 1
            ois_run_logged "$_bd_log" "go" go build -o "$OIS_BUILD_OUT" ./... || _bd_rc=$? ;;
        zig)
            ois_need_tool zig || return 1
            ois_run_logged "$_bd_log" "zig" zig build -Doptimize=ReleaseSafe || _bd_rc=$? ;;
        *)
            [ -n "$_bd_stamp" ] && rm -f "$_bd_stamp"
            ois_fail E-CONF "unknown build system: $OIS_BUILD_SYSTEM" "" \
                "valid: make cmake meson cargo go zig custom"
            return 1 ;;
    esac

    if [ "$_bd_rc" != 0 ]; then
        [ -n "$_bd_stamp" ] && rm -f "$_bd_stamp"
        ois_fail E-BUILD "$OIS_BUILD_SYSTEM build failed (exit $_bd_rc)" \
            "the compiler or build tool reported errors -- see the excerpt above" \
            "full log: $_bd_log" \
            "if headers are missing, declare the library under [deps] in ois.conf" \
            "re-run with --verbose for OIS-side detail"
        return 1
    fi

    if ! _ois_build_locate "$_bd_stamp"; then
        [ -n "$_bd_stamp" ] && rm -f "$_bd_stamp"
        ois_fail E-BUILD "build succeeded but produced no executable named '$OIS_BUILD_OUT'" \
            "the build ran clean yet nothing fresh and executable by that name exists" \
            "if the binary has a different name, set [build] out = <name>" \
            "if only a subtarget builds it, set [build] target = <name>" \
            "full log: $_bd_log"
        return 1
    fi
    [ -n "$_bd_stamp" ] && rm -f "$_bd_stamp"
    ois_dbg "artifact: $OIS_BUILT"
    return 0
}
