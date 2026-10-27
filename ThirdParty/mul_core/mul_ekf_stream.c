/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "mul_ekf_stream.h"
#include <string.h>
bool mulEkfStreamInit(mulEkfStream_t *s,const mulEkfConfig_t *c,uint32_t clock,uint32_t epoch,uint32_t frame,
 uint64_t time,const double x[3],const double p[3][3]) {
 if(!s)return false;
 mulEkf_t initial;if(!mulEkfInit(&initial,c,clock,epoch,frame,time,x,p))return false;
 memset(s,0,sizeof(*s));s->base=s->current=initial;s->healthy=true;return true;
}
bool mulEkfStreamEnable(mulEkfStream_t *s,bool enabled) {
 if(!s||!s->healthy||!mulEkfEnable(&s->base,enabled))return false;
 s->current.enabled=enabled;return true;
}
static bool order(const mulEkfBatch_t *a,const mulEkfBatch_t *b) {
 return a->timeUs<b->timeUs||(a->timeUs==b->timeUs&&a->group<b->group);
}
static bool rebuild(mulEkfStream_t *s) {
 s->replay=s->base;unsigned at=0;
 for(unsigned i=0;i<s->batchCount;i++)s->batches[i].status=mulEkfQueued;
 for(unsigned interval=0;interval<=s->intervalCount;interval++) {
  uint64_t end=interval?s->intervals[interval-1].endUs:s->base.timeUs;
  while(at<s->batchCount&&s->batches[at].batch.timeUs<=end) {
   mulEkfHistoryBatch_t *entry=&s->batches[at];
   if(entry->batch.timeUs<s->replay.timeUs)return false;
   if(entry->batch.timeUs>s->replay.timeUs) {
    if(!interval)return false;
    const mulEkfInterval_t *step=&s->intervals[interval-1];
    double fraction=(double)(entry->batch.timeUs-s->replay.timeUs)/(double)(step->endUs-step->startUs);
    double q[3];for(unsigned k=0;k<3;k++)q[k]=step->q[k]*fraction;
    if(mulEkfPredict(&s->replay,entry->batch.timeUs,step->leftM*fraction,step->rightM*fraction,q)!=mulEkfOk)return false;
   }
   entry->status=mulEkfUpdate(&s->replay,&entry->batch,&s->work);
   if(entry->status==mulEkfNumeric)return false;
   entry->innovationCount=(entry->status==mulEkfOk||entry->status==mulEkfGated)?s->replay.innovationCount:0;
   entry->nis=entry->innovationCount?s->replay.nis:0;
   if(entry->innovationCount)memcpy(entry->innovation,s->replay.innovation,sizeof(entry->innovation));
   at++;
  }
  if(interval) {
   const mulEkfInterval_t *step=&s->intervals[interval-1];
   if(s->replay.timeUs<end) {
    double fraction=(double)(end-s->replay.timeUs)/(double)(step->endUs-step->startUs);
    double q[3];for(unsigned k=0;k<3;k++)q[k]=step->q[k]*fraction;
    if(mulEkfPredict(&s->replay,end,step->leftM*fraction,step->rightM*fraction,q)!=mulEkfOk)return false;
   }
   if(interval==1)s->firstCheckpoint=s->replay;
  }
 }
 s->current=s->replay;s->revision++;s->rebuilds++;return true;
}
static bool evict(mulEkfStream_t *s) {
 if(!s->intervalCount)return false;
 s->base=s->firstCheckpoint;s->trimmed=true;
 s->intervalCount--;memmove(s->intervals,s->intervals+1,s->intervalCount*sizeof(s->intervals[0]));
 unsigned remove=0;while(remove<s->batchCount&&s->batches[remove].batch.timeUs<=s->base.timeUs)remove++;
 s->batchCount=(uint8_t)(s->batchCount-remove);memmove(s->batches,s->batches+remove,s->batchCount*sizeof(s->batches[0]));
 return rebuild(s);
}
mulEkfStatus_t mulEkfStreamOdom(mulEkfStream_t *s,const mulEkfOdom_t *p) {
 if(!s||!s->healthy||!s->current.enabled)return mulEkfDisabled;
 if(!p||p->clock!=s->base.clock||p->epoch!=s->base.epoch)return mulEkfDomain;
 if(!s->hasOdom) {
  // Reuse all declared-width/provenance/uncertainty checks without fabricating a
  // motion interval. A same-sample delta must reach the timestamp guard.
  double l,r,q[3];mulEkfStatus_t status=mulEkfWheelDelta(&s->base.config,p,p,&l,&r,q);
  if(status!=mulEkfTime)return status;
  if(p->timeUs!=s->base.timeUs)return mulEkfTime;
  s->previous=*p;s->hasOdom=true;return mulEkfBaseline;
 }
 double left,right,q[3];mulEkfStatus_t status=mulEkfWheelDelta(&s->base.config,&s->previous,p,&left,&right,q);
 if(status!=mulEkfOk){s->rejected++;return status;}
 if(s->intervalCount==MUL_EKF_INTERVALS&&!evict(s)){s->healthy=false;return mulEkfNumeric;}
 mulEkfInterval_t *step=&s->intervals[s->intervalCount++];
 *step=(mulEkfInterval_t){.startUs=s->previous.timeUs,.endUs=p->timeUs,.leftM=left,.rightM=right};memcpy(step->q,q,sizeof(q));
 s->previous=*p;
 if(!rebuild(s)){s->healthy=false;return mulEkfNumeric;}
 while(s->intervalCount&&s->current.timeUs-s->base.timeUs>s->base.config.lagUs)if(!evict(s)){s->healthy=false;return mulEkfNumeric;}
 return mulEkfOk;
}
mulEkfStatus_t mulEkfStreamBatch(mulEkfStream_t *s,const mulEkfBatch_t *b) {
 if(!s||!s->healthy||!s->current.enabled)return mulEkfDisabled;
 mulEkfStatus_t valid=mulEkfBatchValid(&s->base,b,&s->work);if(valid!=mulEkfOk){s->rejected++;return valid;}
 if(b->timeUs<s->base.timeUs||(s->trimmed&&b->timeUs==s->base.timeUs)){s->late++;return mulEkfTooLate;}
 if(b->timeUs>s->current.timeUs&&b->timeUs-s->current.timeUs>s->base.config.maxIntervalUs)return mulEkfTime;
 for(unsigned i=0;i<s->batchCount;i++) {
  if(s->batches[i].batch.group==b->group)return mulEkfDuplicate;
  for(unsigned j=0;j<b->count;j++)for(unsigned k=0;k<s->batches[i].batch.count;k++)if(b->measurements[j].id==s->batches[i].batch.measurements[k].id)return mulEkfDuplicate;
 }
 for(unsigned i=0;i<s->base.groupCount;i++)if(s->base.groups[i]==b->group)return mulEkfDuplicate;
 for(unsigned i=0;i<b->count;i++)for(unsigned j=0;j<s->base.idCount;j++)if(b->measurements[i].id==s->base.ids[j])return mulEkfDuplicate;
 if(s->batchCount==MUL_EKF_BATCHES)return mulEkfCapacity;
 unsigned at=0;while(at<s->batchCount&&order(&s->batches[at].batch,b))at++;
 memmove(s->batches+at+1,s->batches+at,(s->batchCount-at)*sizeof(s->batches[0]));
 s->batches[at]=(mulEkfHistoryBatch_t){.batch=*b,.status=mulEkfQueued};s->batchCount++;
 if(!rebuild(s)){s->healthy=false;return mulEkfNumeric;}
 return s->batches[at].status;
}
