#include "core/proc.h"
#include <chrono>
#include <iostream>
int main(){auto start=std::chrono::steady_clock::now();auto r=ytcui::proc::run({"/bin/sh","-c","exec 1>&- 2>&-; sleep 0.7"},100);std::cout<<"elapsed_ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count()<<" timed_out="<<r.timed_out<<" exit="<<r.exit_code<<"\n";}
