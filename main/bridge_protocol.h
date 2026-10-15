#ifndef BRIDGE_PROTOCOL_H
#define BRIDGE_PROTOCOL_H
#include <stddef.h>
#define BRIDGE_LINE 320U
#define BRIDGE_SLOTS 8U
typedef struct { char data[BRIDGE_LINE]; size_t length; unsigned cr, bad; } bridge_parser_t;
typedef struct { char data[BRIDGE_SLOTS][BRIDGE_LINE]; size_t length[BRIDGE_SLOTS], offset; unsigned head, tail, count; } bridge_queue_t;
/* 1=complete record, -1=invalid/oversized input, 0=incomplete. */
int bridge_parse(bridge_parser_t *parser, unsigned char byte, size_t limit);
int bridge_push(bridge_queue_t *queue, const char *record);
const char *bridge_front(bridge_queue_t *queue, size_t *remaining);
void bridge_advance(bridge_queue_t *queue, size_t sent);
int bridge_reply_matches(const char *record, const char *session);
#endif
