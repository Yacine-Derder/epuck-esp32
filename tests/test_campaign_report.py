import json
from pathlib import Path
import tempfile
import unittest
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from mul_campaign_report import analyze


class CampaignReport(unittest.TestCase):
    def test_session_identity_invalid_missing_and_exact_truth(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d)/'log.jsonl'
            rows = [dict(type='session', s='c-1', node=141, radio=2, epoch='e1'),
                    dict(type='tdoa', s='c-1', ref=1, fol=2, u_ms=100, dd_mm=1000),
                    dict(type='tdoa', s='c-1', ref=2, fol=3, u_ms=1100, dd_mm=None, status=1),
                    dict(type='session', s='c-2', node=141, radio=2, epoch='e2'),
                    dict(type='tdoa', s='c-2', ref=1, fol=2, u_ms=100, dd_mm=1200)]
            path.write_text('\n'.join(map(json.dumps, rows)))
            result = analyze([path])
            self.assertEqual(result['samples']['tdoa']['valid'], 2)
            self.assertEqual(result['samples']['tdoa']['total'], 3)
            keys = result['raw_keys']
            self.assertNotEqual(keys[0]['sample_key'], keys[2]['sample_key'])
            truth = {keys[0]['sample_key']: .9, keys[2]['sample_key']: 1., 'unmatched': 0}
            result = analyze([path], truth)
            self.assertAlmostEqual(result['samples']['tdoa']['aligned_errors_m']['bias'], .15)
            self.assertEqual(result['truth_unmatched'], 1)

    def test_counter_wrap_excludes_first_and_discontinuity(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d)/'log.jsonl'
            counters = ('attempts', 'completed', 'aborted', 'rejected', 'math_invalid', 'result_dropped', 'raw_dropped', 'late', 'radio_errors', 'timeouts')
            rows = [dict(type='ar_quality', node=141, epoch='a', reset_generation=0,
                         **dict.fromkeys(counters, n)) for n in (0xfffffffe, 2, 1)]
            path.write_text('\n'.join(map(json.dumps, rows)))
            report = analyze([path])
            self.assertEqual(report['quality']['observed_counter_deltas']['attempts'], 4)
            self.assertEqual(report['quality']['counter_discontinuities']['attempts'], 1)
            self.assertEqual(report['quality']['completed_per_attempt'], 1)

    def test_acquisition_freshness_and_same_node_twr_spans_wrap(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d)/'log.jsonl'
            rows = [dict(type='session', s='one', node=141, radio=2, epoch='e')]
            for i, tick in enumerate((0xfffffff0, 34, 84)):
                rows.append(dict(type='tdoa', s='one', ref=i, fol=i, u_ms=tick,
                                 dd_mm=None if i == 1 else 1000, status=1 if i == 1 else 4))
            rows.append(dict(type='ar_range', s='one', node=141, epoch='e', cycle=1,
                operation=0, initiator_node=132, responder_node=141, acquisition_uptime_ms=100,
                valid=True, range_mm=1000, stamp_clock_nodes=[132, 141, 141, 132, 132, 141],
                stamps=[f'{x:010X}' for x in [0xffffffff00, 0xfffffffe00, 99, 99,
                    (0xffffffff00+63897600) & ((1 << 40)-1),
                    (0xfffffffe00+127795200) & ((1 << 40)-1)]]))
            path.write_text('\n'.join(map(json.dumps, rows)))
            report = analyze([path])
            pair = next(v for k, v in report['freshness'].items() if 'tdoa' in k)
            self.assertEqual(pair['valid_interval_ms']['mean'], 100)
            self.assertEqual(pair['age_since_previous_valid_at_observed_sample_ms']['mean'], 75)
            self.assertEqual(report['twr_same_node_spans_ms']['initiator_p1tx_to_p3tx_ms']['mean'], 1)
            self.assertEqual(report['twr_same_node_spans_ms']['responder_p1rx_to_p3rx_ms']['mean'], 2)
