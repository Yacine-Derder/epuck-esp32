#ifndef APP_EKF_H
#define APP_EKF_H
#include "ekf_service.h"
#include <stdbool.h>
#include <stdint.h>
// Bridge is the sole input producer/output consumer; worker owns all filter state.
// No UART writes or motor commands. Session change clears configuration and state.
void app_ekf_reset(uint32_t generation);
bool app_ekf_submit(uint32_t generation, const char *line, uint32_t receivedMs);
bool app_ekf_receive(uint32_t *generation, char line[EKF_SERVICE_LINE]);
uint32_t app_ekf_dropped(void);
#ifdef BRIDGE_HOST_TEST
bool app_ekf_worker_step(void); // Same worker body, explicitly pumped by host owner.
#endif
#endif
