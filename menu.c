
#include "menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <conio.h>
#include <io.h>
#else
#include <sys/select.h>
#include <unistd.h>
#endif

/* Forward declarations for internal functions */
static void menu_edit_tx_message(SimulatorCtrl* ctrl, int ch_index);
static int  menu_ask_channel(int channel_count);
static int menu_ask_channel_or_all(int channel_count);
static void menu_print_tx_table(const TxMsgList* list, int ch_num);
static int  parse_hex_string(const char* input, uint8_t* out_data,
    int expected_len, int* out_count);
static void print_single_msg(int index, const TxMsg* msg);

static int menu_ask_channel(int channel_count)
{
    char input[16];
    int  ch;

    printf("Select channel (1-%d): ", channel_count);
    fflush(stdout);

    if (fgets(input, sizeof(input), stdin) == NULL)
        return -1;

    input[strcspn(input, "\r\n")] = '\0';
    ch = atoi(input);

    if (ch < 1 || ch > channel_count)
    {
        printf("[Menu] Invalid channel: %d\n", ch);
        return -1;
    }

    return ch - 1;  /* 0-based index */
}

/* Returns: 0..N-1 = channel index, -2 = all, -1 = invalid */
static int menu_ask_channel_or_all(int channel_count)
{
    char input[16];
    int  ch;

    printf("Select channel (1-%d, 0=All): ", channel_count);
    fflush(stdout);

    if (fgets(input, sizeof(input), stdin) == NULL)
        return -1;

    input[strcspn(input, "\r\n")] = '\0';
    ch = atoi(input);

    if (ch == 0)
        return -2;  /* all */

    if (ch < 1 || ch > channel_count)
    {
        printf("[Menu] Invalid channel: %d\n", ch);
        return -1;
    }

    return ch - 1;
}


#define COL_NO           3
#define COL_LABEL        8
#define COL_NAME         20
#define COL_ID           6
#define COL_CYCLE        5
#define COL_LEN          3

#define BYTES_PER_ROW    16
#define COL_DATA         48
#define DATA_STR_SIZE    (BYTES_PER_ROW * 3 + 1)

/* segment width = COL + 2 (for spaces around) */
#define TABLE_SEPARATOR \
    "+-----+----------+----------------------+--------+-------+-----+--------------------------------------------------+"


static int wrap_lines_count(const char* s, int col_width)
{
    int len;

    if (col_width <= 0) return 1;
    if (s == NULL || s[0] == '\0') return 1;

    len = (int)strlen(s);
    return (len + col_width - 1) / col_width;
}

static void wrap_get_chunk(const char* s, int col_width, int line_index, char* out, size_t out_size)
{
    int start;
    int remain;
    int copy_len;

    if (out == NULL || out_size == 0) return;
    out[0] = '\0';

    if (col_width <= 0 || s == NULL || s[0] == '\0') return;

    start = line_index * col_width;
    if (start >= (int)strlen(s)) return;

    remain = (int)strlen(s) - start;
    copy_len = (remain > col_width) ? col_width : remain;

    if ((size_t)copy_len >= out_size) copy_len = (int)out_size - 1;
    memcpy(out, s + start, (size_t)copy_len);
    out[copy_len] = '\0';
}

static void build_data_row_str(const TxMsg* msg, int row_index, char* out, size_t out_size)
{
    int byte_start;
    int byte_end;
    int pos = 0;
    int j;

    if (!out || out_size == 0) return;
    out[0] = '\0';

    if (!msg) return;

    byte_start = row_index * BYTES_PER_ROW;
    byte_end = byte_start + BYTES_PER_ROW;
    if (byte_start >= (int)msg->length) return;
    if (byte_end > (int)msg->length) byte_end = (int)msg->length;

    for (j = byte_start; j < byte_end; j++)
    {
        pos += snprintf(out + pos, out_size - (size_t)pos, "%02X ", msg->data[j]);
        if (pos >= (int)out_size) break;
    }

    if (pos > 0 && out[pos - 1] == ' ')
        out[pos - 1] = '\0';
}

static void print_tx_table_header(void)
{
    printf(TABLE_SEPARATOR "\n");
    printf("| %-*s | %-*s | %-*s | %-*s | %-*s | %-*s | %-*s |\n",
        COL_NO, "No.",
        COL_LABEL, "Label",
        COL_NAME, "Name",
        COL_ID, "ID",
        COL_CYCLE, "Cycle",
        COL_LEN, "Len",
        COL_DATA, "Data");
    printf(TABLE_SEPARATOR "\n");
}


static void menu_print_tx_table(const TxMsgList* list, int ch_num)
{
    int i;

    if (list == NULL || list->message_count == 0)
    {
        printf("  (no messages loaded)\n");
        return;
    }

    printf("\n");
    printf(" Ch%d TX Message List | Bus: %-8s | Cycle unit: %s\n",
        ch_num, list->bus, list->cycle_unit);

    print_tx_table_header();

    for (i = 0; i < list->message_count; i++)
    {
        const TxMsg* msg = &list->messages[i];
        char id_str[16];

        int data_rows;
        int label_lines;
        int name_lines;
        int total_lines;
        int line_idx;

        snprintf(id_str, sizeof(id_str), "0x%04X", msg->id);

        data_rows = (msg->length + BYTES_PER_ROW - 1) / BYTES_PER_ROW;
        if (data_rows <= 0) data_rows = 1;

        label_lines = wrap_lines_count(msg->label, COL_LABEL);
        name_lines = wrap_lines_count(msg->name, COL_NAME);

        total_lines = data_rows;
        if (label_lines > total_lines) total_lines = label_lines;
        if (name_lines > total_lines) total_lines = name_lines;

        for (line_idx = 0; line_idx < total_lines; line_idx++)
        {
            char label_chunk[COL_LABEL + 1];
            char name_chunk[COL_NAME + 1];
            char data_str[DATA_STR_SIZE];

            wrap_get_chunk(msg->label, COL_LABEL, line_idx, label_chunk, sizeof(label_chunk));
            wrap_get_chunk(msg->name, COL_NAME, line_idx, name_chunk, sizeof(name_chunk));

            if (line_idx < data_rows)
                build_data_row_str(msg, line_idx, data_str, sizeof(data_str));
            else
                data_str[0] = '\0';

            if (line_idx == 0)
            {
                printf("| %-*d | %-*s | %-*s | %-*s | %-*u | %-*u | %-*.*s |\n",
                    COL_NO, i + 1,
                    COL_LABEL, label_chunk,
                    COL_NAME, name_chunk,
                    COL_ID, id_str,
                    COL_CYCLE, msg->cycle_ms,
                    COL_LEN, msg->length,
                    COL_DATA, COL_DATA, data_str);
            }
            else
            {
                printf("| %-*s | %-*s | %-*s | %-*s | %-*s | %-*s | %-*.*s |\n",
                    COL_NO, "",
                    COL_LABEL, label_chunk,
                    COL_NAME, name_chunk,
                    COL_ID, "",
                    COL_CYCLE, "",
                    COL_LEN, "",
                    COL_DATA, COL_DATA, data_str);
            }
        }
    }

    printf(TABLE_SEPARATOR "\n");
    printf(" Total: %d message(s)\n\n", list->message_count);
}


void menu_print(const SimulatorCtrl* ctrl)
{
    printf("\n");
    printf("=================================\n");

    for (int i = 0; i < ctrl->sim_cfg.channel_count; i++)
    {
        printf("Ch%d: %-12s | TX:%-3s | %3d msg | port %-5u\n",
            i + 1,
            ctrl->ch_connected[i] ? "connected" : "disconnected",
            ctrl->tx_enabled[i] ? "ON" : "OFF",
            ctrl->tx_lists[i].message_count,
            ctrl->sim_cfg.channels[i].port);
    }

    printf("---------------------------------\n");
    printf("RX: %s\n", ctrl->rx_enabled ? "ON" : "OFF");
    printf("---------------------------------\n");
    printf("1. View TX message list\n");
    printf("2. Edit TX message\n");
    printf("3. Start TX\n");
    printf("4. Stop TX\n");
    printf("5. Start RX\n");
    printf("6. Stop RX\n");
    printf("0. Shutdown Simulator\n");
    printf("Select: ");
    fflush(stdout);
}


void menu_handle_choice(int choice, SimulatorCtrl *ctrl)
{
    switch (choice)
    {
        case 1:
        {
            int ch = menu_ask_channel(ctrl->sim_cfg.channel_count);
            if (ch >= 0)
            {
                menu_print_tx_table(&ctrl->tx_lists[ch], ch + 1);
            }
            break;
        }

        case 2:
        {
            int ch = menu_ask_channel(ctrl->sim_cfg.channel_count);
            if (ch >= 0)
            {
                menu_edit_tx_message(ctrl, ch);
            }
            break;
        }

        case 3:
        {
            int ch = menu_ask_channel_or_all(ctrl->sim_cfg.channel_count);
            if (ch >= 0)
            {
                ctrl->tx_enabled[ch] = true;
                printf("[Menu] TX Ch%d enabled\n", ch + 1);
            }
            else if (ch == -2)  /* all */
            {
                for (int k = 0; k < ctrl->sim_cfg.channel_count; k++)
                    ctrl->tx_enabled[k] = true;
                printf("[Menu] TX ALL enabled\n");
            }
            break;
        }

        case 4:
        {
            int ch = menu_ask_channel_or_all(ctrl->sim_cfg.channel_count);
            if (ch >= 0)
            {
                ctrl->tx_enabled[ch] = false;
                printf("[Menu] TX Ch%d disabled\n", ch + 1);
            }
            else if (ch == -2)  /* all */
            {
                for (int k = 0; k < ctrl->sim_cfg.channel_count; k++)
                    ctrl->tx_enabled[k] = false;
                printf("[Menu] TX ALL disabled\n");
            }
            break;
        }

        case 5:
            ctrl->rx_enabled = true;
            printf("[Menu] RX enabled (decode/log to rx_debug.log)\n");
            break;

        case 6:
            ctrl->rx_enabled = false;
            printf("[Menu] RX disabled\n");
            break;

        case 0:
            ctrl->running = false;
            printf("[Menu] Shutdown requested...\n");
            break;

        default:
            printf("[Menu] Invalid option: %d\n", choice);
            break;
    }
}

bool menu_try_read_choice(int *out_choice)
{
#ifdef _WIN32
    static char buf[16];
    static int len = 0;

    /* Support for automated testing via redirected stdin (e.g. from file or pipe) */
    if (!_isatty(_fileno(stdin)))
    {
        char line[32];
        if (fgets(line, sizeof(line), stdin) != NULL)
        {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0] == '\0') return false;
            *out_choice = atoi(line);
            return true;
        }
        return false;
    }

    while (_kbhit())
    {
        int ch = _getch();

        if (ch == '\r' || ch == '\n')
        {
            printf("\n");
            fflush(stdout);

            buf[len] = '\0';
            len = 0;

            if (buf[0] == '\0')
            {
                return false;
            }

            *out_choice = atoi(buf);
            return true;
        }
        else if (ch == '\b')
        {
            if (len > 0)
            {
                len--;
                printf("\b \b");
                fflush(stdout);
            }
        }
        else if (ch >= '0' && ch <= '9')
        {
            if (len < (int)sizeof(buf) - 1)
            {
                buf[len++] = (char)ch;
                printf("%c", ch);
                fflush(stdout);
            }
        }
    }

    return false;

#else
    fd_set rfds;
    struct timeval tv = {0, 0};

    FD_ZERO(&rfds);
    FD_SET(0, &rfds);

    if (select(1, &rfds, NULL, NULL, &tv) > 0)
    {
        char line[32];

        if (fgets(line, sizeof(line), stdin) != NULL)
        {
            line[strcspn(line, "\r\n")] = '\0';

            if (line[0] == '\0')
            {
                return false;
            }

            *out_choice = atoi(line);
            return true;
        }
    }

    return false;
#endif
}


/**
 * @brief Validate and parse a hex string into byte array.
 *        Expected format: "01 02 AB ..." (tokens separated by single space).
 *        Each token must be exactly 2 hex characters.
 *
 * @param input     Input string from user.
 * @param out_data  Output byte array.
 * @param expected_len  Expected number of bytes (must match message length).
 * @param out_count Actual number of bytes parsed.
 * @return 0 if valid, -1 if invalid.
 */
static int parse_hex_string(const char* input,
    uint8_t* out_data,
    int         expected_len,
    int* out_count)
{
    char buf[512];
    char* token;
    int   count = 0;

    if (input == NULL || out_data == NULL || out_count == NULL)
    {
        return -1;
    }

    strncpy(buf, input, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    token = strtok(buf, " ");

    while (token != NULL)
    {
        size_t token_len = strlen(token);
        size_t i;

        /* Each token must be exactly 2 hex characters */
        if (token_len != 2)
        {
            fprintf(stderr,
                "[Menu] Invalid token \"%s\": must be exactly 2 hex digits.\n",
                token);
            return -1;
        }

        /* Each character must be a valid hex digit */
        for (i = 0; i < token_len; i++)
        {
            char c = (char)toupper((unsigned char)token[i]);
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')))
            {
                fprintf(stderr,
                    "[Menu] Invalid character '%c' in token \"%s\".\n",
                    token[i], token);
                return -1;
            }
        }

        if (count >= TX_MSG_MAX_DATA_BYTES)
        {
            fprintf(stderr, "[Menu] Too many bytes (max %d).\n",
                TX_MSG_MAX_DATA_BYTES);
            return -1;
        }

        out_data[count++] = (uint8_t)strtoul(token, NULL, 16);
        token = strtok(NULL, " ");
    }

    *out_count = count;

    /* Byte count must match message length */
    if (count != expected_len)
    {
        fprintf(stderr,
            "[Menu] Data length mismatch: expected %d bytes, got %d bytes.\n",
            expected_len, count);
        return -1;
    }

    return 0;
}

/**
 * @brief Print a single TX message as a table row (with multi-line data support).
 */
static void print_single_msg(int index, const TxMsg* msg)
{
    char id_str[16];

    int data_rows;
    int label_lines;
    int name_lines;
    int total_lines;
    int line_idx;

    if (!msg) return;

    snprintf(id_str, sizeof(id_str), "0x%04X", msg->id);

    data_rows = (msg->length + BYTES_PER_ROW - 1) / BYTES_PER_ROW;
    if (data_rows <= 0) data_rows = 1;

    label_lines = wrap_lines_count(msg->label, COL_LABEL);
    name_lines = wrap_lines_count(msg->name, COL_NAME);

    total_lines = data_rows;
    if (label_lines > total_lines) total_lines = label_lines;
    if (name_lines > total_lines) total_lines = name_lines;

    print_tx_table_header();

    for (line_idx = 0; line_idx < total_lines; line_idx++)
    {
        char label_chunk[COL_LABEL + 1];
        char name_chunk[COL_NAME + 1];
        char data_str[DATA_STR_SIZE];

        wrap_get_chunk(msg->label, COL_LABEL, line_idx, label_chunk, sizeof(label_chunk));
        wrap_get_chunk(msg->name, COL_NAME, line_idx, name_chunk, sizeof(name_chunk));

        if (line_idx < data_rows)
            build_data_row_str(msg, line_idx, data_str, sizeof(data_str));
        else
            data_str[0] = '\0';

        if (line_idx == 0)
        {
            printf("| %-*d | %-*s | %-*s | %-*s | %-*u | %-*u | %-*.*s |\n",
                COL_NO, index,
                COL_LABEL, label_chunk,
                COL_NAME, name_chunk,
                COL_ID, id_str,
                COL_CYCLE, msg->cycle_ms,
                COL_LEN, msg->length,
                COL_DATA, COL_DATA, data_str);
        }
        else
        {
            printf("| %-*s | %-*s | %-*s | %-*s | %-*s | %-*s | %-*.*s |\n",
                COL_NO, "",
                COL_LABEL, label_chunk,
                COL_NAME, name_chunk,
                COL_ID, "",
                COL_CYCLE, "",
                COL_LEN, "",
                COL_DATA, COL_DATA, data_str);
        }
    }

    printf(TABLE_SEPARATOR "\n");
}


/**
 * @brief Handle menu option 2: Edit TX message data interactively.
 * @param ctrl Simulator runtime control object.
 */
static void menu_edit_tx_message(SimulatorCtrl* ctrl, int ch_index)
{
    char input[512];
    int  msg_index;
    int  count;
    int  message_count;
    uint8_t new_data[TX_MSG_MAX_DATA_BYTES];

    TxMsg current_msg;
    TxMsg updated_msg;

    if (ctrl == NULL)
    {
        return;
    }

    /* Read message count safely */
    sim_mutex_lock(&ctrl->tx_list_mutex[ch_index]);
    message_count = ctrl->tx_lists[ch_index].message_count;
    sim_mutex_unlock(&ctrl->tx_list_mutex[ch_index]);

    if (message_count == 0)
    {
        printf("[Menu] Ch%d: No messages loaded.\n", ch_index + 1);
        return;
    }

    printf("[Edit] Ch%d: Enter message number (1..%d): ",
        ch_index + 1, message_count);
    fflush(stdout);

    if (fgets(input, sizeof(input), stdin) == NULL)
    {
        printf("[Menu] Input error.\n");
        return;
    }

    input[strcspn(input, "\r\n")] = '\0';
    msg_index = atoi(input);

    if (msg_index < 1 || msg_index > message_count)
    {
        printf("[Menu] Invalid message number: %d. Valid range: 1..%d.\n",
            msg_index, message_count);
        return;
    }

    /* Copy current message to local snapshot */
    sim_mutex_lock(&ctrl->tx_list_mutex[ch_index]);
    current_msg = ctrl->tx_lists[ch_index].messages[msg_index - 1];
    sim_mutex_unlock(&ctrl->tx_list_mutex[ch_index]);

    printf("[Edit] Current message:\n");
    print_single_msg(msg_index, &current_msg);

    printf("[Edit] Enter new data (%d bytes, format: \"01 02 AB ...\"): ",
        current_msg.length);
    fflush(stdout);

    if (fgets(input, sizeof(input), stdin) == NULL)
    {
        printf("[Menu] Input error.\n");
        return;
    }

    input[strcspn(input, "\r\n")] = '\0';

    memset(new_data, 0, sizeof(new_data));
    count = 0;

    if (parse_hex_string(input, new_data, (int)current_msg.length, &count) != 0)
    {
        printf("[Menu] Data not updated.\n");
        return;
    }

    /* Critical section: apply new payload only */
    sim_mutex_lock(&ctrl->tx_list_mutex[ch_index]);
    memcpy(ctrl->tx_lists[ch_index].messages[msg_index - 1].data,
        new_data,
        (size_t)current_msg.length);
    updated_msg = ctrl->tx_lists[ch_index].messages[msg_index - 1];
    sim_mutex_unlock(&ctrl->tx_list_mutex[ch_index]);

    printf("[Edit] Message updated:\n");
    print_single_msg(msg_index, &updated_msg);
}