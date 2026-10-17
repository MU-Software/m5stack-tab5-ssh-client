// SPDX-License-Identifier: Apache-2.0
#include "c6_nvs_server.h"

#include <string.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <nvs.h>

#include "c6_protocol.h"

_Static_assert(C6_NVS_U8 == NVS_TYPE_U8 && C6_NVS_I8 == NVS_TYPE_I8 && C6_NVS_U16 == NVS_TYPE_U16 && C6_NVS_I16 == NVS_TYPE_I16 &&
                   C6_NVS_U32 == NVS_TYPE_U32 && C6_NVS_I32 == NVS_TYPE_I32 && C6_NVS_U64 == NVS_TYPE_U64 && C6_NVS_I64 == NVS_TYPE_I64 &&
                   C6_NVS_STR == NVS_TYPE_STR && C6_NVS_BLOB == NVS_TYPE_BLOB,
               "nvs_type_t");

static const char* TAG = "nvs";
// C6_NVS_WRITE's value so far, and the [namespace][key][type] it is for
static uint8_t* pending;
static size_t pending_size, pending_got, pending_name_length;
static uint8_t pending_name[2 * NVS_KEY_NAME_MAX_SIZE + 1];

// [length][name] at *at; an empty one only when empty is allowed
static bool take_name(const uint8_t* body, size_t size, size_t* at, char name[NVS_KEY_NAME_MAX_SIZE], bool empty) {
    if (*at >= size || body[*at] >= NVS_KEY_NAME_MAX_SIZE || (body[*at] == 0 && !empty) || *at + 1 + body[*at] > size) return false;
    memcpy(name, body + *at + 1, body[*at]);
    name[body[*at]] = 0;
    *at += 1 + body[*at];
    return true;
}

static void put_number(uint8_t* out, uint32_t n) {
    for (int i = 0; i < 4; i++)
        out[i] = (uint8_t)(n >> 8 * i);
}

static uint32_t number_at(const uint8_t* bytes) {
    return bytes[0] | bytes[1] << 8 | bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

// Before a free, which a memset would not outlive
static void wipe(void* memory, size_t size) {
    for (volatile uint8_t* p = memory; size-- > 0; p++)
        *p = 0;
}

static void drop_pending(void) {
    if (pending != NULL) wipe(pending, pending_size + 1);
    free(pending);
    pending = NULL;
}

static esp_err_t get_integer(nvs_handle_t nvs, const char* key, nvs_type_t type, void* out) {
    switch (type) {
    case NVS_TYPE_U8:
        return nvs_get_u8(nvs, key, out);
    case NVS_TYPE_I8:
        return nvs_get_i8(nvs, key, out);
    case NVS_TYPE_U16:
        return nvs_get_u16(nvs, key, out);
    case NVS_TYPE_I16:
        return nvs_get_i16(nvs, key, out);
    case NVS_TYPE_U32:
        return nvs_get_u32(nvs, key, out);
    case NVS_TYPE_I32:
        return nvs_get_i32(nvs, key, out);
    case NVS_TYPE_U64:
        return nvs_get_u64(nvs, key, out);
    case NVS_TYPE_I64:
        return nvs_get_i64(nvs, key, out);
    default:
        return ESP_ERR_NVS_TYPE_MISMATCH;
    }
}

static esp_err_t set_integer(nvs_handle_t nvs, const char* key, nvs_type_t type, uint64_t n) {
    switch (type) {
    case NVS_TYPE_U8:
        return nvs_set_u8(nvs, key, (uint8_t)n);
    case NVS_TYPE_I8:
        return nvs_set_i8(nvs, key, (int8_t)n);
    case NVS_TYPE_U16:
        return nvs_set_u16(nvs, key, (uint16_t)n);
    case NVS_TYPE_I16:
        return nvs_set_i16(nvs, key, (int16_t)n);
    case NVS_TYPE_U32:
        return nvs_set_u32(nvs, key, (uint32_t)n);
    case NVS_TYPE_I32:
        return nvs_set_i32(nvs, key, (int32_t)n);
    case NVS_TYPE_U64:
        return nvs_set_u64(nvs, key, n);
    case NVS_TYPE_I64:
        return nvs_set_i64(nvs, key, (int64_t)n);
    default:
        return ESP_ERR_NVS_TYPE_MISMATCH;
    }
}

// value has room for a string's terminator after length
static esp_err_t store(nvs_handle_t nvs, const char* key, nvs_type_t type, uint8_t* value, size_t length) {
    uint64_t number = 0;
    if (type == NVS_TYPE_BLOB) return nvs_set_blob(nvs, key, value, length);
    if (type == NVS_TYPE_STR) {
        if (memchr(value, 0, length) != NULL) return ESP_ERR_INVALID_ARG;
        value[length] = 0;
        return nvs_set_str(nvs, key, (const char*)value);
    }
    if (length != (type & 0x0fu)) return ESP_ERR_INVALID_SIZE;
    memcpy(&number, value, length);
    return set_integer(nvs, key, type, number);
}

// A string's with its terminator
static size_t value_size(nvs_handle_t nvs, const char* key, nvs_type_t type) {
    size_t size = type & 0x0f;
    if (type == NVS_TYPE_STR) nvs_get_str(nvs, key, NULL, &size);
    if (type == NVS_TYPE_BLOB) nvs_get_blob(nvs, key, NULL, &size);
    return size;
}

static esp_err_t list_entries(const uint8_t* body, size_t size, uint8_t* out, size_t* length) {
    nvs_iterator_t it = NULL;
    nvs_entry_info_t info;
    nvs_handle_t nvs;
    unsigned skip = size >= 2 ? body[0] | body[1] << 8 : 0;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, NULL, NVS_TYPE_ANY, &it);
    for (; err == ESP_OK; err = nvs_entry_next(&it)) {
        nvs_entry_info(it, &info);
        size_t space = strlen(info.namespace_name), key = strlen(info.key), value = info.type & 0x0f;
        if (skip > 0) {
            skip--;
            continue;
        }
        if (*length + 7 + space + key > C6_NVS_MESSAGE) break;
        if ((info.type == NVS_TYPE_STR || info.type == NVS_TYPE_BLOB) && nvs_open(info.namespace_name, NVS_READONLY, &nvs) == ESP_OK) {
            value = value_size(nvs, info.key, info.type);
            nvs_close(nvs);
        }
        out[(*length)++] = (uint8_t)info.type;
        out[(*length)++] = (uint8_t)space;
        memcpy(out + *length, info.namespace_name, space);
        out[*length += space] = (uint8_t)key;
        memcpy(out + ++*length, info.key, key);
        put_number(out + (*length += key), value);
        *length += 4;
    }
    nvs_release_iterator(it);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

static esp_err_t get_value(nvs_handle_t nvs, const char* key, uint32_t offset, uint8_t* out, size_t* length) {
    nvs_type_t type;
    uint64_t number = 0;
    esp_err_t err = nvs_find_key(nvs, key, &type);
    if (err != ESP_OK) return err;
    size_t size = value_size(nvs, key, type);
    bool held = type == NVS_TYPE_STR || type == NVS_TYPE_BLOB;
    uint8_t* value = held ? heap_caps_malloc(size + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) : (uint8_t*)&number;
    if (value == NULL) return ESP_ERR_NO_MEM;
    if (type == NVS_TYPE_STR)
        err = nvs_get_str(nvs, key, (char*)value, &size);
    else if (type == NVS_TYPE_BLOB)
        err = nvs_get_blob(nvs, key, value, &size);
    else
        err = get_integer(nvs, key, type, value);
    out[0] = (uint8_t)type;
    put_number(out + 1, size);
    *length = 5;
    if (err == ESP_OK && offset < size) {
        size_t n = size - offset < C6_NVS_MESSAGE - 5 ? size - offset : C6_NVS_MESSAGE - 5;
        memcpy(out + 5, value + offset, n);
        *length += n;
    }
    if (held) {
        wipe(value, size);
        free(value);
    }
    return err;
}

// Every namespace's entries; the namespaces themselves stay, unseen
static esp_err_t clear(void) {
    nvs_iterator_t it = NULL;
    nvs_entry_info_t info;
    nvs_handle_t nvs;
    esp_err_t err = ESP_OK;
    for (int i = 0; err == ESP_OK; i++) {
        if (i == 256 || (err = nvs_entry_find(NVS_DEFAULT_PART_NAME, NULL, NVS_TYPE_ANY, &it)) != ESP_OK) break;
        nvs_entry_info(it, &info);
        nvs_release_iterator(it);
        if ((err = nvs_open(info.namespace_name, NVS_READWRITE, &nvs)) != ESP_OK) break;
        err = nvs_erase_all(nvs);
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err == ESP_OK ? ESP_FAIL : err;
}

// [type][size, 4 bytes][offset, 4 bytes][piece] after the names at at; written once the last piece came
static esp_err_t write_piece(const uint8_t* body, size_t size, size_t at, bool* complete) {
    *complete = false;
    if (at + 9 > size) return ESP_ERR_INVALID_ARG;
    uint32_t total = number_at(body + at + 1), offset = number_at(body + at + 5);
    size_t n = size - at - 9;
    if (offset == 0) {
        drop_pending();
        if ((pending = heap_caps_malloc(total + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) == NULL) return ESP_ERR_NO_MEM;
        pending_size = total, pending_got = 0, pending_name_length = at + 1;
        memcpy(pending_name, body, at + 1);
    } else if (pending == NULL || offset != pending_got || total != pending_size || at + 1 != pending_name_length ||
               memcmp(pending_name, body, at + 1) != 0) {
        drop_pending();
        return ESP_ERR_INVALID_STATE;
    }
    if (n > pending_size - pending_got) {
        drop_pending();
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(pending + pending_got, body + at + 9, n);
    pending_got += n;
    *complete = pending_got == pending_size;
    return ESP_OK;
}

// Read-only first: a read-write open makes a namespace that is not there
static esp_err_t open_namespace(const char* space, uint8_t operation, nvs_handle_t* nvs) {
    esp_err_t err = nvs_open(space, NVS_READONLY, nvs);
    if (err == ESP_OK && operation != C6_NVS_GET) {
        nvs_close(*nvs);
        err = nvs_open(space, NVS_READWRITE, nvs);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND && (operation == C6_NVS_SET || operation == C6_NVS_WRITE)) err = nvs_open(space, NVS_READWRITE, nvs);
    return err;
}

size_t c6_nvs_serve(uint8_t operation, const uint8_t* body, size_t size, uint8_t* answer, esp_err_t* error) {
    char space[NVS_KEY_NAME_MAX_SIZE], key[NVS_KEY_NAME_MAX_SIZE];
    size_t at = 0, length = 0;
    nvs_handle_t nvs;
    bool complete = true;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (operation == C6_NVS_LIST) {
        err = list_entries(body, size, answer, &length);
    } else if (operation == C6_NVS_CLEAR) {
        drop_pending();
        err = clear();
        ESP_LOGI(TAG, "clear: %s", esp_err_to_name(err));
    } else if (operation <= C6_NVS_WRITE && take_name(body, size, &at, space, false) && take_name(body, size, &at, key, operation == C6_NVS_ERASE)) {
        uint8_t* value = NULL;
        nvs_type_t type = at < size ? body[at] : 0;
        if (operation == C6_NVS_WRITE) {
            err = write_piece(body, size, at, &complete);
        } else if (operation == C6_NVS_SET && at < size) {
            value = heap_caps_malloc(size - at, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            err = value != NULL ? ESP_OK : ESP_ERR_NO_MEM;
            if (value != NULL) memcpy(value, body + at + 1, size - at - 1);
        } else if (operation != C6_NVS_SET) {
            err = ESP_OK;
        }
        if (err == ESP_OK && complete && (err = open_namespace(space, operation, &nvs)) == ESP_OK) {
            if (operation == C6_NVS_GET)
                err = at + 4 <= size ? get_value(nvs, key, number_at(body + at), answer, &length) : ESP_ERR_INVALID_ARG;
            else if (operation == C6_NVS_SET)
                err = store(nvs, key, type, value, size - at - 1);
            else if (operation == C6_NVS_WRITE)
                err = store(nvs, key, type, pending, pending_size);
            else
                err = key[0] ? nvs_erase_key(nvs, key) : nvs_erase_all(nvs);
            if (err == ESP_OK && operation != C6_NVS_GET) err = nvs_commit(nvs);
            nvs_close(nvs);
        }
        if (value != NULL) {
            wipe(value, size - at);
            free(value);
        }
        if (operation == C6_NVS_WRITE && (complete || err != ESP_OK)) drop_pending();
        if (operation == C6_NVS_SET || (operation == C6_NVS_WRITE && err != ESP_OK))
            ESP_LOGI(TAG, "%s %s %s: %s", operation == C6_NVS_SET ? "set" : "write", space, key, esp_err_to_name(err));
        if (operation == C6_NVS_ERASE) ESP_LOGI(TAG, "erase %s %s: %s", space, key[0] ? key : "(all)", esp_err_to_name(err));
    }
    *error = err;
    return length;
}

esp_err_t c6_nvs_local(const uint8_t* request, size_t request_length, uint8_t* answer, size_t* answer_length) {
    static StaticSemaphore_t memory;
    static SemaphoreHandle_t lock;
    static portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;
    esp_err_t err;
    if (request_length < 1 || *answer_length < C6_NVS_MESSAGE) return ESP_ERR_INVALID_SIZE;
    taskENTER_CRITICAL(&guard);
    if (lock == NULL) lock = xSemaphoreCreateMutexStatic(&memory);
    taskEXIT_CRITICAL(&guard);
    xSemaphoreTake(lock, portMAX_DELAY);
    *answer_length = c6_nvs_serve(request[0], request + 1, request_length - 1, answer, &err);
    xSemaphoreGive(lock);
    return err;
}
