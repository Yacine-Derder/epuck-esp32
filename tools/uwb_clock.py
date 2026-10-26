"""Additive UWB3 type10 clock diagnostics; no host clock correction is applied."""
import struct


def decode_clock(data, out):
    tx = int.from_bytes(data[40:45], 'little')
    rx = int.from_bytes(data[45:50], 'little')
    sequence, applied, fitted, rms, rejected, sigma, age = struct.unpack_from('<IiiIIII', data, 50)
    ref, method, window, count, inliers, status, ready, used, fallback, compensated = data[78:88]
    if (data[2] != 3 or data[6] != 2 or data[8] != 2 or data[7] not in (4, 5)
            or ref == 255 or ref == data[4] or method > 1 or not 8 <= window <= 32
            or count > window or inliers > count or status > 3 or ready > 1 or used > 1
            or fallback > 3 or compensated > 1 or abs(applied) > 200000 or abs(fitted) > 200000
            or (used and (not method or not ready or status != 2 or fallback)) or any(data[88:150])):
        raise ValueError('clock envelope')
    for key in ('raw_mm', 'calibrated_mm', 'raw_valid', 'calibration', 'calibrated_valid'):
        out.pop(key)
    out.update(reference_tx_dtu=tx, local_rx_dtu=rx, reference_sequence=sequence, reference_node=ref,
               applied_ppb=applied, fitted_ppb=fitted, residual_rms_dtu=rms, rejected_samples=rejected,
               fit_sigma_ppb=None if sigma == 0xffffffff else sigma, mapping_age_ms=age,
               clock_estimator=('median', 'regression')[method], clock_window=window,
               observation_count=count, inlier_count=inliers,
               mapping_status=('invalid', 'warmup', 'valid', 'stale')[status], fit_ready=bool(ready),
               fit_applied=bool(used), fallback=('used', 'warmup', 'changing_rate', 'outlier')[fallback],
               propagation_compensated=bool(compensated), mapping_epoch=out['exchange'],
               physical_uncertainty='unknown', fit_uncertainty_scope='conditional scatter; excludes drift/geometry/propagation',
               source='stm32_onboard', local_clock_domain=dict(node=out['node'], radio=out['radio'], epoch=out['epoch']))
    return out
