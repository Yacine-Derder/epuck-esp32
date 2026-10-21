/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef MUL_DIAG_CONFIG_H
#define MUL_DIAG_CONFIG_H
#include <stdint.h>
#ifndef MUL_TWR_ROLE
#define MUL_TWR_ROLE 0
#endif
#ifndef MUL_TWR_SESSION
#define MUL_TWR_SESSION UINT64_C(0)
#endif
#if MUL_TWR_ROLE < 0 || MUL_TWR_ROLE > 2
#error "MUL_TWR_ROLE must be 0/off, 1/initiator or 2/responder"
#endif
#if MUL_TWR_ROLE == 1 && MUL_TWR_SESSION == 0
#error "Initiator requires a fresh nonzero MUL_TWR_SESSION for each boot/run"
#endif
#ifndef MUL_DIAG_ROLE
#define MUL_DIAG_ROLE 0
#endif
#ifndef PHY_INTEROP_ROLE
#define PHY_INTEROP_ROLE 0
#endif
#ifndef MUL_ADS_TRACE_TIMES
#define MUL_ADS_TRACE_TIMES 1
#endif
#if MUL_ADS_TRACE_TIMES != 0 && MUL_ADS_TRACE_TIMES != 1
#error "MUL_ADS_TRACE_TIMES must be 0 or 1"
#endif
#ifndef MUL_TIMING_TRACE
#define MUL_TIMING_TRACE 0
#endif
#if MUL_DIAG_ROLE < 0 || MUL_DIAG_ROLE > 2
#error "MUL_DIAG_ROLE must be 0 (off), 1 (TX), 2 (RX)"
#endif
#if (MUL_TWR_ROLE && (MUL_DIAG_ROLE || PHY_INTEROP_ROLE)) || (MUL_DIAG_ROLE && PHY_INTEROP_ROLE)
#error "Stage 3F and Stage 3G diagnostics are mutually exclusive"
#endif
#if MUL_TIMING_TRACE && !MUL_DIAG_ROLE && !MUL_TWR_ROLE
#error "MUL_TIMING_TRACE requires an opt-in MUL diagnostic or ADS profile"
#endif
#endif
