#include "core/BuildInfo.h"
#include <iostream>

int main()
{
    if (branch_truce::version() != "0.1.0") {
        std::cerr << "Build version mismatch\n";
        return 1;
    }
    std::cout << "1 version assertion passed\n";
}
