#include "deck_hint.h"

int deck_hint_slot(const bool *disabled, const bool *offline, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (disabled[i] && !offline[i]) return (int)i;
    }
    return -1;
}
