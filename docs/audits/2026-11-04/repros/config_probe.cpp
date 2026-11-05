#include "config.h"
#include <iostream>
int main(){ytui::Config a;a.backend=ytui::BackendKind::YtcuiWe;a.cookies_file="/fixture/cookies.txt";a.sponsorblock=false;a.resume_playback=false;a.preferred_height=720;a.save();ytui::Config b;b.load();std::cout<<"backend="<<(int)b.backend<<" cookies="<<b.cookies_file<<" sponsorblock="<<b.sponsorblock<<" resume="<<b.resume_playback<<" height="<<b.preferred_height<<"\n";}
