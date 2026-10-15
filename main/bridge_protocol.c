#include "bridge_protocol.h"
#include <string.h>
int bridge_parse(bridge_parser_t *p, unsigned char c, size_t limit)
{
  if (c=='\n') {
    int result=p->bad ? -1 : (p->length ? 1 : 0);
    p->data[p->length]=0; p->length=0; p->bad=p->cr=0;
    return result;
  }
  if (p->bad) return 0;
  if (p->cr || c<32 || c>126 || p->length>=limit || p->length>=BRIDGE_LINE-1U) {
    if (c=='\r' && !p->cr) p->cr=1U;
    else { p->bad=1U; return -1; }
  } else p->data[p->length++]=(char)c;
  return 0;
}
int bridge_push(bridge_queue_t *q,const char *line)
{
  size_t n=strlen(line);
  if (n+1U>=BRIDGE_LINE || q->count==BRIDGE_SLOTS) return 0;
  memcpy(q->data[q->head],line,n); q->data[q->head][n]='\n';
  q->length[q->head]=n+1U; q->head=(q->head+1U)%BRIDGE_SLOTS; ++q->count;
  return 1;
}
const char *bridge_front(bridge_queue_t *q,size_t *remaining)
{
  if (!q->count) { *remaining=0; return NULL; }
  *remaining=q->length[q->tail]-q->offset;
  return q->data[q->tail]+q->offset;
}
void bridge_advance(bridge_queue_t *q,size_t sent)
{
  if (!q->count || sent>q->length[q->tail]-q->offset) return;
  q->offset+=sent;
  if (q->offset==q->length[q->tail]) { q->offset=0; q->tail=(q->tail+1U)%BRIDGE_SLOTS; --q->count; }
}
int bridge_reply_matches(const char *record,const char *session)
{
  return strlen(record)>=21U && strlen(session)==16U &&
      strncmp(record,"RSP ",4U)==0 && strncmp(record+4,session,16U)==0 && record[20]==' ';
}
