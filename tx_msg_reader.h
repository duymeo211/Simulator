#ifndef TX_MSG_READER_H
#define TX_MSG_READER_H

#include <stdint.h>

#define TX_MSG_MAX_MESSAGES     300

/* Identifier-like label: keep compatibility with legacy tools (commonly 32 chars). */
#define TX_MSG_MAX_LABEL_LEN    33   /* 32 + '\0' */

/* Human-readable name/description: allow longer text safely. */
#define TX_MSG_MAX_NAME_LEN     81

/* CAN FD supports up to 64 payload bytes. */
#define TX_MSG_MAX_DATA_BYTES   64

#define TX_MSG_MAX_BUS_LEN      16
#define TX_MSG_MAX_VERSION_LEN  8
#define TX_MSG_MAX_CYCLE_UNIT   8

typedef struct
{
    char     label[TX_MSG_MAX_LABEL_LEN];
    char     name[TX_MSG_MAX_NAME_LEN];

    uint32_t id;
    uint32_t cycle_ms;

    uint8_t  length;        /* expected payload length */
    uint8_t  data_count;    /* bytes actually parsed from data[] */

    uint8_t  data[TX_MSG_MAX_DATA_BYTES];
} TxMsg;

typedef struct
{
    char  version[TX_MSG_MAX_VERSION_LEN];  /* optional */
    char  bus[TX_MSG_MAX_BUS_LEN];
    char  cycle_unit[TX_MSG_MAX_CYCLE_UNIT];

    TxMsg messages[TX_MSG_MAX_MESSAGES];
    int   message_count;
} TxMsgList;

/**
 * @brief Load and parse TX message file (YAML-like).
 * @param filepath Path to tx.can file.
 * @param out_list Output message list.
 * @return 0 on success, -1 on failure.
 */
int tx_msg_reader_load(const char* filepath, TxMsgList* out_list);

#endif /* TX_MSG_READER_H */