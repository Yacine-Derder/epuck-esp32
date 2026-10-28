"""Capture-local loss accounting; never changes or repairs recorded measurements."""

def increments(rows, fields):
    """Counter samples are cumulative. First sample is a baseline, not a loss."""
    if not rows:
        return dict(baseline={}, last={}, observed_increments={}, resets=0)
    first = {k: rows[0].get(k, 0) for k in fields}
    previous, total, resets = first.copy(), dict.fromkeys(fields, 0), 0
    for row in rows[1:]:
        reset = False
        for key in fields:
            value = row.get(key, previous[key])
            delta = (value-previous[key]) & 0xffffffff
            if delta >= 0x80000000:
                # An observed restart/reset occurred after the previous sample.
                total[key] += value
                reset = True
            else:
                total[key] += delta
            previous[key] = value
        resets += int(reset)
    return dict(baseline=first, last=previous, observed_increments=total, resets=resets)


def audit(records, vectors, losses, incomplete):
    windows = {}
    for (sid, cycle), row in vectors.items():
        windows.setdefault(sid, dict(first=row, last=row))['last'] = row
    current, historical = [], []
    counters = {}
    for row, blob in records:
        counters.setdefault(row['s'], []).append(row)
        window = windows.get(row['s'])
        age = ((window['first']['u_ms']-row['cycle_start_ms']) & 0xffffffff) if window else 0
        # One prior cycle can straddle opening. A many-cycle-old queued blob is
        # demonstrably backlog, not a missing vector in this capture.
        if 2*row['cycle_ms'] < age < 0x80000000:
            historical.append(dict(s=row['s'], cycle=row['cycle'], diagnostic_id=row['diagnostic_id'], age_before_first_vector_ms=age))
        else:
            current.append((row, blob))
    samples = {sid: increments(rows, ['diagnostic_dropped']) for sid, rows in counters.items()}
    last, id_gaps, all_last, all_gaps = {}, 0, {}, 0
    for rows, positions, capture in ((records, all_last, False), (current, last, True)):
        for row, _ in rows:
            old = positions.get(row['s'])
            if old is not None:
                delta = (row['diagnostic_id']-old) & 0xffffffff
                if 1 < delta < 0x80000000:
                    if capture:id_gaps += delta-1
                    else:all_gaps += delta-1
            positions[row['s']] = row['diagnostic_id']
    bridge = increments([r for r in losses if r['type']=='transport'],
                        ['bridge_dropped', 'corrupt', 'uart_errors', 'ekf_dropped'])
    partials = []
    for row in incomplete:
        value = row.copy()
        window = windows.get(row['s'])
        if window:
            age = (window['first']['cycle']-row['cycle']) & 0xffffffff
            if 2 < age < 0x80000000:
                value['boundary_assessment'] = 'historical_initial_partial; connection_end is flush time'
            elif row['cycle']==window['first']['cycle']:
                value['boundary_assessment'] = 'initial_boundary_possible'
            elif row['cycle']==window['last']['cycle']:
                value['boundary_assessment'] = 'final_boundary_possible'
            else:
                value['boundary_assessment'] = 'interior_or_unresolved'
        partials.append(value)
    current_keys = {(r['s'], r['cycle']) for r, _ in current}
    return current, dict(
        current_total_cycles=len(current_keys | set(vectors)), current_reconstructible_cycles=len(current_keys), historical_replay_records=historical,
        current_missing_vector_records=sum((r['s'], r['cycle']) not in vectors for r, _ in current),
        diagnostic_id_gaps=id_gaps, diagnostic_id_gaps_including_backlog=all_gaps,
        diagnostic_dropped=sum(s['observed_increments']['diagnostic_dropped'] for s in samples.values()),
        diagnostic_dropped_boot_last=sum(s['last']['diagnostic_dropped'] for s in samples.values()),
        diagnostic_counter_samples=samples, bridge_counter_samples=bridge,
        incomplete_assessments=partials,
        counter_scope='diagnostic_dropped is observed capture delta; boot_last and baseline are cumulative; ID gaps exclude demonstrably historical backlog')
