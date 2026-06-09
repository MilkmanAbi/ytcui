# Installation Guide

## Quick Start (Ubuntu/Debian)

```bash
# 1. Install dependencies
sudo apt update
sudo apt install -y g++ make libncurses-dev mpv chafa curl python3-pip

# 2. Install yt-dlp
pip install yt-dlp

# 3. Build and install ytcui
git clone <repository-url>
cd ytcui
make
sudo make install

# 4. Run
ytcui
```

## Distribution-Specific Instructions

### Arch Linux
```bash
sudo pacman -S gcc make ncurses mpv chafa curl python-pip
pip install yt-dlp
make
sudo make install
```

### Fedora
```bash
sudo dnf install gcc-c++ make ncurses-devel mpv chafa curl python3-pip
pip install yt-dlp
make
sudo make install
```

### openSUSE
```bash
sudo zypper install gcc-c++ make ncurses-devel mpv chafa curl python3-pip
pip install yt-dlp
make
sudo make install
```

### macOS (Homebrew)
```bash
brew install ncurses mpv chafa curl
pip3 install yt-dlp
make
make install PREFIX=/usr/local
```

## Manual Dependency Installation

### ncurses
**Ubuntu/Debian:**
```bash
sudo apt install libncurses-dev libncurses6
```

**Arch:**
```bash
sudo pacman -S ncurses
```

**Fedora:**
```bash
sudo dnf install ncurses-devel
```

### mpv
**Ubuntu/Debian:**
```bash
sudo apt install mpv
```

**Arch:**
```bash
sudo pacman -S mpv
```

**Fedora:**
```bash
sudo dnf install mpv
```

### chafa (optional, for thumbnails)
**Ubuntu/Debian:**
```bash
sudo apt install chafa
```

**Arch:**
```bash
sudo pacman -S chafa
```

**Fedora:**
```bash
sudo dnf install chafa
```

### yt-dlp
**Via pip (recommended):**
```bash
pip install yt-dlp
```

**Via package manager:**
```bash
# Ubuntu 22.04+
sudo apt install yt-dlp

# Arch
sudo pacman -S yt-dlp

# Fedora
sudo dnf install yt-dlp
```

## Building from Source

### Standard Build
```bash
make
```

### Debug Build
```bash
make CXXFLAGS="-std=c++17 -Wall -Wextra -g -Iinclude"
```

### Custom Install Location
```bash
make install PREFIX=$HOME/.local
```

Add to PATH:
```bash
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc
source ~/.bashrc
```

## Verifying Installation

```bash
# Check ytcui
ytcui --help

# Check dependencies
which yt-dlp
which mpv
which chafa  # optional

# Test UTF-8 support
locale | grep -i utf
```

## Troubleshooting

### "yt-dlp not found"
Make sure pip bin directory is in PATH:
```bash
export PATH="$HOME/.local/bin:$PATH"
```

### "ncurses.h not found" during compilation
Install development headers:
```bash
sudo apt install libncurses-dev
```

### mpv not working
Test mpv directly:
```bash
mpv https://www.youtube.com/watch?v=dQw4w9WgXcQ
```

If that fails, reinstall mpv:
```bash
sudo apt install --reinstall mpv
```

### Thumbnails not showing
1. Check chafa is installed:
   ```bash
   chafa --version
   ```

2. Test thumbnail rendering:
   ```bash
   curl -sL https://i.ytimg.com/vi/dQw4w9WgXcQ/mqdefault.jpg | chafa -s 40x20 -
   ```

### UTF-8 characters display as boxes
Set locale:
```bash
export LANG=en_US.UTF-8
export LC_ALL=en_US.UTF-8
```

Add to `~/.bashrc` to persist.

## Uninstalling

```bash
sudo make uninstall

# Remove config and data (optional)
rm -rf ~/.config/ytcui
rm -rf ~/.local/share/ytcui
rm -rf ~/.cache/ytcui
```

## Next Steps

After installation:
1. Run `ytcui` to start
2. Press `?` or see README.md for keyboard shortcuts
3. Try searching for a video
4. Press `s` for sort/filter options
5. Use browser login for age-restricted content

Enjoy! 🎬
