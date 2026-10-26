/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef MUL_PLAN_STORE_H
#define MUL_PLAN_STORE_H
#include "mul_ranging_plan.h"
#define MUL_PLAN_STORE_BASE 128u
#define MUL_PLAN_STORE_STRIDE 128u
#define MUL_PLAN_STORE_BYTES 96u
typedef enum {mulPlanAbsent,mulPlanStored,mulPlanCorrupt,mulPlanIoError} mulPlanStoreResult_t;
/* EEPROM semantics: byte rewriting (including0->1) and partial failed writes.
 * The adapter must serialize writers and supply bounded, synchronous readback. */
typedef struct {uint8_t bytes[MUL_PLAN_STORE_BYTES],verify[MUL_PLAN_STORE_BYTES];} mulPlanStoreWorkspace_t;
typedef struct {
 void *context;
 bool (*read)(void *,unsigned,void *,size_t);
 bool (*write)(void *,unsigned,const void *,size_t);
 mulPlanStoreWorkspace_t *workspace; // Exclusive caller-owned IO storage.
} mulPlanStore_t;
typedef struct {mulRangingPlan_t plan;uint32_t generation;uint8_t selected,slot;} mulStoredPlan_t;
mulPlanStoreResult_t mulPlanStoreLoad(const mulPlanStore_t *store,mulStoredPlan_t *out);
bool mulPlanStoreSave(const mulPlanStore_t *store,const mulRangingPlan_t *plan,bool selected,uint32_t *generation);
#endif
