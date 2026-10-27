/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef MUL_POSE_H
#define MUL_POSE_H
#include "mul_clock.h"
#define MUL_POSE_BYTES 80u
#define MUL_POSE_MAX 8u
#define MUL_POSE_VALID 1u
#define MUL_POSE_COVARIANCE 2u
// Source explicitly declares zero cross-robot position-noise covariance.
// This says nothing about correlations between successive observations.
#define MUL_POSE_INDEPENDENT 4u
#define MUL_POSE_AGE_UNKNOWN UINT16_MAX
typedef struct {
 uint64_t sourceUs;
 uint32_t clock,epoch,frame,sequence;
 int32_t positionMm[3],covarianceMm2[6]; // xx,xy,xz,yy,yz,zz
 uint32_t receivedMs; // Local monotonic reception domain; not serialized.
 uint16_t ageUpperMs,ttlMs,timeUncertaintyMs; // ttlMs0: local expired tombstone only.
 uint8_t node,flags;
} mulPose_t;
typedef struct {
 mulPose_t poses[MUL_POSE_MAX];
 uint32_t clock,epoch,frame;
 uint16_t maxSkewMs;
 uint8_t count;
 bool active;
} mulPoseStream_t;
typedef enum {
 mulPoseAvailable,mulPoseMissing,mulPoseInvalid,mulPoseDomain,mulPoseFrame,
 mulPoseSkew,mulPoseAgeUnknown,mulPoseStale,mulPoseDistance,
 mulPoseReordered,mulPoseFull,
} mulPoseStatus_t;
typedef struct {
 uint64_t sourceUs[2];
 uint32_t clock,epoch,frame,sequence[2];
 uint8_t reference,follower;
 bool valid;
 double distanceMm,varianceMm2; // First-order supplied covariance projection.
 uint32_t measuredMs,ageMs;
 uint16_t ttlMs,skewMs;
 int64_t delayDtu; // Nominal reference DTU; physical clock scale remains unknown.
 bool covarianceKnown;
} mulPoseGeometry_t;
bool mulPoseValid(const mulPose_t *p);
bool mulPoseEncode(uint8_t out[MUL_POSE_BYTES],const mulPose_t *p);
bool mulPoseDecode(const uint8_t in[MUL_POSE_BYTES],uint32_t receivedMs,mulPose_t *out);
bool mulPoseStreamBegin(mulPoseStream_t *s,uint32_t clock,uint32_t epoch,uint32_t frame,uint16_t maxSkewMs);
// Commit requires prior mulPoseValid validation; only bounded integer/copy work.
mulPoseStatus_t mulPoseStreamCommit(mulPoseStream_t *s,const mulPose_t *p);
mulPoseStatus_t mulPoseStreamPut(mulPoseStream_t *s,const mulPose_t *p);
// Periodic caller service latches expiry before local uint32 uptime can alias.
// Expired entries keep identity/sequence/source time; only newer input revives them.
unsigned mulPoseStreamExpire(mulPoseStream_t *s,uint32_t nowMs);
const mulPose_t *mulPoseStreamGet(const mulPoseStream_t *s,uint8_t node);
// Input source timestamps must share clock/epoch/frame. AgeUpperMs is an explicit
// upper bound on observation age at LOCAL acceptance, including transit; UNKNOWN
// cannot produce a correction. Source timestamps are never subtracted from MCU time.
// Callers serialize stream access; all math works on immutable pose snapshots.
mulPoseStatus_t mulPoseGeometry(const mulPose_t *a,const mulPose_t *b,uint32_t nowMs,
 uint16_t maxSkewMs,mulPoseGeometry_t *out);
bool mulPoseDelay(const mulNetwork_t *network,const mulClock_t *clock,
 const mulPoseGeometry_t *geometry,mulClockDelay_t *out);
#endif
