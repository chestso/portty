#include "portty_bug.h"

#define PORTTY_BUG_MAX_DUMPERS 8

static void (*dumpers[PORTTY_BUG_MAX_DUMPERS])(void);
static int dumper_count = 0;

void portty_bug_register_dump(void (*fn)(void))
{
    if (!fn || dumper_count >= PORTTY_BUG_MAX_DUMPERS)
        return;
    /* Duplicate registrations are not deduplicated -- the gtk4 plugin
     * can be unloaded and reloaded across sessions, but within a single
     * run each init path registers once. */
    dumpers[dumper_count++] = fn;
}

void portty_bug_dump_state(void)
{
    for (int i = 0; i < dumper_count; ++i) {
        if (dumpers[i])
            dumpers[i]();
    }
}
