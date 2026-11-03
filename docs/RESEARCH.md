# What makes a terminal YouTube client worth using

Research for ytcui v5, done by surveying what the existing terminal clients
ship and what their users single out. Sources at the bottom.

The useful finding is not a list of features — it is that the field is
crowded with tools that all do *search → mpv* and differentiate on what
happens **around** playback. Nearly everyone has search. Almost nobody has
the surrounding workflow.

## The field

| Tool | Shape | Noted for |
|---|---|---|
| **ytfzf** | shell + fzf | fastest search-to-play; multi-select queueing; thumbnails via ueberzug/kitty; dmenu/rofi front-ends |
| **yt-tui** | Rust TUI | tabs/panels; built-in SponsorBlock; marks downloaded / watched / partially-watched |
| **yewtube** | Python | search, queue, comments, subscriptions |
| **youtube-viewer** | Perl | search, subscriptions, playlists, comments |
| **tuitube** | Rust | vim keys, local subscriptions |
| **yt-x** | POSIX + fzf | previews, rofi launcher, other yt-dlp sites |
| **yt-cli** | Python | SponsorBlock, "infinite radio", downloads |
| **ytcc** | Python | marks watched on mpv exit 0; `Shift-Q` saves position without marking watched |

## What actually gets singled out

Ranked by how often it comes up, and how loudly:

### 1. SponsorBlock — the clearest single differentiator

It is the one feature every comparison table leads with, and the one tools
advertise in their first paragraph. ytfzf only gets it indirectly via an mpv
script; yt-tui lists "built-in" as its headline advantage over ytfzf.

Worth doing **natively** rather than by telling people to install an mpv
script, because the data is also useful in the UI — showing that a video has
3 minutes of sponsor in it before you play it is something no client does.

The privacy-preserving endpoint matters and is not optional for this
audience. `GET /api/skipSegments/<4 hex chars of sha256(videoID)>` returns
every video sharing that hash prefix — ~128 of them in the bucket measured
— so the server never learns which video is being watched. Response shape,
confirmed live:

```
[ { "videoID": "...",
    "segments": [ { "category": "intro", "actionType": "skip",
                    "segment": [0, 14.827], "votes": 0, "locked": false,
                    "UUID": "...", "videoDuration": 212.0 } ] } ]
```

Categories: `sponsor`, `selfpromo`, `intro`, `outro`, `interaction`,
`music_offtopic`, `preview`, `filler`. `actionType` is `skip`, `mute`,
`full` or `poi`.

### 2. Watch state: watched / partially watched / resume

Universally present in the better tools, universally absent from the simple
ones, and the thing that turns a search tool into something you live in.

ytcc's rule is the sound one and worth copying: a video counts as *watched*
when the player exits cleanly, not merely when it was opened. mpv's own
`watch-later` only saves on a graceful quit, so a client that wants reliable
resume should track position itself rather than relying on it — ytcui already
polls `time-pos` over IPC, so this is nearly free.

### 3. A queue

ytfzf gets this through fzf multi-select and people clearly use it. In a
full TUI it is the obvious missing verb: "play this next" / "add to queue",
with the queue visible and reorderable.

### 4. Thumbnails

Divisive. Everyone who has them mentions them; they also drag in ueberzug or
a kitty-specific protocol and are the most common source of "it doesn't work
in my terminal". The chafa symbol-cell approach v4 already used is the
portable answer.

### 5. Comments

yewtube and youtube-viewer have them, and they are cited as a reason to use
those over lighter tools. Nobody lists them as a *first* reason.

### 6. Autoplay / radio

yt-cli's "infinite radio" — keep playing related tracks forever — is exactly
the use case a music-oriented TUI gets reached for, and it pairs with audio
mode and the streamlined layout.

## What this means for ytcui v5

Already planned and confirmed worth it: live backend switching (unique —
nothing else surveyed lets you change extractor at runtime, and every one of
these tools dies the same way when YouTube changes), three UI modes, share
menu, link handling, themes.

Added on the strength of this research, in priority order:

1. **SponsorBlock**, native, via the hash-prefix endpoint, with the segment
   summary shown before playback and skipping driven over mpv IPC.
2. **Resume and watched state**, tracked by ytcui rather than mpv, with the
   ytcc rule for what counts as watched.
3. **A queue**, with play-next and a visible, reorderable list.
4. **Autoplay / radio** for audio mode.

Deliberately not now: comments (large, and nobody's first reason), and real
graphics-protocol thumbnails (chafa cells first, since they work everywhere).

## The gap nobody fills

Every tool here breaks the same way: YouTube changes something, the single
extractor it depends on stops working, and the tool is dead until its
maintainer ships a fix and the user updates. ytcui v5 carries two extractors
and switches between them at runtime with automatic failover, which is the
one structural advantage available here — and the reason the rest of this
list is worth building on top of it.

## Sources

- [ytfzf](https://github.com/linuxindia/ytfzf) ·
  [ytfzf write-up](https://www.linuxuprising.com/2021/03/ytfzf-search-with-thumbnails-and-play.html) ·
  [ytfzf guide](https://www.linuxfordevices.com/tutorials/linux/ytfzf-play-youtube-videos-in-terminal)
- [yt-tui](https://github.com/mooxiu/yt-tui) ·
  [youtube-tui](https://github.com/jJohn313/youtube-tui) ·
  [tuitube](https://docs.rs/crate/tuitube/latest/source/src/player.rs)
- [yt-x](https://github.com/Benexl/yt-x) ·
  [yt-cli write-up](https://hrithick.hashnode.dev/how-i-built-yt-cli-a-terminal-youtube-player-with-sponsorblock-infinite-radio-and-downloads)
- [yewtube alternatives](https://alternativeto.net/software/yewtube) ·
  [terminal YouTube tool roundup](https://www.positioniseverything.net/18-best-free-and-open-source-terminal-based-youtube-tools/)
- [ytcc's mpv notes](https://sources.debian.org/src/ytcc/2.6.1-1.1/doc/mpv.md) — the watched/resume rule
- SponsorBlock API shape confirmed by querying
  `https://sponsor.ajay.app/api/skipSegments/<prefix>` directly; the wiki is
  behind bot protection.

> Caveat worth stating: most feature claims above come from each project's
> own README, and the one "survey" style source is a listicle. The ranking
> here is a judgement from what tools choose to advertise first, not from
> user-survey data, which does not appear to exist for this niche.
