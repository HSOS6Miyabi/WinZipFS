#include "../src/api.h"
#include <iostream>
int main() {
    WzOptions o{};wz_default_options(&o);
    if(o.delaySeconds!=5 || o.level!=5 || wz_count()!=0) return 1;
    wchar_t mount[16]{},err[512]{};
    if(wz_mount(L"Z:\\file-that-does-not-exist.zip",&o,mount,16)) return 2;
    wz_error(err,512);if(!*err || wz_count()!=0) return 3;
    if(wz_eject(L"Z:") || wz_flush(L"Z:")) return 4;
    if(!wz_shutdown()) return 5;
    std::cout<<"Core API checks passed\n";
    return 0;
}
