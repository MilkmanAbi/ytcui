#include <bits/stdc++.h>
#include "thumbs.h"
int main(int argc,char**argv){if(argc!=2)return 2;ytui::Thumbnails::download_async("fixture",argv[1]);while(waitpid(-1,nullptr,0)>0){} }
