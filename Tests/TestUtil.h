#pragma once

#include <cstdlib>
#include <iostream>
#include <string>

namespace testutil
{
    inline int failures = 0;

    // On GitHub Actions, failures are also emitted as ::error annotations: those
    // are readable through the public API, unlike the job log.
    inline void check (bool cond, const std::string& what)
    {
        std::cout << (cond ? "  PASS  " : "  FAIL  ") << what << "\n";
        if (cond) return;
        ++failures;
        if (std::getenv ("GITHUB_ACTIONS") != nullptr)
        {
            std::string msg;
            for (char c : what)
                msg += c == '%' ? std::string ("%25") : c == '\n' ? std::string ("%0A") : std::string (1, c);
            std::cout << "::error title=Test failure::" << msg << std::endl;
        }
    }

    inline int finish (const char* suite)
    {
        std::cout << (failures == 0 ? "\n" : "\n*** ") << suite << ": "
                  << (failures == 0 ? "all passed" : std::to_string (failures) + " failure(s)") << "\n";
        return failures == 0 ? 0 : 1;
    }
}
