/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef MUL_LEGACY_TIMING_H
#define MUL_LEGACY_TIMING_H

#include <stdint.h>

// Build all three nodes with the same slot. No EEPROM or wire-format change.
#ifndef MUL_FOLLOWER_SLOT_MS
#define MUL_FOLLOWER_SLOT_MS 500
#endif
#if MUL_FOLLOWER_SLOT_MS != 500 && MUL_FOLLOWER_SLOT_MS != 50 && MUL_FOLLOWER_SLOT_MS != 10
#error "MUL_FOLLOWER_SLOT_MS must be 500, 50 or 10"
#endif
#define MUL_FOLLOWER_SLOT_DTU (MUL_FOLLOWER_SLOT_MS * UINT64_C(63897600))

// Preserve the baseline +/-10% gate; allow 2 ms at the shortest slot.
#define MUL_SLOT_TOLERANCE_MS ((MUL_FOLLOWER_SLOT_MS / 10) < 2 ? 2 : (MUL_FOLLOWER_SLOT_MS / 10))
#define MUL_SLOT_MIN_MS (MUL_FOLLOWER_SLOT_MS - MUL_SLOT_TOLERANCE_MS)
#define MUL_SLOT_MAX_MS (MUL_FOLLOWER_SLOT_MS + MUL_SLOT_TOLERANCE_MS)
// Reference presence allowed extra completion/dispatch margin before TWR.
#define MUL_SLOT_PRESENCE_MAX_MS (MUL_FOLLOWER_SLOT_MS + 2 * MUL_SLOT_TOLERANCE_MS)
#define MUL_PAIR_MAX_AGE_MS (MUL_FOLLOWER_SLOT_MS + 200)
#define MUL_SLOT_MIN_DTU (MUL_SLOT_MIN_MS * INT64_C(63897600))
#define MUL_SLOT_MAX_DTU (MUL_SLOT_MAX_MS * INT64_C(63897600))

#endif
