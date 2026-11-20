/* fr_tool.h — what friendchat and friendtalk both do before they start.
 *
 * Included rather than linked: each tool is one file built in one
 * compiler invocation, and everything here is static.
 */

#ifndef FR_TOOL_H
#define FR_TOOL_H

#include <stdio.h>
#include <stdlib.h>

#include "fr_clock.h"
#include "fr_env.h"
#include "fr_net.h"

/* The environment into config, and the wall clock's offset. Returns 0,
 * having said why, when there is no key to talk to a model with. */
static int tool_configure(const Fr_Env *env, Friend_Config *config)
{
    fr_env_configure(env, config);
    config->utc_offset_minutes = fr_local_utc_offset_minutes();
    if (config->api_key.len > 0) return 1;
    fprintf(stderr, "no key: set GEMINI_KEY or OPENAI_KEY\n");
    return 0;
}

/* The transport, in a block of its own. Null, having said why, when it
 * would not start; tool_net_end gives the block back either way. */
static Fr_Net *tool_net_begin(const Fr_Env *env, void **out_block)
{
    size_t  size = fr_net_size(FR_NET_CONNECTIONS);
    Fr_Net *net  = 0;
    char    why[256];

    *out_block = malloc(size);
    if (*out_block) {
        net = fr_net_begin(*out_block, size, FR_NET_CONNECTIONS,
                           env->ca_bundle, why, sizeof(why));
    } else {
        snprintf(why, sizeof(why), "there is no memory for it");
    }
    if (!net) fprintf(stderr, "the transport would not start: %s\n", why);
    return net;
}

static void tool_net_end(Fr_Net *net, void *block)
{
    fr_net_end(net);
    free(block);
}

#endif /* FR_TOOL_H */
