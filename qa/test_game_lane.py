"""No-game gameslot adapter checks: mocked backend, no child processes or kills."""
import importlib.util
import os
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("game_lane", Path(__file__).parent / "game_lane.py")
lane = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lane)


class Backend:
    def __init__(self):
        self.active = False
        self.foreign = False
        self.busy = False
        self.kills = 0
        self.calls = []
        self.grant = [0, 1]
        self.identity = {"name": "native-launcher.exe", "created": 123}
        self.registered = []

    def max_slots(self):
        return 2

    def kill_registered(self, records):
        self.kills += 1

    def try_acquire(self, owner, ttl, want, ticket):
        self.calls.append((owner, ttl, want, ticket))
        if self.foreign:
            self.kill_registered([{"pid": 9999}])
        if self.busy:
            return False, [], "another owner"
        self.active = True
        return True, self.grant, ""

    def release(self, owner):
        count = 2 if self.active else 0
        self.active = False
        return count

    def process_info(self, pid):
        return self.identity

    def register_pid(self, owner, pid):
        self.registered.append(pid)
        return 2


class Checks(unittest.TestCase):
    def setUp(self):
        self.backend = Backend()

    def take(self, budget=300):
        return lane.GameLane(budget, backend=self.backend).acquire()

    def test_exclusive_bounded_lease(self):
        lease = self.take(1000)
        self.assertEqual(lease.ttl, 1120)
        self.assertIsNone(self.backend.calls[0][2])
        self.assertEqual(lease.slots, [0, 1])
        lease.release()
        self.assertFalse(self.backend.active)

    def test_bad_budgets(self):
        for budget in (0, -1, float("inf"), float("nan")):
            with self.assertRaises(lane.LaneError):
                lane.GameLane(budget, backend=self.backend)
        self.assertEqual(self.backend.calls, [])

    def test_live_foreign_is_busy(self):
        self.backend.busy = True
        with self.assertRaisesRegex(lane.LaneError, "busy"):
            self.take()
        self.assertEqual(len(self.backend.calls), 1)
        self.assertEqual(self.backend.kills, 0)

    def test_expired_foreign_is_not_killed(self):
        self.backend.foreign = True
        with self.assertRaisesRegex(lane.LaneError, "no process was killed"):
            self.take()
        self.assertEqual(self.backend.kills, 0)
        self.assertFalse(self.backend.active)

    def test_partial_exclusive_grant_is_rejected(self):
        self.backend.grant = [0]
        with self.assertRaises(lane.LaneError):
            self.take()
        self.assertFalse(self.backend.active)

    def test_missing_source_fails_closed(self):
        with patch.dict(os.environ, {"COOP_ANYTHING_ROOT": "missing-hub"}), \
                patch.object(Path, "is_file", return_value=False):
            with self.assertRaisesRegex(lane.LaneError, "COOP_ANYTHING_ROOT"):
                lane.GameLane(300).acquire()

    def test_retained_registration_then_cleanup(self):
        lease = self.take()
        state = {"code": None}
        process = SimpleNamespace(pid=17, poll=lambda: state["code"])
        with patch.object(lane, "_retained_identity", return_value=dict(self.backend.identity)):
            lease.register_launcher(process)
        self.assertEqual(self.backend.registered, [17])
        with self.assertRaisesRegex(lane.LaneError, "retained"):
            lease.release()
        self.assertTrue(self.backend.active)
        state["code"] = 0
        lease.release()
        self.assertFalse(self.backend.active)

    def test_unverified_pid_never_registered(self):
        lease = self.take()
        process = SimpleNamespace(pid=9999, poll=lambda: 0)
        with patch.object(lane, "_retained_identity", return_value={"name": "foreign.exe", "created": 999}):
            with self.assertRaisesRegex(lane.LaneError, "identity"):
                lease.register_launcher(process)
        self.assertEqual(self.backend.registered, [])
        lease.release()

    def test_children_must_be_stopped(self):
        lease = self.take()
        with self.assertRaises(lane.LaneError):
            lease.release(children_stopped=False)
        self.assertTrue(self.backend.active)
        lease.release()

    def test_registration_requires_lease(self):
        with self.assertRaises(lane.LaneError):
            lane.GameLane(300, backend=self.backend).register_launcher(SimpleNamespace(pid=17))
        self.assertEqual(self.backend.registered, [])


class MutationTwins(unittest.TestCase):
    """The same safety oracles must reject deliberately broken adapters."""
    def caught(self, method, replacement, check_name):
        check = Checks(check_name)
        check.setUp()
        with patch.object(lane.GameLane, method, replacement):
            with self.assertRaises(AssertionError):
                getattr(check, check_name)()

    def test_early_release_twin(self):
        def broken(self, **_kwargs):
            self.backend.release(self.owner)
            self.held = False
        self.caught("release", broken, "test_retained_registration_then_cleanup")

    def test_foreign_reap_twin(self):
        def broken(self):
            ok, slots, _ = self.backend.try_acquire(self.owner, self.ttl, None, 0)
            self.held, self.slots = ok, slots
            return self
        self.caught("acquire", broken, "test_expired_foreign_is_not_killed")

    def test_short_ttl_twin(self):
        original = lane.GameLane.acquire
        def broken(self):
            self.ttl = 1
            return original(self)
        self.caught("acquire", broken, "test_exclusive_bounded_lease")


if __name__ == "__main__":
    unittest.main(verbosity=2)
