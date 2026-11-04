#include "app.h"
#include <atomic>
#include <thread>
#include <chrono>
#include <iostream>
static std::atomic<bool> entered{false}, release_fetch{false};
extern "C" ytcui::sb::Segments __wrap__ZN5ytcui2sb5fetchERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES8_i(const std::string&, const std::string&, int) {
    entered = true;
    while (!release_fetch) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return {};
}
int main() {
    auto* app = new ytui::App(ytui::Theme::Default);
    app->config_.sponsorblock = true;
    app->start_sb_fetch("audit-fixture");
    while (!entered) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    delete app;
    release_fetch = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::cout << "probe finished\n";
}
