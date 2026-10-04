#ifndef BONGO_CAT_AI_KEY_STORE_H
#define BONGO_CAT_AI_KEY_STORE_H

#include "bongo_cat/common.h"

#ifdef __cplusplus
extern "C" {
#endif

bool bongo_cat_ai_key_store(const char *config_root, const char *key);
bool bongo_cat_ai_key_present(const char *config_root);

#ifdef __cplusplus
}
#endif

#endif
