"""Production ESP command executor + canonical EKF; all sources synthetic/host-only."""
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
import sys
sys.path.insert(0, str(ROOT/'tools'))
import ekf_replay as replay


class EkfService(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.temp.name)/'ekf'
        replay.compile_service(cls.exe, sanitize=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_commands(self, commands):
        return replay.replay(self.exe, commands)

    def test_off_explicit_configuration_and_provenance(self):
        commands = ['EKF STATUS', 'EKF ON', replay.CONFIG, replay.INIT, 'EKF ON']
        commands += [f'EKF ODOM 1 2 {replay.T0} 0 32 {p} 0 0 0'
                     for p in ('UNKNOWN', 'COMMANDED', 'SIMULATED')]
        trace = self.run_commands(commands)
        self.assertIn('enabled=0', trace[0][1])
        self.assertIn('status=disabled', trace[1][1])
        self.assertIn('enabled=0', trace[3][1])
        self.assertIn('status=provenance', trace[-3][1])
        self.assertIn('status=provenance', trace[-2][1])
        self.assertIn('status=baseline', trace[-1][1])
        trace = self.run_commands([replay.CONFIG[:-1]+'0', replay.INIT, 'EKF ON', commands[-1]])
        self.assertIn('status=provenance', trace[-1][1])

    def test_invalid_numbers_are_atomic_and_bounded(self):
        raw = [replay.CONFIG, replay.INIT, 'EKF ON', 'EKF STATE']
        for time in ('-1', str(1 << 64), 'NaN', '1e9'):
            raw += [f'EKF ODOM 1 2 {time} 0 32 MEASURED 0 0 0', 'EKF STATE']
        trace = self.run_commands(raw)
        initial = trace[3][1]
        for i in range(4, len(trace), 2):
            self.assertTrue(trace[i][1].startswith('EKF ERR'))
            self.assertEqual(trace[i+1][1], initial)
        self.assertTrue(all(len(response) < 318 for _, response in trace))

    def test_domain_time_gap_and_counter_width(self):
        baseline = f'EKF ODOM 1 2 {replay.T0} 0 32 MEASURED 4294967290 4294967290 0'
        raw = [replay.CONFIG, replay.INIT, 'EKF ON', baseline]
        raw += [f'EKF ODOM {clock} {epoch} {time} 1 {bits} MEASURED 20 20 {uncertainty}'
                for clock, epoch, time, bits, uncertainty in (
                    (2, 2, replay.T0+50000, 32, 0), (1, 3, replay.T0+50000, 32, 0),
                    (1, 2, replay.T0+50000, 16, 0), (1, 2, replay.T0+100001, 32, 0),
                    (1, 2, replay.T0+50000, 32, 1001))]
        raw.append(f'EKF ODOM 1 2 {replay.T0+50000} 1 32 MEASURED 20 20 0')
        statuses = [replay.fields(r).get('status') for _, r in self.run_commands(raw)]
        self.assertEqual(statuses[-6:], ['domain', 'domain', 'domain', 'gap', 'invalid', 'ok'])

    def test_correlation_no_rate_and_incomplete_covariance(self):
        data = replay.scenario(steps=5)
        raw = replay.commands(data)[:3]
        # Cover batch timestamp with genuine simulated counter endpoints.
        for s in data['samples']:
            raw.append(f'EKF ODOM 1 2 {s["time_us"]} {s["step"]} 32 SIMULATED {s["counters"][0]} {s["counters"][1]} 0')
        sample = dict(data['samples'][-1], sigma_m=data['sigma_m'])
        batch = replay.batch_commands(sample)
        unknown = batch.copy();unknown[0] = unknown[0][:-1]+'0'
        raw += unknown
        trace = self.run_commands(raw)
        self.assertIn('status=correlation', trace[-2][1])
        invalid = batch.copy()
        invalid[5] = invalid[5][:-1]+'0' # first TDoA rateValid
        trace = self.run_commands(raw+invalid)
        self.assertIn('status=invalid', trace[-2][1])
        # Missing covariance row cannot commit; repair then commit once.
        raw += batch[:-3]+['EKF COMMIT']
        trace = self.run_commands(raw)
        self.assertTrue(trace[-1][1].startswith('EKF ERR'))
        raw += batch[-3:]
        trace = self.run_commands(raw)
        self.assertIn('status=ok', trace[-2][1])
        trace = self.run_commands(raw+batch)
        self.assertIn('status=duplicate', trace[-2][1])

    def test_pose_and_radio_covariance_validation(self):
        data = replay.scenario(steps=5)
        raw = replay.commands(data)
        batch_start = next(i for i, s in enumerate(raw) if s.startswith('EKF BEGIN'))
        for alteration in ('crc', 'noise'):
            commands = raw.copy()
            if alteration == 'crc':
                commands[batch_start+1] = commands[batch_start+1][:-1]+('1' if commands[batch_start+1][-1] != '1' else '0')
                self.assertTrue(any(r.startswith('EKF ERR') for _, r in self.run_commands(commands)))
            else:
                row = next(i for i, c in enumerate(commands) if c.startswith('EKF COVROW 0'))
                commands[row] = 'EKF COVROW 0 -1 0 0 0'
                self.assertTrue(any('status=covariance' in r for _, r in self.run_commands(commands)))

    def test_trajectories_loss_nlos_bias_psd_and_delayed_replay(self):
        for kind in ('straight', 'turns', 'spin', 'loss', 'bias', 'nlos'):
            with self.subTest(kind=kind):
                data = replay.scenario(kind=kind, steps=90)
                ordered = self.run_commands(replay.commands(data))
                delayed = self.run_commands(replay.commands(data, delayed=True))
                states, cov = replay.validate(ordered)
                late, _ = replay.validate(delayed)
                # Compare time/mean/full covariance; revisions count input/rebuilds.
                for key in ('t_us', 'x', 'y', 'theta', 'updates', 'gated'):
                    self.assertEqual(states[-1][key], late[-1][key])
                self.assertEqual([r for c, r in ordered if c == 'EKF COV'][-1],
                                 [r for c, r in delayed if c == 'EKF COV'][-1])
                m = replay.metrics(data, states)
                self.assertLess(m['position_rmse_m'], .12)
                self.assertGreater(m['updates'], 5)
                if kind == 'nlos':
                    self.assertEqual(m['gated'], 6)
                if kind == 'loss':
                    # Wheel uncertainty grows during missing observations, recovers.
                    self.assertGreater(cov[55][0]+cov[55][1], cov[20][0]+cov[20][1])
                    self.assertLess(cov[-1][0]+cov[-1][1], cov[55][0]+cov[55][1])

    def test_seed_noise_stationary_alrs_and_determinism(self):
        for seed in (1, 2, 3):
            data = replay.scenario(seed=seed, steps=60, sigma=.06, moving=False)
            self.assertEqual(data, replay.scenario(seed=seed, steps=60, sigma=.06, moving=False))
            raw = replay.commands(data)
            a = self.run_commands(raw)
            self.assertEqual(a, self.run_commands(raw))
            states, _ = replay.validate(a)
            self.assertLess(replay.metrics(data, states)['final_error_m'], .12)

    def test_off_frees_prior_and_reinitialization_required(self):
        trace = self.run_commands([replay.CONFIG, replay.INIT, 'EKF ON', 'EKF OFF', 'EKF ON', 'EKF STATUS'])
        self.assertIn('status=disabled', trace[-2][1])
        self.assertIn('initialized=0', trace[-1][1])


class EkfLogging(unittest.TestCase):
    def test_compact_preserves_esp_source_and_worker_drop_changes(self):
        import io
        import json
        from uwb_logging import CompactLog
        output = io.StringIO()
        log = CompactLog(output, 'host-test')
        lines = ['EKF1 status=ok x=1 y=2 theta=0', 'EKF ERR queue full',
                 'UWBSTAT version=1 dropped=0 corrupt=0 uart_errors=0 ekf_dropped=0',
                 'UWBSTAT version=1 dropped=0 corrupt=0 uart_errors=0 ekf_dropped=2']
        for line in lines:
            log.accept(dict(kind='bridge', host_received_utc='2026-10-27T00:00:00Z', line=line))
        rows = [json.loads(line) for line in output.getvalue().splitlines()]
        ekf = [r for r in rows if r['type'] == 'ekf']
        self.assertEqual([r['message'] for r in ekf], lines[:2])
        self.assertTrue(all(r['source'] == 'esp32_ekf' for r in ekf))
        self.assertEqual(rows[-1]['ekf_dropped'], 2)
