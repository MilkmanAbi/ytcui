#pragma once
// The share menu: put a link on the clipboard, or draw it as a QR code.
//
// Both halves are harder than they look for the same reason -- a terminal is
// not a desktop. There is no one clipboard API, and a QR code rendered in the
// wrong colours will not scan at all.
//
// Clipboard: OSC 52 is tried first, because it is the only method that works
// through SSH and inside tmux (with `set-clipboard on`) -- the terminal itself
// does the copying, so no local tool has to exist. Then the platform tools, in
// the order they are likely to be right.
//
// QR: encoded from the short youtu.be form, because fewer characters means a
// lower QR version, which means fewer modules, which means it still scans when
// each module is one terminal half-block. Rendered dark-on-light regardless of
// the active theme: a scanner expects dark modules on a light background, and
// inverting that is the single most common reason a terminal QR code fails to
// read.

#include <string>
#include <vector>

namespace ytcui::share {

// ---------------------------------------------------------------------------
// QR
// ---------------------------------------------------------------------------
struct QrMatrix {
    int size = 0;                   // modules per side, excluding the quiet zone
    int quiet = 4;                  // the spec's minimum quiet zone
    std::vector<bool> modules;      // size*size, true = dark

    bool at(int x, int y) const;    // false (light) outside the matrix
    // Including the quiet zone on all four sides.
    int  full_size() const { return size + quiet * 2; }
    // With the quiet zone applied, so (0,0) is the top-left of the drawable
    // area rather than of the symbol.
    bool at_padded(int x, int y) const;
};

// Encode text as a QR code. Returns size == 0 when the text is too long for
// the largest supported symbol.
QrMatrix encode(const std::string& text);

// Render as terminal rows using half-block glyphs, two module rows per text
// row. Each returned string is one line; 'X' marks dark, ' ' light, and the
// caller maps those onto colours. Provided so the matrix can be tested and
// rendered without FTXUI.
std::vector<std::string> to_ascii(const QrMatrix& m);

// Will a code for `text` fit in a terminal this big, drawn with half-blocks?
bool fits_in(const std::string& text, int cols, int rows);

// ---------------------------------------------------------------------------
// Clipboard
// ---------------------------------------------------------------------------
enum class CopyMethod { None, Osc52, WlCopy, Xclip, Xsel, PbCopy, Termux, Clip };

struct CopyResult {
    bool        ok = false;
    CopyMethod  method = CopyMethod::None;
    std::string detail;        // what was tried, for the status line
};

// Copy to the system clipboard. `allow_osc52` exists because a few terminals
// echo the escape sequence as text instead of acting on it, and a user who
// sees that wants to turn it off.
CopyResult copy(const std::string& text, bool allow_osc52 = true);

// The OSC 52 escape sequence for `text`, so a UI can emit it at the right
// moment in its own output stream rather than having this write to stdout
// underneath a live full-screen renderer.
std::string osc52_sequence(const std::string& text);

const char* to_string(CopyMethod m);

// Which external clipboard tools are actually present. For --diag.
std::vector<std::string> available_tools();

// ---------------------------------------------------------------------------
// Opening a link in the user's browser
// ---------------------------------------------------------------------------
// xdg-open / open / the BSDs' equivalents. Detached, so ytcui does not wait
// for a browser to exit.
bool open_in_browser(const std::string& url);

}  // namespace ytcui::share
