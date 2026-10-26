/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef MUL_SCHEDULE_H
#define MUL_SCHEDULE_H
#include "mul_common.h"
#define MUL_MAX_ALRS 8u
#define MUL_SCHEDULE_BYTES 32u
#define MUL_SCHEDULE_VERSION 1u
#define MUL_PROFILE_LEGACY 1u
#define MUL_PROFILE_MULTI 2u
#define MUL_PROFILE_EXPERIMENTAL 3u
#define MUL_RANGING_ALR_ALR 1u
#define MUL_RANGING_REPLY_MASK 6u //0=20ms,2=10ms,4=5ms;6 reserved.
#define MUL_SCHEDULE_CYCLE_MS 1000u
#define MUL_DATA_GUARD_MS 20u
#define MUL_SYNC_WINDOWS 2u
typedef struct {
 uint32_t revision;
 uint16_t slotMs,periodMs;
 uint8_t count,profile,ids[MUL_MAX_ALRS];
 uint16_t txLeadUs,rxBudgetUs,airtimeUs;
 uint8_t guard100us,rangingFlags;
} mulSchedule_t;
void mulScheduleExperimental(mulSchedule_t *s);
uint16_t mulScheduleReplyMs(const mulSchedule_t *s);
uint16_t mulScheduleLeadUs(const mulSchedule_t *s);
bool mulScheduleAlrRanging(const mulSchedule_t *s);
void mulScheduleLegacy(mulSchedule_t *s,uint8_t reference,uint8_t follower,uint16_t slotMs);
const char *mulScheduleValidate(const mulSchedule_t *s);
int mulScheduleIndex(const mulSchedule_t *s,uint8_t id);
bool mulSchedulePair(const mulSchedule_t *s,unsigned follower,mulNetwork_t *pair);
uint32_t mulScheduleFingerprint(const mulSchedule_t *s);
bool mulScheduleEncode(uint8_t out[MUL_SCHEDULE_BYTES],const mulSchedule_t *s);
bool mulScheduleDecode(const uint8_t in[MUL_SCHEDULE_BYTES],mulSchedule_t *s);
uint32_t mulScheduleCrc(const uint8_t *data,size_t n);
uint16_t mulScheduleOffsetMs(const mulSchedule_t *s,unsigned slot);
/* Returns follower slot,0 if this window is unused. Cycle0 is acquisition only. */
unsigned mulScheduleSyncOwner(const mulSchedule_t *s,uint32_t cycle,unsigned window);
uint16_t mulScheduleSyncPollMs(unsigned window);
uint16_t mulScheduleSyncEndMs(unsigned window);
#endif
