/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef MUL_PROPAGATION_H
#define MUL_PROPAGATION_H
#include <stdint.h>
// Strict v1 compatibility by default. Opt into continuous v2 on all nodes.
#ifndef MUL_PROP_MAX_AGE_MS
#define MUL_PROP_MAX_AGE_MS 1000
#endif
#if MUL_PROP_MAX_AGE_MS < 1000 || MUL_PROP_MAX_AGE_MS > 6000
#error "MUL_PROP_MAX_AGE_MS must be between 1000 and 6000"
#endif
#ifndef MUL_RELATIVE_SPEED_MM_S
#define MUL_RELATIVE_SPEED_MM_S (-1) // No physical motion guarantee.
#endif
#if MUL_RELATIVE_SPEED_MM_S < -1 || MUL_RELATIVE_SPEED_MM_S > 1000000
#error "MUL_RELATIVE_SPEED_MM_S must be -1 (UNKNOWN) or 0..1000000"
#endif
typedef enum {
  mulPropagationNone,
  mulPropagationFresh, // Measurement cycle or first reference after it.
  mulPropagationReused,
  mulPropagationExpired,
  mulPropagationInvalidated,
} mulPropagationState_t;
#endif
