# EP1 — ExperimentalPatch1

## 256-colour thumbnail rendering

**Problem:** thumbnails were monochrome because `chafa --colors=none` was the
only way to produce output ncurses could handle without ghosting.

**Root cause recap:** chafa with colors emits `\033[38;5;Nm` ANSI escape
sequences. ncurses treats those as literal bytes — it tracks characters in its
own virtual buffer but has no knowledge of color state set via raw ANSI. When
`erase()` runs, ncurses clears its buffer but the terminal's framebuffer still
has the colored glyphs rendered outside ncurses' control → ghost thumbnails.

**EP1 fix:** parse chafa's ANSI output into ncurses color pairs. Specifically:

1. At `TUI::init()`, allocate 256 ncurses color pairs (17–272), one per
   256-color palette index: `init_pair(17+N, N, -1)` for N in 0..255.
   This is checked at runtime — if `COLOR_PAIRS < 273` we skip and fall back.

2. `Thumbnails::render_color()` runs `chafa --colors=256 --format=symbols`,
   then `parse_ansi()` walks the output character by character, tracking
   `\033[38;5;Nm` → fg=N and `\033[0m` → fg=reset, emitting `ThumbCell`
   structs (glyph + color_idx).

3. `TUI::draw_thumb()` renders each cell via:
   ```cpp
   attron(COLOR_PAIR(THUMB_COLOR_BASE + cell.color_idx));
   addstr(cell.glyph.c_str());
   attroff(COLOR_PAIR(THUMB_COLOR_BASE + cell.color_idx));
   ```
   ncurses fully owns every character and every color attribute.
   `erase()` clears everything correctly. Zero ghosting.

4. Falls back to `--colors=none` monochrome if:
   - `COLORS < 256` (terminal doesn't support 256 colors)
   - `COLOR_PAIRS < 273` (not enough pair slots)
   - `render_color()` returns empty (chafa or parse issue)

**Terminal compatibility:** works everywhere that supports 256 colors:
xterm-256color, gnome-terminal, macOS Terminal, iTerm2, blackbox, konsole,
WezTerm, foot, alacritty, etc.

## Also in EP1

- Install theme picker: 18 themes listed with scenic descriptions
- Theme chosen at install written to `~/.config/ytcui/config.json`
