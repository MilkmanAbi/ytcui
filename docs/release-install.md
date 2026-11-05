All required Linux GCC/Clang, macOS Intel/ARM, FreeBSD, NetBSD and OpenBSD CI jobs must pass before publication.

Choose the archive matching your CPU. macOS archives are separate arm64 and x86_64 builds, not universal binaries. Native libraries (OpenSSL, curl, ncurses and zlib) must be installed for your platform.

Run `ytcui` beside `ytcui-we` and `vendor/ejs/` from the extracted archive, or install:

```sh
sudo install -m755 ytcui ytcui-we /usr/local/bin/
sudo mkdir -p /usr/local/share/ytcui-we
sudo cp -R vendor/ejs /usr/local/share/ytcui-we/
```

ytcui-we needs a compatible Node runtime for EJS player challenges. Install Node 24 or newer and run `ytcui-we --check-runtime`. You may override its executable with `YTCUI_JS_RUNTIME`. yt-dlp and mpv are separate runtime dependencies for their respective modes.
