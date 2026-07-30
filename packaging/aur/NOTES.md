# Packaging notes

## The AUR `ytcui-bin` 404

The failing install was:

```
curl: (22) The requested URL returned error: 404
==> ERROR: Failure while downloading https://raw.githubusercontent.com/MilkmanAbi/ytcui/v1.0.0/README.md
```

Root cause: **the repository has no git tags at all** (`git tag -l` is empty),
so any URL pinned to a tag ref — here `.../ytcui/v1.0.0/README.md` — resolves to
404. It is not a transient network error and retrying never helps. The current
source tree is `v3.0.0` (see `VERSION`), and `v1.0.0` was never tagged.

Two contributing problems in the old PKGBUILD:

1. It pinned a tag (`v1.0.0`) that does not exist.
2. It fetched the README as a *separate* `source=()` entry from `raw.githubusercontent.com`.
   A release tarball already contains the README, so this extra fetch should not
   exist in the first place.

## Fix

Pick one of the two PKGBUILDs in this directory:

- **`ytcui-git/PKGBUILD`** — builds from `main`. Works *today*, needs no tags.
  This is the recommended package while releases are untagged.

  ```sh
  cd packaging/aur/ytcui-git
  makepkg -si
  ```

- **`ytcui/PKGBUILD`** — versioned, builds from a release tarball. Use this once
  you start tagging releases. Before publishing it:

  ```sh
  git tag -a v3.0.0 -m "v3.0.0"
  git push origin v3.0.0
  cd packaging/aur/ytcui
  updpkgsums      # fills in sha256sums from the real tarball
  makepkg -si
  ```

Both `provides`/`conflicts` with each other and with the old `ytcui-bin`, so
users won't end up with two copies.

## Recommendation: tag your releases

Most packaging pain here disappears if each release is tagged. The `VERSION`
file is already the source of truth — tag it on every bump:

```sh
git tag -a "v$(cat VERSION)" -m "v$(cat VERSION)"
git push origin "v$(cat VERSION)"
```

That makes `archive/refs/tags/vX.Y.Z.tar.gz` and any `raw/.../vX.Y.Z/...` URL
resolve, and lets the versioned PKGBUILD (and any downstream distro packaging)
work without modification.

## Build knobs the packages expose

- `BACKEND=ytcuidl` (default) — built-in InnerTube client, no Python/yt-dlp.
- `BACKEND=ytdlp` — legacy yt-dlp backend.
- `SIXEL=libsixel` — link libsixel for in-process SIXEL thumbnail encoding
  (optional; the default sixel path uses chafa, already a runtime dep).

Runtime deps: `mpv`, `chafa`. Build deps: a C++17 compiler, `make`, `pkgconf`,
plus `ncurses`/`curl`/`openssl` dev headers.
