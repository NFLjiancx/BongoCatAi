#include "bongo_cat/ai_key_store.h"
#include "bongo_cat/file.h"
#include "bongo_cat/path.h"

#include <string.h>
#include <sys/types.h>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#else
#include <stdio.h>
#endif

#define BONGO_CAT_AI_KEY_FILE "dashscope.key"

static bool key_path(const char *config_root, char *path, size_t capacity) {
    return config_root && config_root[0] &&
        bongo_cat_path_join(path, capacity, config_root,
            BONGO_CAT_AI_KEY_FILE);
}

bool bongo_cat_ai_key_store(const char *config_root, const char *key) {
    char path[BONGO_CAT_PATH_CAP];
    if (!key_path(config_root, path, sizeof(path))) return false;
    if (!key || !key[0]) {
        if (bongo_cat_file_remove(path)) return true;
        uint64_t size = 0;
        return !bongo_cat_path_file_size(path, &size);
    }
    size_t length = strlen(key);
    bool written = false;
#ifdef _WIN32
    FILE *file = bongo_cat_file_open(path, "wb");
    if (file) {
        written = fwrite(key, 1, length, file) == length;
        if (fclose(file) != 0) written = false;
    }
#else
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (descriptor >= 0) {
        written = write(descriptor, key, length) == (ssize_t)length;
        close(descriptor);
    }
#endif
    memset(path, 0, sizeof(path));
    return written;
}

bool bongo_cat_ai_key_present(const char *config_root) {
    char path[BONGO_CAT_PATH_CAP];
    if (!key_path(config_root, path, sizeof(path))) return false;
    uint64_t size = 0;
    return bongo_cat_path_file_size(path, &size) && size > 0;
}
