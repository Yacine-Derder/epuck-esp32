"""Compact projection, optional best-effort live display, and non-destructive log copy.

No localization arithmetic. Measurement values are decoded onboard results.
Raw recording bypasses this projection and retains the original collector schema.
"""
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import threading
import uuid

DEFAULT_LOG_DIR = Path.home() / 'Documents' / 'uwb_logs'
OLD_LOG_DIR = Path.home() / 'uwb_logs'


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='milliseconds').replace('+00:00', 'Z')


class CompactLog:
    def __init__(self, output, endpoint, health_seconds=30, metadata=None):
        if not math.isfinite(health_seconds) or health_seconds < 0:
            raise ValueError('health interval must be finite and nonnegative')
        self.output, self.health_seconds = output, health_seconds
        self.capture = uuid.uuid4().hex[:16]
        self.sessions, self.rosters, self.health_at = {}, {}, {}
        self.counter = 0
        self.bridge_counts = None
        self.write(dict(type='capture', schema=1, capture=self.capture, endpoint=endpoint,
                        measurement_source='stm32_onboard', host_time='UTC reception',
                        measurement_time='receiver u_ms; wraps uint32', uncertainty='unknown',
                        **({'experiment': metadata} if metadata is not None else {})))

    def write(self, row):
        row.setdefault('t', utc())
        row['t'] = datetime.datetime.fromisoformat(row['t'].replace('Z', '+00:00')).isoformat(timespec='milliseconds').replace('+00:00', 'Z')
        self.output.write(json.dumps(row, separators=(',', ':'), allow_nan=False) + '\n')
        self.output.flush()

    def session(self, row):
        key = tuple(row[k] for k in ('node', 'radio', 'epoch', 'mode', 'role'))
        if key not in self.sessions:
            self.counter += 1
            self.sessions[key] = f'{self.capture}-{self.counter}'
            if len(self.sessions) > 128:
                old = self.sessions.pop(next(iter(self.sessions)))
                self.rosters.pop(old, None)
                self.health_at.pop(old, None)
            self.write(dict(type='session', s=self.sessions[key], node=row['node'], radio=row['radio'],
                            epoch=row['epoch'], mode=row['mode'], role=row['role']))
        return self.sessions[key]

    def accept(self, row):
        kind = row['kind']
        t = row['host_received_utc']
        if kind in ('connected', 'disconnected'):
            fields = dict(type='connection', capture=self.capture, t=t,
                          state=kind, connections=row['connections'])
            if kind == 'disconnected':
                fields['reason'] = row['reason']
            self.write(fields)
            return
        if kind == 'corrupt':
            self.write(dict(type='error', t=t, capture=self.capture, reason=row['reason'], corrupt=row['count']))
            return
        if kind == 'bridge':
            line = row['line']
            if line.startswith('UWBSTAT '):
                try:
                    fields = dict(part.split('=', 1) for part in line.split()[1:])
                    counts = tuple(int(fields[k]) for k in ('dropped', 'corrupt', 'uart_errors')) + (int(fields.get('ekf_dropped', 0)),)
                    if min(counts) < 0:
                        raise ValueError('negative counter')
                except (ValueError, KeyError):
                    self.write(dict(type='error', t=t, capture=self.capture, message=line))
                    return
                if counts != self.bridge_counts and (any(counts) or self.bridge_counts is not None):
                    self.write(dict(type='transport', t=t, capture=self.capture,
                                    bridge_dropped=counts[0], corrupt=counts[1], uart_errors=counts[2], ekf_dropped=counts[3],
                                    reset=self.bridge_counts is not None and any(a < b for a, b in zip(counts, self.bridge_counts))))
                self.bridge_counts = counts
            elif line.startswith(('EKF1 ', 'EKF ERR ')):
                self.write(dict(type='ekf', t=t, capture=self.capture, source='esp32_ekf', message=line))
            elif line.startswith(('SESSION ', 'DISCONNECT ', 'DIAG ', 'ERR ')) or any(word in line.lower() for word in ('fault', 'overflow', 'error:')):
                self.write(dict(type='message', t=t, capture=self.capture, message=line))
            return
        if kind != 'telemetry':
            return
        event = row['type']
        # Raw packets/peer tables are deliberately omitted from routine experiments.
        if event not in ('tdoa', 'tdoa_vector', 'clock', 'range', 'summary', 'ar_range', 'ar_quality') and not row.get('missing'):
            return
        sid = self.session(row)
        base = dict(t=t, s=sid)
        if row.get('missing'):
            self.write(dict(base, type='gap', missing=row['missing'], total=row['missing_total']))
        if row.get('sequence_status') in ('duplicate', 'old'):
            self.write(dict(base, type='ordering', state=row['sequence_status'], seq=row['sequence']))
            return
        if event == 'clock':
            fields = ('reference_node', 'reference_sequence', 'reference_tx_dtu', 'local_rx_dtu',
                      'applied_ppb', 'fitted_ppb', 'residual_rms_dtu', 'rejected_samples', 'fit_sigma_ppb',
                      'mapping_age_ms', 'clock_estimator', 'clock_window', 'observation_count', 'inlier_count',
                      'mapping_status', 'fit_ready', 'fit_applied', 'fallback', 'propagation_compensated',
                      'mapping_epoch', 'physical_uncertainty', 'fit_uncertainty_scope',
                      'clock_record_version', 'sample_period_ms', 'capture_age_ms', 'capture_valid')
            self.write(dict(base, type='clock', node=row['node'], radio=row['radio'], epoch=row['epoch'],
                            u_ms=row['uptime_ms'], **{field: row[field] for field in fields}))
        elif event == 'tdoa_vector':
            self.write(dict(base, type='tdoa_vector', cycle=row['cycle'], u_ms=row['uptime_ms'],
                            reference_node=row['reference_node'], alr_ids=row['alr_ids'],
                            schedule_revision=row['schedule_revision'], schedule_fingerprint=row['schedule_fingerprint'],
                            slot_ms=row['slot_ms'], cycle_ms=row['cycle_ms'], rate_policy=row['rate_policy'],
                            schedule_profile=row['schedule_profile'], experimental_timing=row['experimental_timing'],
                            fingerprint_scope=row.get('fingerprint_scope', 'broadcast_schedule'),
                            seen_mask=row['seen_mask'], valid_mask=row['valid_mask'], followers=row['followers'],
                            **{k:row[k] for k in ('propagation_metadata_version','physical_uncertainty') if k in row}))
        elif event == 'tdoa':
            roster = (row['reference_node'], row['follower_node'], row['rate_policy'], row['slot_ms'])
            if self.rosters.get(sid) != roster:
                self.rosters[sid] = roster
                self.write(dict(base, type='roster', ref_node=roster[0], fol_node=roster[1], rate_policy=roster[2], slot_ms=roster[3]))
            value = dict(base, type='tdoa', ref=row['reference_sequence'], fol=row['follower_sequence'],
                         u_ms=row['uptime_ms'], dt_dtu=row['tdoa_dtu'], dd_mm=row['range_diff_mm'])
            # A finite distance unambiguously means status=available, clock=true,
            # rate=stationary. Invalid rows retain the actual code/clock/rate.
            if not row['valid']:
                value.update(status=row['status'], clock=row['clock_valid'], rate=row['rate_state'])
            value.update({k:row[k] for k in ('propagation_metadata_version','propagation_source','physical_uncertainty') if k in row})
            self.write(value)
        elif event in ('ar_range','ar_quality'):
            keys=('cycle','schedule_fingerprint','schedule_revision','schedule_profile','fingerprint_scope','source','physical_uncertainty')
            if event=='ar_range':
                keys+=('initiator_node','responder_node','operation','reply_ms','raw_tof_dtu','tof_dtu','raw_range_mm','range_mm',
                       'reported_uptime_ms','acquisition_uptime_ms','measurement_age_at_report_ms','stamps','stamp_clock_nodes','tof_clock_node',
                       'power','power_corrected','calibrated','valid','remote_epoch_known')
            else:
                keys+=('attempts','completed','aborted','rejected','math_invalid','result_dropped','raw_dropped','late','radio_errors','timeouts',
                       'reset_generation','active','pending','reference_seen','enabled','last_reason','operation','stage')
            self.write(dict(base,type=event,u_ms=row['uptime_ms'],**{k:row[k] for k in keys}))
        elif event == 'range':
            calibrated = row['calibration'] and row['calibrated_valid']
            mm = row['calibrated_mm'] if calibrated else row['raw_mm'] if row['raw_valid'] else None
            if mm == -2147483648:
                mm = None
            value = dict(base, type='twr', peer=row['peer'], u_ms=row['uptime_ms'], mm=mm)
            if calibrated:
                value['calibrated'] = True
            self.write(value)
        elif event == 'summary' and self.health_seconds:
            now = row['host_monotonic_ns'] / 1e9
            if now - self.health_at.get(sid, -math.inf) >= self.health_seconds:
                self.health_at[sid] = now
                # Cumulative, not inferred current loss rates. Zero arrays omitted.
                names = ('attempts', 'completed', 'rejected', 'radio_errors', 'timeouts', 'dropped', 'uart_errors', 'tx_dropped')
                self.write(dict(base, type='health', **{k: row[k] for k in names if row.get(k)}))


class LiveMonitor:
    """Latest-state renderer on a daemon thread; no console I/O on acquisition."""
    def __init__(self, enabled=True, fd=None, interval=.5):
        self.enabled, self.fd, self.interval = enabled, 2 if fd is None else fd, interval
        self.lock, self.stop_event = threading.Lock(), threading.Event()
        self.states = {}
        self.thread = None
        if enabled:
            self.thread = threading.Thread(target=self._render, daemon=True, name='uwb-live')
            self.thread.start()

    def accept(self, endpoint, row):
        if not self.enabled:
            return
        with self.lock:
            s = self.states.setdefault(endpoint, dict(connection='connecting', valid=0, invalid=0, mean=0., m2=0., latest=None, status='waiting'))
            if row['kind'] in ('connected', 'disconnected'):
                s['connection'] = row['kind']
            elif row['kind'] == 'telemetry' and row['type'] == 'tdoa_vector' and row.get('sequence_status') not in ('old', 'duplicate'):
                valid = sum(value['valid'] for value in row['followers'])
                s['vector'] = {value['follower_node']: value['range_diff_mm'] for value in row['followers']}
                s['status'] = f"cycle={row['cycle']} {valid}/{len(row['followers'])} valid"
                s['valid'] += valid
                s['invalid'] += len(row['followers'])-valid
            elif row['kind'] == 'telemetry' and row['type'] == 'tdoa' and row.get('sequence_status') not in ('old', 'duplicate'):
                s['vector'] = None
                s['status'] = row['status_name']
                s['latest'] = row['range_diff_mm']
                if row['valid']:
                    s['valid'] += 1
                    x = row['range_diff_mm'] / 1000.
                    delta = x - s['mean']
                    s['mean'] += delta / s['valid']
                    s['m2'] += delta * (x - s['mean'])
                else:
                    s['invalid'] += 1

    def snapshot(self):
        with self.lock:
            return {endpoint: dict(s, std=math.sqrt(max(0., s['m2']) / (s['valid'] - 1)) if s['valid'] > 1 else 0.) for endpoint, s in self.states.items()}

    def _render(self):
        tty = os.isatty(self.fd)
        while not self.stop_event.wait(self.interval):
            parts = []
            for endpoint, s in self.snapshot().items():
                if s.get('vector') is not None:
                    values = ' '.join(f"0x{node:02X}={'--' if value is None else f'{value/1000.:+.3f}m'}" for node, value in s['vector'].items())
                    parts.append(f"{endpoint} {s['connection']} {s['status']} {values}")
                    continue
                latest = '--' if s['latest'] is None else f"{s['latest']/1000.:+.3f}m"
                mean = f"{s['mean']:+.3f}" if s['valid'] else '--'
                parts.append(f"{endpoint} {s['connection']} dd={latest} {s['status']} valid={s['valid']} invalid={s['invalid']} mean={mean}m sd={s['std']:.3f}m")
            if parts:
                try:
                    os.write(self.fd, (('\r\x1b[2K' if tty else '') + ' | '.join(parts) + ('' if tty else '\n')).encode())
                except OSError:
                    return

    def close(self):
        self.stop_event.set()
        if self.thread:
            self.thread.join(timeout=.2)


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def relevant_log(path):
    if not path.is_file() or path.is_symlink() or path.suffix.lower() not in ('.jsonl', '.log'):
        return False
    with path.open('rb') as source:
        prefix = source.read(65536)
    if path.suffix.lower() == '.jsonl':
        for line in prefix.splitlines():
            try:
                record = json.loads(line)
            except (ValueError, UnicodeError):
                continue
            if isinstance(record, dict) and ((record.get('kind') == 'telemetry' and
                    record.get('type') in ('summary', 'range', 'event', 'observation', 'peer', 'hybrid_rx', 'tdoa', 'tdoa_vector', 'multi_rx') and
                    all(key in record for key in ('node', 'radio', 'epoch'))) or
                    (record.get('type') == 'capture' and record.get('measurement_source') == 'stm32_onboard')):
                return True
        return False
    return b'MUL ALR:' in prefix or b'MULH ' in prefix


def migrate_logs(source=OLD_LOG_DIR, destination=DEFAULT_LOG_DIR):
    """Copy recognized project logs exclusively, verify hashes, retain every source.

    Collisions use a deterministic content-hash suffix; existing identical files
    are reused. Symlinks and unrecognized/unrelated files are never copied.
    """
    source, destination = Path(source), Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    result = dict(copied=[], identical=[], skipped=[], source=str(source), destination=str(destination))
    if not source.exists():
        return result
    if source.resolve() == destination.resolve():
        raise ValueError('source and destination are the same directory')
    for original in sorted(source.iterdir()):
        if not relevant_log(original):
            result['skipped'].append(str(original))
            continue
        checksum = digest(original)
        target = destination / original.name
        if target.exists() or target.is_symlink():
            if target.is_file() and not target.is_symlink() and digest(target) == checksum:
                result['identical'].append(str(target))
                continue
            target = destination / f'{original.stem}.collision-{checksum}{original.suffix}'
        if target.exists() or target.is_symlink():
            if not target.is_file() or target.is_symlink() or digest(target) != checksum:
                raise FileExistsError(f'conflicting migration destination: {target}')
            result['identical'].append(str(target))
            continue
        # Exclusive creation also protects against a destination race.
        try:
            with original.open('rb') as src, target.open('xb') as dst:
                shutil.copyfileobj(src, dst, 1024 * 1024)
                dst.flush()
                os.fsync(dst.fileno())
            if digest(target) != checksum or digest(original) != checksum:
                raise OSError(f'migration verification failed; originals retained: {original}')
        except Exception:
            # Keep all bytes for inspection; never delete an original or hide a
            # partial destination behind a success report.
            raise
        shutil.copystat(original, target, follow_symlinks=False)
        result['copied'].append(str(target))
    return result
