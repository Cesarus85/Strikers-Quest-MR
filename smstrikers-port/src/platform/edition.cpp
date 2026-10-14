// strikers-quest r43: see include/port/edition.h.

#include "port/edition.h"

#include <stdlib.h>

namespace
{

// "classic" in any case; anything else (and nothing at all: a start that did not come through the
// panel, an older panel) is Plus, the default.
int read_edition()
{
    const char* e = getenv("STRIKERS_EDITION");
    static const char kClassic[] = "classic";
    if (e == NULL)
        return 1;
    for (int i = 0;; i++)
    {
        char c = e[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != kClassic[i])
            return 1;
        if (c == '\0')
            return 0;
    }
}

} // namespace

extern "C" int PortPlusFeatures(void)
{
    // Once: the game thread, the mixer thread and a static initialiser (crowdsoundproperties.cpp)
    // all ask, and the answer must not change under a running match.
    static const int plus = read_edition();
    return plus;
}
