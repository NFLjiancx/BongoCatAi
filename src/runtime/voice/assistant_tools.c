#include "assistant_internal.h"
#include "bongo_cat/file.h"
#include "bongo_cat/model.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_process.h>
#include <yyjson.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COMMAND_TIMEOUT_MS (90 * 1000)

/* One tool parameter: name, JSON type and description. */
typedef struct {
    const char *name;
    const char *type;
    const char *description;
} AssistantParam;

static yyjson_mut_val *assistant_tool_entry(yyjson_mut_doc *doc,
    const char *name, const char *description, const AssistantParam *params,
    size_t count, const char *const *required, size_t required_count) {
    yyjson_mut_val *entry = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, entry, "type", "function");
    yyjson_mut_val *function = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, function, "name", name);
    yyjson_mut_obj_add_str(doc, function, "description", description);
    yyjson_mut_val *parameters = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, parameters, "type", "object");
    yyjson_mut_val *properties = yyjson_mut_obj(doc);
    for (size_t i = 0; i < count; ++i) {
        yyjson_mut_val *property = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, property, "type", params[i].type);
        if (params[i].description)
            yyjson_mut_obj_add_str(doc, property, "description",
                params[i].description);
        yyjson_mut_obj_add_val(doc, properties, params[i].name, property);
    }
    yyjson_mut_obj_add_val(doc, parameters, "properties", properties);
    if (required_count) {
        yyjson_mut_val *list = yyjson_mut_arr(doc);
        for (size_t i = 0; i < required_count; ++i)
            yyjson_mut_arr_add_str(doc, list, required[i]);
        yyjson_mut_obj_add_val(doc, parameters, "required", list);
    }
    yyjson_mut_obj_add_val(doc, function, "parameters", parameters);
    yyjson_mut_obj_add_val(doc, entry, "function", function);
    return entry;
}

#define ASSISTANT_PARAM(NAME, TYPE, WHAT) \
    {NAME, TYPE, WHAT}

/* OpenAI-format tool list sent in every chat request; caller frees. */
char *bongo_cat_assistant_tools_json(void) {
    static const AssistantParam name_one[] = {
        ASSISTANT_PARAM("name", "string", "Behavior id or label")};
    static const AssistantParam visible_one[] = {
        ASSISTANT_PARAM("visible", "boolean", "true to show, false to hide")};
    static const AssistantParam enabled_one[] = {
        ASSISTANT_PARAM("enabled", "boolean", "true to enable, false to mute")};
    static const AssistantParam path_one[] = {
        ASSISTANT_PARAM("path", "string", "File or directory path")};
    static const AssistantParam write_two[] = {
        ASSISTANT_PARAM("path", "string", "File path"),
        ASSISTANT_PARAM("content", "string", "UTF-8 file content")};
    static const AssistantParam command_one[] = {
        ASSISTANT_PARAM("command", "string", "Shell command line")};
    static const char *const req_name[] = {"name"};
    static const char *const req_visible[] = {"visible"};
    static const char *const req_enabled[] = {"enabled"};
    static const char *const req_path[] = {"path"};
    static const char *const req_write[] = {"path", "content"};
    static const char *const req_command[] = {"command"};

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *tools = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, tools);
    yyjson_mut_arr_add_val(tools, assistant_tool_entry(doc,
        "get_datetime", "Get the local date and time now.", NULL, 0, NULL, 0));
    yyjson_mut_arr_add_val(tools, assistant_tool_entry(doc,
        "list_behaviors", "List the pet's available behaviors as "
        "id-and-label lines.", NULL, 0, NULL, 0));
    yyjson_mut_arr_add_val(tools, assistant_tool_entry(doc,
        "play_behavior", "Play one pet behavior by id or label "
        "(case-insensitive substring match).", name_one, 1, req_name, 1));
    yyjson_mut_arr_add_val(tools, assistant_tool_entry(doc,
        "set_pet_visibility", "Show or hide the pet window.", visible_one, 1,
        req_visible, 1));
    yyjson_mut_arr_add_val(tools, assistant_tool_entry(doc,
        "set_sound_enabled", "Turn the pet's sound effects on or off for "
        "this session.", enabled_one, 1, req_enabled, 1));
    yyjson_mut_arr_add_val(tools, assistant_tool_entry(doc,
        "read_file", "Read a UTF-8 text file, returning at most 32KB.",
        path_one, 1, req_path, 1));
    yyjson_mut_arr_add_val(tools, assistant_tool_entry(doc,
        "write_file", "Create or overwrite a text file (parent directory "
        "must exist). Empty content deletes it.", write_two, 2, req_write, 2));
    yyjson_mut_arr_add_val(tools, assistant_tool_entry(doc,
        "list_directory", "List files and subdirectories of a directory.",
        path_one, 1, req_path, 1));
    yyjson_mut_arr_add_val(tools, assistant_tool_entry(doc,
        "run_command", "Run a shell command (/bin/sh -c, cmd.exe /c on "
        "Windows) and return combined output. Takes up to 90 seconds.",
        command_one, 1, req_command, 1));
    char *text = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return text;
}

bool bongo_cat_assistant_arg_string(const char *arguments, const char *key,
    char *out, size_t cap) {
    if (!arguments) return false;
    yyjson_doc *doc = yyjson_read(arguments, SDL_strlen(arguments), 0);
    bool found = false;
    if (doc) {
        yyjson_val *value = yyjson_obj_get(yyjson_doc_get_root(doc), key);
        if (yyjson_is_str(value)) {
            SDL_strlcpy(out, yyjson_get_str(value), cap);
            found = true;
        }
        yyjson_doc_free(doc);
    }
    return found;
}

static bool arg_bool(const char *arguments, const char *key, bool *value) {
    if (!arguments) return false;
    yyjson_doc *doc = yyjson_read(arguments, SDL_strlen(arguments), 0);
    bool found = false;
    if (doc) {
        yyjson_val *item = yyjson_obj_get(yyjson_doc_get_root(doc), key);
        if (yyjson_is_bool(item)) {
            *value = yyjson_get_bool(item);
            found = true;
        }
        yyjson_doc_free(doc);
    }
    return found;
}

void bongo_cat_assistant_now(char *out, size_t cap) {
    SDL_Time ticks = 0;
    SDL_DateTime date = {0};
    if (!SDL_GetCurrentTime(&ticks) ||
        !SDL_TimeToDateTime(ticks, &date, true)) {
        snprintf(out, cap, "error: clock unavailable");
        return;
    }
    snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d (weekday %d)",
        date.year, date.month, date.day, date.hour, date.minute,
        date.second, date.day_of_week);
}

/* Hand one pet/settings action to the main thread and wait for its
   result string; bongo_cat_assistant_pump performs the work. */
static bool request_action(BongoCatAssistantRunner *runner,
    enum AssistantActionKind kind, const char *arg, bool flag,
    char *out, size_t cap) {
    SDL_LockMutex(runner->action_mutex);
    runner->action.kind = kind;
    SDL_strlcpy(runner->action.arg, arg ? arg : "",
        sizeof(runner->action.arg));
    runner->action.flag = flag;
    runner->action.result[0] = '\0';
    runner->action.done = false;
    runner->action.pending = true;
    SDL_UnlockMutex(runner->action_mutex);
    bool ok = false;
    while (!ok && !SDL_GetAtomicInt(&runner->cancel)) {
        SDL_LockMutex(runner->action_mutex);
        while (!runner->action.done && !SDL_GetAtomicInt(&runner->cancel))
            SDL_WaitConditionTimeout(runner->action_cond,
                runner->action_mutex, 100);
        if (runner->action.done) {
            SDL_strlcpy(out, runner->action.result, cap);
            runner->action.pending = false;
            ok = true;
        }
        SDL_UnlockMutex(runner->action_mutex);
    }
    return ok;
}

static void tool_read_file(const char *arguments, char *out, size_t cap) {
    char path[ASSISTANT_PROMPT_CAP];
    if (!bongo_cat_assistant_arg_string(arguments, "path", path,
        sizeof(path))) {
        snprintf(out, cap, "error: missing path");
        return;
    }
    size_t size = 0;
    void *data = SDL_LoadFile(path, &size);
    if (!data) {
        snprintf(out, cap, "error: %s", SDL_GetError());
        return;
    }
    if (size > ASSISTANT_FILE_READ_CAP) size = ASSISTANT_FILE_READ_CAP;
    size_t length = size < cap - 1 ? size : cap - 1;
    SDL_memcpy(out, data, length);
    out[length] = '\0';
    SDL_free(data);
}

static void tool_write_file(const char *arguments, char *out, size_t cap) {
    char path[ASSISTANT_PROMPT_CAP];
    char content[ASSISTANT_PROMPT_CAP * 2];
    if (!bongo_cat_assistant_arg_string(arguments, "path", path,
        sizeof(path))) {
        snprintf(out, cap, "error: missing path");
        return;
    }
    if (!bongo_cat_assistant_arg_string(arguments, "content", content,
        sizeof(content))) {
        snprintf(out, cap, "error: missing content");
        return;
    }
    bool removed = !content[0];
    if (removed) {
        if (bongo_cat_file_remove(path)) snprintf(out, cap, "deleted");
        else snprintf(out, cap, "error: remove failed: %s",
            SDL_GetError());
        return;
    }
    FILE *file = fopen(path, "wb");
    if (!file || fwrite(content, 1, SDL_strlen(content), file)
        != SDL_strlen(content)) {
        if (file) fclose(file);
        snprintf(out, cap, "error: %s", SDL_GetError());
        return;
    }
    fclose(file);
    snprintf(out, cap, "wrote %u bytes",
        (unsigned)SDL_strlen(content));
}

static void tool_list_directory(const char *arguments, char *out,
    size_t cap) {
    char path[ASSISTANT_PROMPT_CAP];
    if (!bongo_cat_assistant_arg_string(arguments, "path", path,
        sizeof(path)))
        path[0] = '\0';
    if (!path[0]) SDL_strlcpy(path, ".", sizeof(path));
    int count = 0;
    char **names = SDL_GlobDirectory(path, "*", 0, &count);
    if (!names) {
        snprintf(out, cap, "error: %s", SDL_GetError());
        return;
    }
    out[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < count && used < cap - 2; ++i) {
        used += (size_t)snprintf(out + used, cap - used, "%s\n", names[i]);
        SDL_free(names[i]);
    }
    SDL_free(names);
    if (!used) snprintf(out, cap, "(empty)");
}

static char *drain_process_io(SDL_IOStream *stream, SDL_AtomicInt *cancel) {
    char *buffer = SDL_malloc(ASSISTANT_TOOL_RESULT_CAP);
    size_t length = 0;
    if (!buffer) return NULL;
    buffer[0] = '\0';
    Uint64 deadline = SDL_GetTicksNS()
        + (Uint64)COMMAND_TIMEOUT_MS * 1000000ull;
    while (stream && length < ASSISTANT_TOOL_RESULT_CAP - 1
        && SDL_GetTicksNS() < deadline && !SDL_GetAtomicInt(cancel)) {
        size_t room = ASSISTANT_TOOL_RESULT_CAP - 1 - length;
        size_t want = room < 1024 ? room : 1024;
        size_t read = SDL_ReadIO(stream, buffer + length, want);
        if (read == 0) {
            SDL_IOStatus status = SDL_GetIOStatus(stream);
            if (status == SDL_IO_STATUS_EOF) break;
            if (status == SDL_IO_STATUS_NOT_READY) { SDL_Delay(50);
                continue; }
            break;
        }
        length += read;
        buffer[length] = '\0';
    }
    return buffer;
}

static void tool_run_command(BongoCatAssistantRunner *runner,
    const char *arguments, char *out, size_t cap) {
    char command[ASSISTANT_PROMPT_CAP];
    if (!bongo_cat_assistant_arg_string(arguments, "command", command,
        sizeof(command))) {
        snprintf(out, cap, "error: missing command");
        return;
    }
#ifdef _WIN32
    const char *shell_args[4] = {"cmd.exe", "/c", command, NULL};
    const char *merged = NULL;
#else
    char *merged = SDL_malloc(SDL_strlen(command) + 8);
    if (!merged) { snprintf(out, cap, "error: out of memory"); return; }
    SDL_snprintf(merged, SDL_strlen(command) + 8, "%s 2>&1", command);
    const char *shell_args[4] = {"/bin/sh", "-c", merged, NULL};
#endif
    SDL_Process *process = NULL;
    SDL_PropertiesID properties = SDL_CreateProperties();
    if (properties) {
        if (SDL_SetPointerProperty(properties,
            SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)shell_args) &&
            SDL_SetNumberProperty(properties,
                SDL_PROP_PROCESS_CREATE_STDIN_NUMBER,
                SDL_PROCESS_STDIO_NULL) &&
            SDL_SetNumberProperty(properties,
                SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
                SDL_PROCESS_STDIO_APP) &&
            SDL_SetNumberProperty(properties,
                SDL_PROP_PROCESS_CREATE_STDERR_NUMBER,
                SDL_PROCESS_STDIO_NULL))
            process = SDL_CreateProcessWithProperties(properties);
        SDL_DestroyProperties(properties);
    }
#ifndef _WIN32
    SDL_free(merged);
#endif
    if (!process) {
        snprintf(out, cap, "error: %s", SDL_GetError());
        return;
    }
    char *output = drain_process_io(SDL_GetProcessOutput(process),
        &runner->cancel);
    int exit_code = -1;
    bool finished = SDL_WaitProcess(process, false, &exit_code);
    if (!finished) {
        SDL_KillProcess(process, true);
        snprintf(out, cap, "error: command timed out");
    } else if (!output) {
        snprintf(out, cap, "error: out of memory");
    } else {
        snprintf(out, cap, "exit=%d\n%s", exit_code, output);
        SDL_free(output);
    }
    SDL_DestroyProcess(process);
}

void bongo_cat_assistant_tool_execute(BongoCatAssistantRunner *runner,
    const char *name, const char *arguments, char *out, size_t cap) {
    out[0] = '\0';
    if (!SDL_strcmp(name, "get_datetime")) {
        bongo_cat_assistant_now(out, cap);
    } else if (!SDL_strcmp(name, "list_behaviors")) {
        request_action(runner, ACTION_LIST_BEHAVIORS, NULL, false, out, cap);
    } else if (!SDL_strcmp(name, "play_behavior")) {
        char behavior[512];
        if (!bongo_cat_assistant_arg_string(arguments, "name", behavior,
            sizeof(behavior)))
            snprintf(out, cap, "error: missing name");
        else
            request_action(runner, ACTION_PLAY_BEHAVIOR, behavior, false,
                out, cap);
    } else if (!SDL_strcmp(name, "set_pet_visibility")) {
        bool visible = true;
        if (!arg_bool(arguments, "visible", &visible))
            snprintf(out, cap, "error: missing visible");
        else
            request_action(runner, ACTION_SET_VISIBILITY, NULL, visible,
                out, cap);
    } else if (!SDL_strcmp(name, "set_sound_enabled")) {
        bool enabled = true;
        if (!arg_bool(arguments, "enabled", &enabled))
            snprintf(out, cap, "error: missing enabled");
        else
            request_action(runner, ACTION_SET_SOUND, NULL, enabled, out,
                cap);
    } else if (!SDL_strcmp(name, "read_file")) {
        tool_read_file(arguments, out, cap);
    } else if (!SDL_strcmp(name, "write_file")) {
        tool_write_file(arguments, out, cap);
    } else if (!SDL_strcmp(name, "list_directory")) {
        tool_list_directory(arguments, out, cap);
    } else if (!SDL_strcmp(name, "run_command")) {
        tool_run_command(runner, arguments, out, cap);
    } else {
        snprintf(out, cap, "error: unknown tool %s", name);
    }
    if (!out[0]) snprintf(out, cap, SDL_GetAtomicInt(&runner->cancel)
        ? "cancelled" : "error: empty tool result");
}
