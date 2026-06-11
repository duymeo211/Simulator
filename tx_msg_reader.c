#include "tx_msg_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------ */
/* Small safe helpers */
/* ------------------------------ */

static char* trim(char* s)
{
    char* end;

    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;

    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return s;
}

static void copy_str(char* dst, size_t cap, const char* src)
{
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }

    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

/* Returns pointer to value after "key:" or NULL if not matched. */
static char* match_key(char* text, const char* key_with_colon)
{
    size_t klen = strlen(key_with_colon);
    char* v;

    if (strncmp(text, key_with_colon, klen) != 0) return NULL;

    v = text + klen;
    while (*v && isspace((unsigned char)*v)) v++;
    return v;
}

static void strip_optional_quotes(char* s)
{
    size_t n;
    if (!s) return;

    n = strlen(s);
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\''))) {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

/* Parse comma-separated bytes (hex or dec). Returns parsed count (may exceed out_cap). */
static int parse_byte_list(const char* list, uint8_t* out, int out_cap)
{
    const char* p = list;
    int count = 0;

    while (p && *p) {
        char* endp;
        unsigned long v;

        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (*p == '\0') break;

        v = strtoul(p, &endp, 0);
        if (endp == p) break; /* invalid token -> stop */

        if (count < out_cap) {
            out[count] = (uint8_t)(v & 0xFFu);
        }
        count++;
        p = endp;
    }
    return count;
}

static int msg_has_identity(const TxMsg* m)
{
    return (m && (m->label[0] != '\0' || m->name[0] != '\0'));
}

/* Finalize message: enforce identity rule, clamp lengths, compute DLC. */
static int finalize_message(TxMsg* m)
{
    if (!m) return 0;

    /* Must have at least label or name */
    if (!msg_has_identity(m)) return -1;

    /* Do NOT auto-fill missing identity.
     * Missing label/name will be filled with "N/A" after parsing. */

    /* Clamp length */
    if (m->length > TX_MSG_MAX_DATA_BYTES) {
        fprintf(stderr,
            "[tx_msg_reader] Warning: length=%u exceeds max %u, clamped.\n",
            (unsigned)m->length, (unsigned)TX_MSG_MAX_DATA_BYTES);
        m->length = TX_MSG_MAX_DATA_BYTES;
    }

    /* If length is 0 but data exists, infer length from data_count */
    if (m->length == 0 && m->data_count > 0) {
        m->length = m->data_count;
    }

    /* If data_count > length, truncate */
    if (m->length > 0 && m->data_count > m->length) {
        fprintf(stderr,
            "[tx_msg_reader] Warning: data_count=%u > length=%u for '%s' (label=%s). Truncating.\n",
            (unsigned)m->data_count, (unsigned)m->length, m->name, m->label);
        m->data_count = m->length;
    }

    return 0;
}

/* Read "data: [ ... ]" possibly spanning multiple lines until ']' */
static void read_data_block(FILE* fp, char* first_value, TxMsg* msg)
{
    char acc[1024];
    int acc_len = 0;

    char* open_br = strchr(first_value, '[');
    char* close_br;

    if (!msg) return;
    msg->data_count = 0;

    if (!open_br) return;

    open_br++; /* after '[' */
    close_br = strchr(open_br, ']');

    acc[0] = '\0';

    if (close_br) {
        *close_br = '\0';
        copy_str(acc, sizeof(acc), open_br);
    }
    else {
        /* Accumulate current line remainder */
        acc_len += snprintf(acc + acc_len, sizeof(acc) - (size_t)acc_len, "%s", open_br);

        /* Read until closing bracket */
        while (fgets(acc + acc_len, (int)(sizeof(acc) - (size_t)acc_len), fp) != NULL) {
            char* t = acc + acc_len;
            char* cb;

            t[strcspn(t, "\r\n")] = '\0';
            t = trim(t);

            cb = strchr(t, ']');
            if (cb) {
                *cb = '\0';
                acc_len += snprintf(acc + acc_len, sizeof(acc) - (size_t)acc_len, ",%s", t);
                break;
            }

            acc_len += snprintf(acc + acc_len, sizeof(acc) - (size_t)acc_len, ",%s", t);

            if ((size_t)acc_len >= sizeof(acc) - 4) {
                fprintf(stderr, "[tx_msg_reader] Warning: data block too long, truncated buffer.\n");
                break;
            }
        }
    }

    /* Parse bytes */
    {
        int parsed = parse_byte_list(acc, msg->data, TX_MSG_MAX_DATA_BYTES);
        if (parsed > TX_MSG_MAX_DATA_BYTES) {
            fprintf(stderr,
                "[tx_msg_reader] Warning: data has %d bytes, truncated to %d.\n",
                parsed, TX_MSG_MAX_DATA_BYTES);
            msg->data_count = (uint8_t)TX_MSG_MAX_DATA_BYTES;
        }
        else {
            msg->data_count = (uint8_t)parsed;
        }
    }
}

/* ------------------------------ */
/* Public API */
/* ------------------------------ */

int tx_msg_reader_load(const char* filepath, TxMsgList* out_list)
{
    FILE* fp;
    char line[512];

    int in_messages = 0;
    TxMsg* current_msg = NULL;

    if (!filepath || !out_list) return -1;

    fp = fopen(filepath, "r");
    if (!fp) {
        fprintf(stderr, "[tx_msg_reader] Cannot open: %s\n", filepath);
        return -1;
    }

    memset(out_list, 0, sizeof(*out_list));
    copy_str(out_list->cycle_unit, sizeof(out_list->cycle_unit), "ms");

    while (fgets(line, sizeof(line), fp) != NULL) {
        char* text;
        char* value;

        line[strcspn(line, "\r\n")] = '\0';
        text = trim(line);

        if (text[0] == '\0' || text[0] == '#') continue;

        /* Header keys */
        if ((value = match_key(text, "version:")) != NULL) {
            strip_optional_quotes(value);
            copy_str(out_list->version, sizeof(out_list->version), value);
            continue;
        }
        if ((value = match_key(text, "bus:")) != NULL) {
            strip_optional_quotes(value);
            copy_str(out_list->bus, sizeof(out_list->bus), value);
            continue;
        }
        if ((value = match_key(text, "cycle_unit:")) != NULL) {
            strip_optional_quotes(value);
            copy_str(out_list->cycle_unit, sizeof(out_list->cycle_unit), value);
            continue;
        }

        if (strcmp(text, "messages:") == 0) {
            in_messages = 1;
            current_msg = NULL;
            continue;
        }

        if (!in_messages) continue;

        /* New message item begins with '-' */
        if (text[0] == '-') {
            char* after_dash = trim(text + 1);

            /* finalize previous message */
            if (current_msg != NULL) {
                if (finalize_message(current_msg) != 0) {
                    int removed_index = out_list->message_count; /* before decrement */
                    out_list->message_count--;
                    fprintf(stderr,
                        "[tx_msg_reader] Warning: message #%d has no label/name. Removed.\n",
                        removed_index);
                }
            }

            /* allocate new */
            if (out_list->message_count >= TX_MSG_MAX_MESSAGES) {
                fprintf(stderr,
                    "[tx_msg_reader] Warning: message limit reached (%d). Remaining messages ignored.\n",
                    TX_MSG_MAX_MESSAGES);
                break;
            }

            current_msg = &out_list->messages[out_list->message_count++];
            memset(current_msg, 0, sizeof(*current_msg));

            /* Inline "- label: xxx" or "- name: yyy" */
            if ((value = match_key(after_dash, "label:")) != NULL) {
                strip_optional_quotes(value);
                copy_str(current_msg->label, sizeof(current_msg->label), value);
            }
            else if ((value = match_key(after_dash, "name:")) != NULL) {
                strip_optional_quotes(value);
                copy_str(current_msg->name, sizeof(current_msg->name), value);
            }

            continue;
        }

        if (current_msg == NULL) continue;

        /* Message keys */
        if ((value = match_key(text, "label:")) != NULL) {
            strip_optional_quotes(value);
            copy_str(current_msg->label, sizeof(current_msg->label), value);
            continue;
        }
        if ((value = match_key(text, "name:")) != NULL) {
            strip_optional_quotes(value);
            copy_str(current_msg->name, sizeof(current_msg->name), value);
            continue;
        }
        if ((value = match_key(text, "id:")) != NULL) {
            current_msg->id = (uint32_t)strtoul(value, NULL, 0);
            continue;
        }
        if ((value = match_key(text, "cycle:")) != NULL) {
            current_msg->cycle_ms = (uint32_t)strtoul(value, NULL, 10);
            continue;
        }
        if ((value = match_key(text, "length:")) != NULL) {
            unsigned long len = strtoul(value, NULL, 10);
            if (len > TX_MSG_MAX_DATA_BYTES) len = TX_MSG_MAX_DATA_BYTES;
            current_msg->length = (uint8_t)len;
            continue;
        }
        if ((value = match_key(text, "data:")) != NULL) {
            read_data_block(fp, value, current_msg);
            continue;
        }

        /* Unknown keys ignored (forward compatible) */
    }

    /* finalize last message */
    if (current_msg != NULL && out_list->message_count > 0) {
        if (finalize_message(current_msg) != 0) {
            int removed_index = out_list->message_count;
            out_list->message_count--;
            fprintf(stderr,
                "[tx_msg_reader] Warning: message #%d has no label/name. Removed.\n",
                removed_index);
        }
    }

    fclose(fp);


    /* Fill missing label/name with "N/A" */
    for (int i = 0; i < out_list->message_count; i++)
    {
        TxMsg* m = &out_list->messages[i];

        if (m->label[0] == '\0')
            strncpy(m->label, "N/A", 4);

        if (m->name[0] == '\0')
            strncpy(m->name, "N/A", 4);
    }

    if (out_list->message_count == 0) {
        fprintf(stderr, "[tx_msg_reader] No valid messages found in: %s\n", filepath);
        return -1;
    }

    return 0;
}