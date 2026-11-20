/* fr_env.h — what the environment says about running a friend.
 *
 * Every desktop program here reads the same variables, and reads them
 * the same way, through this:
 *
 *   GEMINI_KEY, OPENAI_KEY  the keys. The environment, never a command
 *                           line: an argument list is visible to every
 *                           process on the machine.
 *   FRIEND_LOCAL_PROVIDER   "gemini" or "openai", to choose outright.
 *   FRIEND_LOCAL_BASE_URL   an OpenAI-compatible server of your own.
 *   FRIEND_LOCAL_CA         the CA bundle TLS verifies against.
 *   FRIEND_LOCAL_LIVE_MODEL the realtime model, for trying one against
 *                           another without rebuilding.
 *   FRIEND_LOCAL_DIR        where a frontend keeps its friends.
 *
 * Precedence, decided once:
 *   - The provider is FRIEND_LOCAL_PROVIDER when it names one. Otherwise
 *     it is Gemini when GEMINI_KEY is set, OpenAI when only OPENAI_KEY
 *     is, and Gemini when neither is.
 *   - The key is the chosen provider's variable, and nothing else: an
 *     OpenAI key is never sent to Gemini because it happened to be set.
 *   - The environment seeds; a frontend's own settings beat it. A key or
 *     address somebody typed into a settings sheet is what they meant
 *     right now, so a frontend applies its settings after
 *     fr_env_configure, and keeps an environment key out of any file.
 */

#ifndef FR_ENV_H
#define FR_ENV_H

#include "friend.h"

typedef struct Fr_Env {
    Friend_Provider provider;
    Friend_String   gemini_key;
    Friend_String   openai_key;
    Friend_String   base_url;
    Friend_String   live_model;
    const char     *ca_bundle;      /* null: the system's */
    const char     *directory;      /* null: the frontend's own choice */
} Fr_Env;

/* The strings point into the environment, which outlives everything
 * that reads it here. */
Fr_Env fr_env_read(void);

/* This provider's key from the environment, or empty. */
Friend_String fr_env_key(const Fr_Env *env, Friend_Provider provider);

/* Provider, key, and whichever of the base URL and realtime model are
 * set, into a configuration. */
void fr_env_configure(const Fr_Env *env, Friend_Config *config);

#endif /* FR_ENV_H */
