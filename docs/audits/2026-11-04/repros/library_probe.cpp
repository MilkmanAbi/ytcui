#include "library.h"
int main(){ytui::Library library;library.load();library.toggle_bookmark("new-item",std::string(4000,'X'),"fixture","fixture");}
