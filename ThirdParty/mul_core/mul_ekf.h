/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef MUL_EKF_H
#define MUL_EKF_H
#include "mul_pose.h"
#define MUL_EKF_MEASUREMENTS 8u
#define MUL_EKF_GROUPS 32u
#define MUL_EKF_IDS 64u
#define MUL_EKF_TDOA 1u
#define MUL_EKF_RANGE 2u
enum {mulOdomUnknown,mulOdomCommanded,mulOdomMeasured,mulOdomSyntheticMeasured};
typedef enum {
 mulEkfOk,mulEkfDisabled,mulEkfInvalid,mulEkfTime,mulEkfDomain,mulEkfProvenance,
 mulEkfGap,mulEkfAmbiguous,mulEkfCovariance,mulEkfGeometry,mulEkfGated,
 mulEkfDuplicate,mulEkfCorrelation,mulEkfCapacity,mulEkfQueued,mulEkfBaseline,
 mulEkfTooLate,mulEkfNumeric,
} mulEkfStatus_t;
typedef struct {
 double wheelBaseM,metresPerCount,variancePerCountM2,varianceFloorM2,wheelCorrelation;
 double leverXM,leverYM,heightM,gatePerDof;
 uint32_t maxDeltaCount,maxIntervalUs,maxPoseSkewUs,maxTimeUncertaintyUs,lagUs;
 uint8_t modes,allowSynthetic,experimentalNoise;
} mulEkfConfig_t;
typedef struct {
 uint64_t timeUs;
 uint32_t clock,epoch,sequence,left,right,timeUncertaintyUs;
 uint8_t bits,provenance;
} mulEkfOdom_t;
typedef struct {
 uint64_t id;
 double valueM;
 uint8_t kind,anchor,reference,valid,rateValid;
} mulEkfMeasurement_t;
typedef struct {
 uint64_t timeUs,group;
 uint32_t clock,epoch,frame,timeUncertaintyUs;
 uint8_t count,anchorCount,independentPoseTimes;
 mulPose_t anchors[MUL_POSE_MAX];
 mulEkfMeasurement_t measurements[MUL_EKF_MEASUREMENTS];
 // Explicit joint UWB noise covariance, m^2, stride MUL_EKF_MEASUREMENTS.
 // Shared ALR pose covariance is added by the filter, not included here.
 double noise[MUL_EKF_MEASUREMENTS][MUL_EKF_MEASUREMENTS];
} mulEkfBatch_t;
typedef struct {
 uint64_t sourceUs;
 uint32_t sequence;
 uint8_t node;
} mulEkfPoseUse_t;
typedef struct {
 mulEkfConfig_t config;
 double x[3],p[3][3],innovation[MUL_EKF_MEASUREMENTS],nis;
 uint64_t timeUs,lastGroup,groups[MUL_EKF_GROUPS],ids[MUL_EKF_IDS];
 mulEkfPoseUse_t used[MUL_POSE_MAX];
 uint32_t clock,epoch,frame,predictions,updates,gated,duplicates;
 uint8_t groupNext,groupCount,idNext,idCount,usedCount,innovationCount;
 bool initialized,enabled;
} mulEkf_t;
// Caller-owned scratch: no dynamic allocation or implicit global workspace.
typedef struct {
 double h[MUL_EKF_MEASUREMENTS][3],j[MUL_EKF_MEASUREMENTS][MUL_POSE_MAX][3];
 double r[MUL_EKF_MEASUREMENTS][MUL_EKF_MEASUREMENTS],s[MUL_EKF_MEASUREMENTS][MUL_EKF_MEASUREMENTS];
 double ph[3][MUL_EKF_MEASUREMENTS],k[3][MUL_EKF_MEASUREMENTS],residual[MUL_EKF_MEASUREMENTS],temp[MUL_EKF_MEASUREMENTS];
} mulEkfWork_t;
const char *mulEkfStatusName(mulEkfStatus_t status);
bool mulEkfConfigValid(const mulEkfConfig_t *config);
bool mulEkfInit(mulEkf_t *s,const mulEkfConfig_t *config,uint32_t clock,uint32_t epoch,uint32_t frame,
 uint64_t timeUs,const double x[3],const double covariance[3][3]);
bool mulEkfEnable(mulEkf_t *s,bool enabled);
double mulEkfWrap(double radians);
// Exact constant-curvature mean and analytic differential-drive Jacobians.
bool mulEkfMotion(const mulEkfConfig_t *config,const double x[3],double leftM,double rightM,
 double predicted[3],double f[3][3],double g[3][2]);
mulEkfStatus_t mulEkfPredict(mulEkf_t *s,uint64_t timeUs,double leftM,double rightM,const double wheelCovariance[3]);
// Validates measured provenance, declared16/32-bit counter width, modular deltas,
// source time/epoch and a configured experimental white per-count noise model.
mulEkfStatus_t mulEkfWheelDelta(const mulEkfConfig_t *config,const mulEkfOdom_t *previous,
 const mulEkfOdom_t *current,double *leftM,double *rightM,double covariance[3]);
mulEkfStatus_t mulEkfBatchValid(const mulEkf_t *s,const mulEkfBatch_t *batch,mulEkfWork_t *work);
// range gradient wrt receiver state and ALR position; used in finite-difference tests.
bool mulEkfRangeModel(const mulEkfConfig_t *config,const double x[3],const mulPose_t *anchor,
 double *range,double stateJacobian[3],double anchorJacobian[3]);
// Atomic joint Joseph update at EXACT state time. A correlation group may be used
// only once; repeated ALR pose observations require a joint update, not reuse as
// independent noise. Unknown/correlated-across-time pose models are rejected.
mulEkfStatus_t mulEkfUpdate(mulEkf_t *s,const mulEkfBatch_t *batch,mulEkfWork_t *work);
#endif
