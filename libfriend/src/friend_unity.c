/* friend_unity.c — the whole library as one translation unit.
 *
 * There is no build system to configure and no object files to keep in
 * step: a platform's build script compiles this file and links it. Tests
 * include it directly, which is also how they get at internals without
 * exporting them. */

#include "../../base/fr_base.c"
#include "../../base/fr_json.c"
#include "fr_inflate.c"
#include "fr_init.c"
#include "fr_events.c"
#include "fr_graph.c"
#include "fr_state.c"
#include "fr_scroll.c"
#include "fr_blob.c"
#include "fr_prompt.c"
#include "fr_recall.c"
#include "fr_provider.c"
#include "fr_io.c"
#include "fr_rank.c"
#include "fr_turn.c"
#include "fr_voice.c"
#include "fr_live.c"
#include "fr_memory.c"
#include "fr_import.c"
#include "fr_save.c"
#include "fr_views.c"
