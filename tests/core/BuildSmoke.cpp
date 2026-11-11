#include "core/BuildInfo.h"
#include <iostream>

#ifndef BRANCH_TRUCE_EXPECTED_VERSION
#error "BRANCH_TRUCE_EXPECTED_VERSION must name the resolved application version"
#endif

int main()
{
    if (branch_truce::version() != BRANCH_TRUCE_EXPECTED_VERSION) {
        std::cerr << "Build version mismatch: got " << branch_truce::version() << ", expected "
                  << BRANCH_TRUCE_EXPECTED_VERSION << "\n";
        return 1;
    }
    std::cout << "1 version assertion passed\n";
}
