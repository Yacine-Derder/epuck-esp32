/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef MUL_EKF_STREAM_H
#define MUL_EKF_STREAM_H
#include "mul_ekf.h"
#define MUL_EKF_INTERVALS 8u
#define MUL_EKF_BATCHES 4u
typedef struct {
 uint64_t startUs,endUs;
 double leftM,rightM,q[3];
} mulEkfInterval_t;
typedef struct {
 mulEkfBatch_t batch;
 mulEkfStatus_t status;
 uint8_t innovationCount;
 double nis,innovation[MUL_EKF_MEASUREMENTS];
} mulEkfHistoryBatch_t;
typedef struct {
 mulEkf_t base,current,firstCheckpoint,replay;
 mulEkfWork_t work;
 mulEkfOdom_t previous;
 mulEkfInterval_t intervals[MUL_EKF_INTERVALS];
 mulEkfHistoryBatch_t batches[MUL_EKF_BATCHES];
 uint32_t revision,late,rejected,rebuilds;
 uint8_t intervalCount,batchCount;
 bool hasOdom,trimmed,healthy;
} mulEkfStream_t;
bool mulEkfStreamInit(mulEkfStream_t *s,const mulEkfConfig_t *config,uint32_t clock,uint32_t epoch,uint32_t frame,
 uint64_t timeUs,const double x[3],const double covariance[3][3]);
bool mulEkfStreamEnable(mulEkfStream_t *s,bool enabled);
// Measured interval endpoints define a constant-curvature interpolation mean.
// Configured process variance is distributed as white interval noise; this is an
// explicit experimental model, not extra wheel samples or commanded speed.
// Rebuild is bounded by8 intervals/4 joint batches, no heap or global workspace.
mulEkfStatus_t mulEkfStreamOdom(mulEkfStream_t *s,const mulEkfOdom_t *sample);
// Delayed input inside retained history is inserted in source-time/group order
// and the posterior is replayed. Earlier-than-checkpoint data is rejected, never
// applied at receipt time. Future batches wait for measured odometry coverage.
mulEkfStatus_t mulEkfStreamBatch(mulEkfStream_t *s,const mulEkfBatch_t *batch);
#endif
