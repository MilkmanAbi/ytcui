# Project Structure

Complete file organization for ytcui project.

## Directory Tree

```
ytcui/
├── include/                    # Header files (.h)
│   ├── app.h                  # Main application controller
│   ├── auth.h                 # Browser cookie authentication
│   ├── config.h               # Configuration management
│   ├── input.h                # Keyboard & mouse input handling
│   ├── library.h              # Local data (bookmarks, history, subscriptions)
│   ├── log.h                  # Debug logging system
│   ├── player.h               # Media playback (mpv integration) [FIXED]
│   ├── thumbs.h               # Thumbnail download & rendering
│   ├── tui.h                  # Terminal UI rendering [FIXED]
│   ├── types.h                # Core data structures
│   └── youtube.h              # yt-dlp integration
│
├── src/                       # Source files (.cpp)
│   ├── app.cpp                # Application logic & action handling
│   ├── config.cpp             # Config file I/O
│   ├── input.cpp              # Input processing [FIXED]
│   ├── main.cpp               # Entry point & signal handling
│   ├── player.cpp             # Playback implementation [FIXED]
│   ├── tui.cpp                # UI rendering [FIXED]
│   └── youtube.cpp            # YouTube API wrapper
│
├── build/                     # Build artifacts (generated)
│   └── *.o                    # Object files
│
├── Makefile                   # Build system
├── README.md                  # Main documentation
├── INSTALL.md                 # Installation guide
├── CHANGELOG.md               # Version history & fixes
├── LICENSE                    # MIT License
├── docs_FIXES.md              # Detailed technical fix documentation
├── quick-start.sh             # Quick build & dependency check script
└── .gitignore                 # Git ignore rules
```

## File Descriptions

### Headers (include/)

#### app.h
**Purpose**: Main application controller interface
**Dependencies**: types.h, tui.h, youtube.h, player.h, input.h, config.h, library.h
**Key Components**:
- `class App` - Orchestrates all components
- Action execution system
- Search & playback management

#### auth.h
**Purpose**: Browser cookie authentication for age-restricted content
**Dependencies**: Standard library only
**Key Components**:
- `class Auth` - Static methods for browser detection
- Cookie extraction for yt-dlp
- Supported browsers: Firefox, Chrome, Chromium, Brave, Edge, Opera, Vivaldi

#### config.h
**Purpose**: Configuration file management
**Dependencies**: Standard library only
**Key Components**:
- `struct Config` - Configuration data
- JSON file I/O at `~/.config/ytcui/config.json`

#### input.h
**Purpose**: Input handling interface
**Dependencies**: types.h, ncurses
**Key Components**:
- `class InputHandler` - Processes keyboard & mouse events
- Vim-style navigation
- UTF-8 input validation [FIXED]

#### library.h
**Purpose**: Local data persistence
**Dependencies**: Standard library, nlohmann/json
**Key Components**:
- `class Library` - Bookmarks, subscriptions, history
- JSON storage at `~/.local/share/ytcui/`
- `struct BookmarkEntry` - Entry data structure

#### log.h
**Purpose**: Debug logging system
**Dependencies**: Standard library only
**Key Components**:
- `class Log` - Static logging interface
- Optional debug output to `~/.cache/ytcui/debug.log`
- Timestamp tracking

#### player.h [FIXED]
**Purpose**: Media playback interface
**Dependencies**: types.h, sys/prctl.h [NEW]
**Key Components**:
- `class Player` - mpv process management
- Audio/video/loop modes
- Pause/resume control
**Critical Fixes**:
- Added `prctl(PR_SET_PDEATHSIG)` for proper cleanup
- Removed process group detachment

#### thumbs.h
**Purpose**: Thumbnail management
**Dependencies**: Standard library only
**Key Components**:
- `class Thumbnails` - Static methods for thumbnail handling
- Async download with curl
- chafa rendering (kitty/sixel/halfblock)
- Cache at `~/.cache/ytcui/thumbs/`

#### tui.h [FIXED]
**Purpose**: Terminal UI rendering interface
**Dependencies**: types.h, library.h, ncurses
**Key Components**:
- `class TUI` - ncurses wrapper
- Multi-panel layout system
- UTF-8 display width calculation [FIXED]

#### types.h
**Purpose**: Core data structures
**Dependencies**: Standard library only
**Key Components**:
- `struct Video` - Video metadata
- `struct AppState` - Application state
- `enum class PlayMode` - Playback modes
- `enum class Panel` - UI panels
- `namespace Color` - Color definitions

#### youtube.h
**Purpose**: YouTube API wrapper interface
**Dependencies**: types.h
**Key Components**:
- `class YouTube` - yt-dlp integration
- Video search
- Stream URL extraction
- Metadata parsing

### Source Files (src/)

#### app.cpp (15.2 KB)
**Purpose**: Application logic implementation
**Key Functions**:
- `run()` - Main event loop
- `do_search()` - Execute YouTube search
- `execute_action()` - Handle user actions
- `build_actions()` - Generate action menu
- `do_save()` - Bookmark/download workflow

#### config.cpp (1.4 KB)
**Purpose**: Configuration file I/O
**Key Functions**:
- `load()` - Read config.json
- `save()` - Write config.json
- `config_dir()` - Get config directory path

#### input.cpp (13.9 KB) [FIXED]
**Purpose**: Input event processing
**Key Functions**:
- `handle()` - Main input dispatcher
- `handle_search_input()` - Search box input [FIXED UTF-8 validation]
- `handle_results_input()` - Result list navigation
- `handle_actions_input()` - Action menu navigation
- Mouse event handling

#### main.cpp (2.4 KB)
**Purpose**: Program entry point
**Key Functions**:
- `main()` - Setup, initialization, cleanup
- `signal_handler()` - SIGINT/SIGTERM handling
- Locale setup for UTF-8

#### player.cpp (6.7 KB) [FIXED]
**Purpose**: Media playback implementation
**Key Functions**:
- `play()` - Start playback
- `play_piped()` - Audio mode (yt-dlp → mpv) [FIXED]
- `play_direct()` - Video mode (mpv direct) [FIXED]
- `toggle_pause()` - Pause/resume [FIXED]
- `kill_mpv()` - Process cleanup [FIXED]
**Critical Fixes**:
- Proper process lifecycle with prctl
- No zombie processes

#### tui.cpp (29.0 KB) [FIXED]
**Purpose**: Terminal UI rendering
**Key Functions**:
- `render()` - Main render loop
- `draw_results_panel()` - Video list [FIXED bounds checking]
- `draw_info_panel()` - Video details [FIXED UTF-8 width]
- `draw_actions_panel()` - Action menu
- `utf8_display_width()` - Calculate display width [FIXED mbrtowc]
- `utf8_truncate()` - Safe text truncation [FIXED]
**Critical Fixes**:
- Proper UTF-8 wide character support
- Bounds checking on all rendering

#### youtube.cpp (6.5 KB)
**Purpose**: yt-dlp integration
**Key Functions**:
- `search()` - YouTube search
- `exec_ytdlp()` - Execute yt-dlp commands
- `parse_video_json()` - Parse metadata
- `format_views()` - View count formatting
- `format_duration()` - Duration formatting
- `sanitize_utf8()` - Clean invalid UTF-8

### Build System

#### Makefile (1.7 KB)
**Build Targets**:
- `all` - Build ytcui (default)
- `clean` - Remove build artifacts
- `install` - Install to system (default: /usr/local/bin)
- `uninstall` - Remove from system

**Build Flags**:
- C++17 standard
- `-Wall -Wextra` warnings
- `-O2` optimization
- Include path: `-Iinclude`
- Libraries: `-lncurses -lpthread`

**Directory Variables**:
- `SRC_DIR` = src/
- `INC_DIR` = include/
- `OBJ_DIR` = build/
- `BIN_DIR` = . (current directory)

### Documentation

#### README.md (9.3 KB)
Main user documentation covering:
- Features
- Installation
- Usage & keyboard shortcuts
- Configuration
- Troubleshooting
- Architecture overview

#### INSTALL.md (3.4 KB)
Detailed installation instructions for:
- Ubuntu/Debian, Arch, Fedora, openSUSE, macOS
- Manual dependency installation
- Build options
- Troubleshooting

#### CHANGELOG.md (4.7 KB)
Version history documenting:
- Critical bug fixes (zombies, UTF-8, clipping)
- Technical details of each fix
- Testing procedures
- Migration notes

#### docs_FIXES.md (15.8 KB)
In-depth technical documentation:
- Root cause analysis
- Code examples (before/after)
- Fix methodology
- Testing procedures
- Nuclear options

#### LICENSE (1.1 KB)
MIT License

### Scripts

#### quick-start.sh
**Purpose**: Automated setup & build
**Functions**:
- Dependency checking
- Build execution
- Quick start guide

**Usage**:
```bash
./quick-start.sh
```

### Configuration Files

#### .gitignore (460 bytes)
Git exclusions for:
- Build artifacts (build/, *.o, ytcui)
- IDE files (.vscode/, .idea/)
- User data (config.json, library.json)
- Thumbnails (thumbs/, *.jpg)
- Logs (*.log)

## Module Dependencies

```
main.cpp
  └─> App
       ├─> TUI
       │    └─> Library (optional)
       ├─> YouTube
       ├─> Player
       ├─> InputHandler
       ├─> Config
       ├─> Library
       └─> Log

All modules depend on types.h
```

## Data Flow

```
User Input → InputHandler → AppState → App → Action Execution
                                       ↓
                                      TUI ← Rendering
                                       ↓
YouTube Search → yt-dlp → JSON → Video List
                                       ↓
Video Selection → Action → Player → mpv
                                       ↓
                         Library → JSON Files
```

## Key Design Patterns

- **Single Responsibility**: Each class handles one major concern
- **Static Classes**: Auth, Log, Thumbnails (utility functions)
- **State Container**: AppState holds all UI state
- **Action-Based**: Commands executed through Action enum
- **Async Operations**: Thumbnail downloads, video streaming

## Critical Sections (Fixed)

### Process Management (player.cpp)
```cpp
// Child processes MUST die when parent dies
prctl(PR_SET_PDEATHSIG, SIGKILL);
```

### UTF-8 Display (tui.cpp)
```cpp
// MUST use mbrtowc with state, NOT mbtowc
mbstate_t state{};
mbrtowc(&wc, p, len, &state);
int width = wcwidth(wc);  // 2 for CJK!
```

### Input Validation (input.cpp)
```cpp
// MUST validate continuation bytes
if ((byte & 0xC0) != 0x80) {
    // Invalid UTF-8 - reject!
}
```

## File Size Summary

- **Total Headers**: 11 files, ~15 KB
- **Total Sources**: 7 files, ~75 KB
- **Total Code**: ~90 KB
- **Documentation**: ~30 KB
- **Total Project**: ~120 KB

## External Dependencies

**Required**:
- ncurses (≥6.0)
- yt-dlp (≥2023.03.04)
- mpv (≥0.33.0)
- C++17 compiler (GCC ≥7.0 or Clang ≥5.0)

**Optional**:
- chafa (≥1.8.0) - Thumbnails
- curl - Async downloads
- nlohmann/json (header-only, included in library.h)

## Build Requirements

- GNU Make
- Linux kernel ≥3.17 (for prctl support)
- libc6 ≥2.27
- libncurses6
- libstdc++6

## Runtime Data

**Config Directory**: `~/.config/ytcui/`
- config.json - Settings
- browser - Auth token

**Data Directory**: `~/.local/share/ytcui/`
- library.json - Bookmarks & subscriptions
- history.json - Watch history

**Cache Directory**: `~/.cache/ytcui/`
- thumbs/ - Thumbnail images
- debug.log - Debug output (with --debug)

---

**Complete project ready for compilation and distribution! 🚀**
