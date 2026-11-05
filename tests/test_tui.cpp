#include "check.h"
#include "tui.h"
#include "input.h"
#include "termcaps.h"
#include "core/text.h"
#include "core/proc.h"
#include <clocale>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <chrono>
#include <unistd.h>

namespace ytui {
struct TuiTestAccess {
    static void adopt(TUI& tui, SCREEN* screen) {
        tui.screen_ = screen; tui.initialized_ = true;
        if (has_colors()) { start_color(); tui.default_bg_ = use_default_colors() == OK ? -1 : COLOR_BLACK; tui.init_thumb_colors(); }
    }
    static void share(TUI& tui, const AppState& state) { tui.setup_colors(state); tui.draw_share_menu(state); }
    static bool overlay_clears_graphics(TUI& tui, const AppState& state) {
        tui.pending_gfx_.push_back({"fixture",1,1,10,10});
        tui.draw_overlays(state);return tui.pending_gfx_.empty();
    }
    static int thumb_pairs(TUI& tui) { return tui.thumb_pairs_; }
    static int put(int y, int x, const char* text) { return TUI::put_at(y,x,text); }
};
}
using namespace ytui;
static void text_and_input() {
    using namespace ytcui::txt;
    CHECK(decode_utf8("\xe6" "A").back() == 'A');
    CHECK(decode_utf8("\xc0\xaf")[0] == 0xfffd);
    CHECK(decode_utf8("\xed\xa0\x80")[0] == 0xfffd);
    CHECK(decode_utf8("\xf4\x90\x80\x80")[0] == 0xfffd);
    CHECK(encode_utf8(0xd800) == "\xef\xbf\xbd");
    CHECK(terminal_sanitize("a\n\tb\x1b[31m") == "ab[31m");
    CHECK(terminal_sanitize("\xcc\x81" "a") == "a");
    CHECK(terminal_sanitize("a\xe2\x80\xae" "b") == "ab");
    for (int cols=0; cols<20; ++cols) CHECK(terminal_width(terminal_clip("日本語 accents e\xcc\x81 \xff",cols)) <= cols);
    CHECK(terminal_sanitize("日本語abc",false) == "abc");
    InputHandler input; AppState state; state.focus = Panel::Search;
    auto start = std::chrono::steady_clock::now();
    input.handle(0xe6,state); input.handle(ERR,state);
    CHECK(state.search_query.empty());
    input.handle(0x97,state); input.handle(0xa5,state);
    CHECK(state.search_query == "日");
    input.handle(0xe6,state); input.handle('x',state);
    CHECK(state.search_query == "日x");
    input.handle(0x80,state); CHECK(state.search_query == "日x");
    CHECK(std::chrono::steady_clock::now()-start < std::chrono::seconds(1));
    state.ui_mode=1; state.stream_screen=static_cast<int>(StreamScreen::Search);
    state.focus=Panel::ShareMenu; state.show_share=true; state.share_return_focus=Panel::Actions;
    input.handle(ERR,state); input.handle(KEY_RESIZE,state); CHECK(state.show_share);
    input.handle('m',state); CHECK(state.share_mode != 0);
    input.handle('c',state); CHECK(state.status_message == "__SHARE_COPY__");
    input.handle('q',state); CHECK(!state.show_share && state.focus==Panel::Actions);
    CHECK(state.search_query == "日x");
}
static void transport() {
    std::string text(262144,'X');
    auto result=ytcui::proc::run({"/bin/cat"},3000,text.size()+1,&text);
    CHECK(result.ok() && result.out==text);
    // Early stdin closure must not deliver a fatal SIGPIPE to the UI.
    result=ytcui::proc::run({"/bin/sh","-c","exit 0"},1000,4096,&text);
    CHECK(result.ran && !result.timed_out);
    result=ytcui::proc::run({"/bin/sh","-c","sleep 2"},100,4096,&text);
    CHECK(result.timed_out);
}
static void qr_rasters() {
    using namespace ytcui::share;
    auto matrix=encode("https://youtu.be/jNQXAC9IVRw");
    for (auto style:{QrStyle::Cells,QrStyle::HalfBlocks}) {
        auto raster=rasterize(matrix,style);
        CHECK(raster.cols>0 && raster.rows>0);
        for (int y=0;y<matrix.full_size();++y) for(int x=0;x<matrix.full_size();++x) {
            int index=style==QrStyle::Cells ? y*raster.cols+2*x : (y/2)*raster.cols+x;
            auto cell=raster.cells[index];
            bool dark=style==QrStyle::Cells ? cell==3 : (cell & (y%2 ? 1:2))!=0;
            CHECK(dark==matrix.at_padded(x,y));
        }
    }
}
static void clipboard_and_detection(const char* argv0) {
    char temporary[]="/tmp/ytcui-clipboard-XXXXXX";
    auto dir=mkdtemp(temporary); CHECK(dir!=nullptr); if(!dir)return;
    std::string self=ytcui::proc::self_exe();
    if(self.empty())self=std::filesystem::absolute(argv0).string();
    std::filesystem::create_symlink(self,std::string(dir)+"/wl-copy");
    std::string file=std::string(dir)+"/clipboard.txt";
    std::string old_path=getenv("PATH")?getenv("PATH"):"";
    setenv("PATH",dir,1);setenv("WAYLAND_DISPLAY","fixture",1);setenv("TUI_CLIPBOARD_FIXTURE",file.c_str(),1);
    auto started=std::chrono::steady_clock::now();
    auto copied=ytcui::share::copy("fixture clipboard payload",false);
    CHECK(std::chrono::steady_clock::now()-started<std::chrono::seconds(1));
    CHECK(copied.ok && copied.method==ytcui::share::CopyMethod::WlCopy);
    std::ifstream input(file);std::string value((std::istreambuf_iterator<char>(input)),{});
    CHECK(value=="fixture clipboard payload");
    setenv("PATH",old_path.c_str(),1);
    std::filesystem::remove_all(dir);
    setenv("TERM","xterm-256color",1);setenv("TERM_PROGRAM","Apple_Terminal",1);setenv("COLORTERM","truecolor",1);
    unsetenv("TMUX");unsetenv("MLTERM");TermCaps::detect();
    CHECK(TermCaps::get().id==TermId::AppleTerminal && !TermCaps::get().truecolor);
    setenv("TMUX","fixture",1);TermCaps::detect();CHECK(TermCaps::get().id==TermId::Tmux);
    unsetenv("TMUX");unsetenv("TERM_PROGRAM");setenv("MLTERM","fixture",1);
    TermCaps::detect();CHECK(TermCaps::get().mono_hardening);
    unsetenv("MLTERM");TermCaps::detect();CHECK(!TermCaps::get().mono_hardening);
}

static int pty_session(const char* report) {
    auto& caps=TermCaps::get();caps.id=TermId::AppleTerminal;caps.unicode=true;
    termios before{},after{};tcgetattr(STDIN_FILENO,&before);
    TUI tui;if(!tui.init())return 2;
    InputHandler input;AppState state;state.show_share=true;state.focus=Panel::ShareMenu;
    state.share_url="https://youtu.be/jNQXAC9IVRw";state.resolved_colors=get_theme_colors(Theme::Default);
    int frames=0,resizes=0;
    while(state.show_share && frames++<120) {
        tui.get_dimensions(state.term_w,state.term_h);tui.render(state,nullptr);
        int key=getch();if(key==KEY_RESIZE)++resizes;
        input.handle(key,state);
    }
    tui.shutdown();tcgetattr(STDIN_FILENO,&after);
    bool restored=before.c_iflag==after.c_iflag && before.c_oflag==after.c_oflag && before.c_lflag==after.c_lflag;
    std::ofstream result(report);result<<(restored?"restored":"changed")<<" "<<resizes<<" "<<state.show_share;
    return restored && !state.show_share && resizes>0 ? 0:1;
}

static void render_matrix(const char* terminal, const std::string& pgm) {
    FILE* in=tmpfile(); FILE* out=tmpfile();
    SCREEN* screen=newterm(terminal,out,in);
    CHECK(screen!=nullptr); if(!screen) { fclose(in);fclose(out);return; }
    set_term(screen);
    {
        TUI tui; TuiTestAccess::adopt(tui,screen);
        auto& caps=TermCaps::get(); caps.unicode=true; caps.id=TermId::AppleTerminal; caps.colors=COLORS;
        AppState state; state.resolved_colors=get_theme_colors(Theme::Default);
        state.share_url="https://youtu.be/jNQXAC9IVRw"; state.show_share=true; state.focus=Panel::ShareMenu;
        resizeterm(55,100); state.term_w=100;state.term_h=55;
        erase();TuiTestAccess::share(tui,state);
        auto matrix=ytcui::share::encode(state.share_url);
        auto raster=ytcui::share::rasterize(matrix,ytcui::share::QrStyle::Cells);
        int pw=std::max(36,raster.cols+4), ph=raster.rows+4;
        int x=(100-pw)/2+(pw-raster.cols)/2, y=(55-ph)/2+1;
        if(COLORS>=8) {
            short fg,bg; CHECK(pair_content(Color::QR,&fg,&bg)==OK); CHECK(fg==COLOR_BLACK && bg==COLOR_WHITE);
            std::ofstream image;
            if(!pgm.empty()) { image.open(pgm,std::ios::binary);image<<"P5\n"<<raster.cols*8<<" "<<raster.rows*16<<"\n255\n"; }
            for(int row=0;row<raster.rows;++row) {
                std::string pixels;
                for(int col=0;col<raster.cols;++col) {
                    chtype ch=mvinch(y+row,x+col);
                    CHECK((ch&A_CHARTEXT)==' '); CHECK(PAIR_NUMBER(ch)==Color::QR);
                    bool dark=(ch&A_REVERSE)!=0;
                    CHECK(dark==(raster.cells[row*raster.cols+col]==3));
                    pixels.append(8,dark ? '\0' : '\xff');
                }
                if(image) for(int i=0;i<16;++i) image.write(pixels.data(),pixels.size());
            }
        }
        // The complete compact symbol (including quiet zone) fits a classic 80x24.
        if(COLORS>=8) {
            resizeterm(24,80);state.term_w=80;state.term_h=24;state.share_mode=2;
            erase();TuiTestAccess::share(tui,state);
            auto compact=ytcui::share::rasterize(matrix,ytcui::share::QrStyle::HalfBlocks);
            int cw=std::max(36,compact.cols+4),ch=compact.rows+4;
            CHECK(ch<=24);
            int cx=(80-cw)/2+(cw-compact.cols)/2, cy=(24-ch)/2+1;
            for(int row:{0,compact.rows-1})for(int col=0;col<compact.cols;++col) {
                auto cell=mvinch(cy+row,cx+col);
                CHECK((cell&A_CHARTEXT)==' ' && PAIR_NUMBER(cell)==Color::QR && !(cell&A_REVERSE));
            }
            resizeterm(55,100);state.term_w=100;state.term_h=55;state.share_mode=0;
        }
        // A browser modal is visible in streamlined mode as well as normal mode.
        state.show_share=false;state.focus=Panel::BrowserPick;state.browser_choices={"firefox","chrome"};state.ui_mode=1;
        tui.render(state,nullptr);
        std::string screen_text;
        for(int row=0;row<55;++row)for(int col=0;col<100;++col)screen_text+=static_cast<char>(mvinch(row,col)&A_CHARTEXT);
        CHECK(screen_text.find("Select Browser")!=std::string::npos);
        CHECK(TuiTestAccess::overlay_clears_graphics(tui,state));
        if(COLORS==8 || COLORS==16) {
            short fg,bg;CHECK(pair_content(Color::ACCENT,&fg,&bg)==OK);CHECK(fg==3);
        }
        InputHandler modal_input;MEVENT event{};event.x=3;event.y=4;event.bstate=BUTTON1_PRESSED;
        state.active_tab=Tab::Results;ungetmouse(&event);modal_input.handle(KEY_MOUSE,state);
        CHECK(state.active_tab==Tab::Results && state.focus==Panel::BrowserPick);
        CHECK(TuiTestAccess::thumb_pairs(tui)<=237);
        for(int pair=19;pair<19+TuiTestAccess::thumb_pairs(tui);++pair) CHECK(PAIR_NUMBER(COLOR_PAIR(pair))==pair);
        InputHandler input;
        for(auto dims:{std::pair<int,int>{3,1},{12,4},{20,6},{39,12},{80,24},{80,40},{120,50}}) {
            resizeterm(dims.second,dims.first);state.term_w=dims.first;state.term_h=dims.second;
            Video video;video.id="fixture";video.title="日本語 \n\x1b[31m title";state.results={video};
            state.active_tab=Tab::Results;state.selected_result=-1;state.results_scroll=-3;
            state.res_labels={"Auto","1080p","720p"};state.res_heights={0,1080,720};state.res_sel=-1;
            state.queue_items={video};state.actions={{Action::Share,"Share"}};
            for(int mode:{0,1}) for(int theme:{static_cast<int>(Theme::Default),static_cast<int>(Theme::Grayscale),static_cast<int>(Theme::MLterm)}) {
                state.ui_mode=mode;state.theme=static_cast<Theme>(theme);state.resolved_colors=get_theme_colors(state.theme);
                for(auto focus:{Panel::Results,Panel::ShareMenu,Panel::ResMenu,Panel::QueueView,Panel::SortMenu,Panel::BrowserPick,Panel::SavePrompt,Panel::NewPlaylist}) {
                    state.focus=focus;state.show_share=focus==Panel::ShareMenu;state.show_res_menu=focus==Panel::ResMenu;
                    state.show_queue=focus==Panel::QueueView;
                    for(int stream=0;stream<=static_cast<int>(StreamScreen::MoreActions);++stream) {
                        state.stream_screen=stream;tui.render(state,nullptr);
                    }
                }
            }
        }
        resizeterm(8,20);erase();
        TuiTestAccess::put(1,19,"AB\nCD");
        CHECK((mvinch(1,19)&A_CHARTEXT)=='A');CHECK((mvinch(2,0)&A_CHARTEXT)==' ');
        CHECK(TuiTestAccess::put(-1,0,"invalid")==ERR);
    }
    fclose(in);fclose(out);
}
int main(int argc,char**argv) {
    if(std::filesystem::path(argv[0]).filename()=="wl-copy") {
        { std::ofstream file(getenv("TUI_CLIPBOARD_FIXTURE"));file<<std::cin.rdbuf(); }
        // Model wl-copy/xclip: a background owner retains inherited stdio.
        if(fork()==0) { std::this_thread::sleep_for(std::chrono::seconds(2));_exit(0); }
        return 0;
    }
    if(!setlocale(LC_ALL,"C.UTF-8")) setlocale(LC_ALL,"en_US.UTF-8");
    if(argc>2 && std::string(argv[1])=="--pty")return pty_session(argv[2]);
    text_and_input();transport();qr_rasters();clipboard_and_detection(argv[0]);
    render_matrix("xterm-256color",argc>1?argv[1]:"");
    render_matrix("xterm","");render_matrix("vt100","");
    return tst::finish("TUI rendering/input audit");
}
