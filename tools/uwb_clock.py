"""Additive UWB3 type10 clock diagnostics; no host clock correction is applied."""
import struct


def decode_clock(data, out):
    tx = int.from_bytes(data[40:45], 'little')
    rx = int.from_bytes(data[45:50], 'little')
    sequence, applied, fitted, rms, rejected, sigma, age = struct.unpack_from('<IiiIIII', data, 50)
    ref, method, window, count, inliers, status, ready, used, fallback, compensated = data[78:88]
    version = 1 if data[88] == 0 else data[88]
    period = out['transaction'] or 1000
    if (version not in (1, 2, 3) or data[89] or any(data[92 if version >= 2 else 88:150])
            or (version >= 2 and not 10 <= period <= 2000)):
        raise ValueError('clock version/period/reserved')
    capture_age = int.from_bytes(data[90:92], 'little') if version >= 2 else None
    if (data[2] != 3 or data[6] != 2 or data[8] != 2 or data[7] not in (4, 5)
            or ref == 255 or ref == data[4]
            or ((method not in (0, 2) or window != 5) if version == 3 else (method > 1 or not 8 <= window <= 32))
            or count > window or inliers > count or status > 3 or ready > 1 or used > 1
            or fallback > 3 or compensated > 1 or abs(applied) > 200000 or abs(fitted) > 200000
            or (used and (not method or not ready or status != 2 or fallback))):
        raise ValueError('clock envelope')
    for key in ('raw_mm', 'calibrated_mm', 'raw_valid', 'calibration', 'calibrated_valid'):
        out.pop(key)
    out.update(clock_record_version=version, sample_period_ms=period if version >= 2 else None,
               capture_age_ms=capture_age, capture_valid=None if capture_age is None else capture_age <= 15,
               reference_tx_dtu=tx, local_rx_dtu=rx, reference_sequence=sequence, reference_node=ref,
               applied_ppb=applied, fitted_ppb=fitted, residual_rms_dtu=rms if version < 3 else None,
               innovation_abs_dtu=rms if version == 3 and method == 2 else None, rejected_samples=rejected,
               fit_sigma_ppb=None if sigma == 0xffffffff else sigma, mapping_age_ms=age,
               clock_estimator=('kalman' if method == 2 else 'median_5') if version == 3 else ('median', 'regression')[method], clock_window=window,
               observation_count=count, inlier_count=inliers,
               mapping_status=('invalid', 'warmup', 'valid', 'stale')[status], fit_ready=bool(ready),
               fit_applied=bool(used), fallback=(('used','warmup','uncertain','numerical') if version == 3 else ('used', 'warmup', 'changing_rate', 'outlier'))[fallback],
               propagation_compensated=bool(compensated), mapping_epoch=out['exchange'],
               physical_uncertainty='unknown', fit_uncertainty_scope=('conditional Kalman covariance; excludes geometry/propagation' if version == 3 and method == 2 else 'conditional scatter; excludes drift/geometry/propagation'),
               source='stm32_onboard', local_clock_domain=dict(node=out['node'], radio=out['radio'], epoch=out['epoch']))
    out['applied_estimator'] = ('kalman' if version == 3 else 'regression') if used else ('median_5' if version == 3 else 'median') if status == 2 else 'unavailable'
    return out
