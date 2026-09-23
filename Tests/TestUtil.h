#pragma once

#include <iostream>
#include <string>

namespace testutil
{
    inline int failures = 0;

    inline void check (bool cond, const std::string& what)
    {
        std::cout << (cond ? "  PASS  " : "  FAIL  ") << what << "\n";
        if (! cond) ++failures;
    }

    inline int finish (const char* suite)
    {
        std::cout << (failures == 0 ? "\n" : "\n*** ") << suite << ": "
                  << (failures == 0 ? "all passed" : std::to_string (failures) + " failure(s)") << "\n";
        return failures == 0 ? 0 : 1;
    }
}
