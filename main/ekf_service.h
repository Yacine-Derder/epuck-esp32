#ifndef EKF_SERVICE_H
#define EKF_SERVICE_H
#include "mul_ekf_stream.h"
#define EKF_SERVICE_LINE 320u
typedef struct {
  mulEkfConfig_t config;
  mulEkfStream_t *stream;
  mulEkfBatch_t assembly;
  uint8_t nodes[MUL_EKF_MEASUREMENTS], references[MUL_EKF_MEASUREMENTS], rowMask, observationMask;
  bool assembling;
} ekf_service_t;
// Zero-initialize before first use; clear on session loss.
// Single-owner command service. No sockets, motors, ROS, MCS or implicit DATA2
// odometry backend. A measured input producer/time alignment remains external.
void ekf_service_clear(ekf_service_t *service);
// Returns0 for other namespaces; handled commands always produce one bounded line.
int ekf_service_command(ekf_service_t *service, const char *line, uint32_t receivedMs,
                        char out[EKF_SERVICE_LINE]);
#endif
