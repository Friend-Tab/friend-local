/* fr_env.c — see fr_env.h. */

#include "fr_env.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

static Friend_String fr_env_string(const char *name)
{
    Friend_String s;
    s.data = getenv(name);
    s.len  = s.data ? (int32_t)strlen(s.data) : 0;
    if (s.len == 0) s.data = "";
    return s;
}

static const char *fr_env_path(const char *name)
{
    const char *value = getenv(name);
    return value && *value ? value : 0;
}

Fr_Env fr_env_read(void)
{
    Fr_Env      env;
    const char *named = getenv("FRIEND_LOCAL_PROVIDER");

    memset(&env, 0, sizeof(env));
    env.gemini_key = fr_env_string("GEMINI_KEY");
    env.openai_key = fr_env_string("OPENAI_KEY");
    env.base_url   = fr_env_string("FRIEND_LOCAL_BASE_URL");
    env.live_model = fr_env_string("FRIEND_LOCAL_LIVE_MODEL");
    env.ca_bundle  = fr_env_path("FRIEND_LOCAL_CA");
    env.directory  = fr_env_path("FRIEND_LOCAL_DIR");

    if (named && strcasecmp(named, "openai") == 0) {
        env.provider = FRIEND_PROVIDER_OPENAI_COMPATIBLE;
    } else if (named && strcasecmp(named, "gemini") == 0) {
        env.provider = FRIEND_PROVIDER_GEMINI;
    } else if (env.gemini_key.len == 0 && env.openai_key.len > 0) {
        env.provider = FRIEND_PROVIDER_OPENAI_COMPATIBLE;
    } else {
        env.provider = FRIEND_PROVIDER_GEMINI;
    }
    return env;
}

Friend_String fr_env_key(const Fr_Env *env, Friend_Provider provider)
{
    return provider == FRIEND_PROVIDER_OPENAI_COMPATIBLE ? env->openai_key
                                                         : env->gemini_key;
}

void fr_env_configure(const Fr_Env *env, Friend_Config *config)
{
    config->provider = env->provider;
    config->api_key  = fr_env_key(env, env->provider);
    if (env->base_url.len > 0) config->base_url = env->base_url;
    if (env->live_model.len > 0) config->models.live = env->live_model;
}
