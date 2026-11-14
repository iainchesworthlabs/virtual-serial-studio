# SPDX-License-Identifier: GPL-3.0-or-later
"""The client against the real programs: `vss_agent` and `vss_gate` are
started as child processes under an instance name of their own, so the tests
never meet a person's agent or gateway.

The programs come from the environment: VSS_AGENT_EXE and VSS_GATE_EXE (the
whole module is skipped without them) and VSS_CLI_EXE (the token tests are
skipped without it).  Nothing here sleeps to wait for a program: a test waits
for the line a program prints, and then retries `Client.connect` until a
deadline.
"""

import os
import secrets
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import unittest
import xml.etree.ElementTree as ElementTree
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir))

from vss_control import Client, ControlError, EngineError, TransportError, _peer  # noqa: E402

AGENT_EXE = os.environ.get("VSS_AGENT_EXE")
GATE_EXE = os.environ.get("VSS_GATE_EXE")
CLI_EXE = os.environ.get("VSS_CLI_EXE")

if not AGENT_EXE or not GATE_EXE:
    raise unittest.SkipTest(
        "VSS_AGENT_EXE and VSS_GATE_EXE are not both set; these tests start the real programs"
    )

AGENT_EXE = os.path.abspath(AGENT_EXE)
GATE_EXE = os.path.abspath(GATE_EXE)
if CLI_EXE:
    CLI_EXE = os.path.abspath(CLI_EXE)

# The longest any wait for a program may take.  A passing run takes a second.
LONG = 30.0

_counter = [0]
_counter_lock = threading.Lock()


def unique_name(prefix):
    # Short: a Unix socket path is limited (104 bytes on macOS) and the
    # instance name is part of it.
    with _counter_lock:
        _counter[0] += 1
        number = _counter[0]
    return "%s%s%d" % (prefix, secrets.token_hex(3), number)


class Program:
    """A child process whose output is read by a thread, so that a program
    that says more than a pipe holds is never stuck, and whose readiness is a
    line of its output."""

    def __init__(self, arguments, marker):
        self.arguments = arguments
        self.marker = marker
        self.lines = []
        self.ready = threading.Event()
        self.process = subprocess.Popen(
            arguments,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        self.reader = threading.Thread(target=self._pump, daemon=True)
        self.reader.start()

    def _pump(self):
        for raw in iter(self.process.stdout.readline, b""):
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            self.lines.append(line)
            if self.marker in line:
                self.ready.set()
        self.ready.set()

    def wait_ready(self):
        if not self.ready.wait(LONG) or not any(self.marker in line for line in self.lines):
            self.stop()
            raise AssertionError(
                "%s did not say %r; it said: %s"
                % (os.path.basename(self.arguments[0]), self.marker, self.lines)
            )

    def stop(self):
        process = self.process
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        self.reader.join(5)
        process.stdout.close()


class Rig:
    """An agent and a gateway that serves it, and a scratch directory."""

    def __init__(self, local_role=None):
        self.instance = unique_name("py")
        self.directory = tempfile.mkdtemp(prefix="vsspy")
        self.programs = []
        try:
            agent = Program([AGENT_EXE, "--instance=" + self.instance], "listening on")
            self.programs.append(agent)
            agent.wait_ready()
            arguments = [
                GATE_EXE,
                "--instance=" + self.instance,
                "--agent-instance=" + self.instance,
                "--audit-dir=" + os.path.join(self.directory, "audit"),
                "--token-file=" + os.path.join(self.directory, "tokens.json"),
            ]
            if local_role:
                arguments.append("--local-role=" + local_role)
            gate = Program(arguments, "administration on")
            self.programs.append(gate)
            gate.wait_ready()
        except BaseException:
            self.stop()
            raise

    def stop(self):
        for program in reversed(self.programs):
            program.stop()
        self.programs = []
        shutil.rmtree(self.directory, ignore_errors=True)

    def connect(self, **options):
        """Connects, retrying while the gateway is not listening yet."""
        options.setdefault("name", "vss_control tests")
        options.setdefault("timeout", 10.0)
        deadline = time.monotonic() + LONG
        while True:
            try:
                return Client.connect(instance=self.instance, **options)
            except TransportError as error:
                if error.code not in ("not_running", "busy") or time.monotonic() > deadline:
                    raise
                time.sleep(0.05)


class RigCase(unittest.TestCase):
    local_role = None
    rig = None

    @classmethod
    def setUpClass(cls):
        cls.rig = Rig(cls.local_role)
        cls.addClassCleanup(cls.rig.stop)

    def connect(self, **options):
        client = self.rig.connect(**options)
        self.addCleanup(client.close)
        return client

    def make_port(self, client, label="a"):
        name = unique_name(label)
        port = client.create_port(name)
        self.addCleanup(self.remove_quietly, port)
        return port

    def remove_quietly(self, port):
        try:
            with self.rig.connect() as client:
                client.remove_port(port)
        except ControlError:
            pass

    def linked_pair(self, client):
        first = self.make_port(client, "a")
        second = self.make_port(client, "b")
        link = client.connect_ports(first, second, one_way=True)
        self.assertTrue(link.startswith("l_"), link)
        return first, second


class ControlRoleTests(RigCase):
    """A gateway whose local user may change ports and send bytes."""

    local_role = "control"

    def test_hello(self):
        client = self.connect()
        hello = client.hello
        self.assertEqual(hello["api"], 1)
        self.assertTrue(hello["version"])
        self.assertTrue(hello["principal"].startswith("local:"), hello["principal"])
        for scope in ("monitor", "monitor.data", "control.ports", "control.data"):
            self.assertIn(scope, hello["scopes"])
        for feature in ("ports.read", "expect.bytes", "runs"):
            self.assertIn(feature, hello["features"])

    def test_status(self):
        client = self.connect()
        status = client.status()
        self.assertTrue(status["agent"]["running"])
        self.assertGreaterEqual(status["sessions"], 1)
        self.assertIsInstance(status["uptime_ms"], int)

    def test_list_ports_shows_a_new_port_and_a_link(self):
        client = self.connect()
        before = client.list_ports()
        self.assertEqual(set(before), {"revision", "ports", "links"})
        first, second = self.linked_pair(client)
        after = client.list_ports()
        self.assertGreater(after["revision"], before["revision"])
        ids = {port["id"]: port for port in after["ports"]}
        self.assertIn(first, ids)
        self.assertIn(second, ids)
        self.assertTrue(first.startswith("p_"), first)
        self.assertTrue(ids[first]["open"])
        link = [one for one in after["links"] if one["sources"] == [first]]
        self.assertEqual(len(link), 1)
        self.assertEqual(link[0]["targets"], [second])

    def test_send_and_expect_with_a_send(self):
        client = self.connect()
        first, second = self.linked_pair(client)
        # Six bytes go by before the pattern.
        found = client.expect_bytes(
            port=second,
            pattern=b"OK\r\n",
            timeout_ms=5000,
            send_port=first,
            send=b"noise OK\r\n",
        )
        self.assertEqual(found["offset"], 6)
        self.assertGreater(found["mono_ns"], 0)
        self.assertEqual(found["events_lost"], 0)
        # Plain send, and the counters see it.
        client.send(first, b"\x00\xff\r\n")
        ports = {port["id"]: port for port in client.list_ports()["ports"]}
        self.assertGreaterEqual(ports[second]["bytes_rx"], 14)

    def test_expect_that_does_not_come_is_a_timeout_and_the_connection_lives(self):
        client = self.connect()
        first, second = self.linked_pair(client)
        started = time.monotonic()
        with self.assertRaises(ControlError) as caught:
            client.expect_bytes(
                port=second, pattern=b"NEVER", timeout_ms=200, send_port=first, send=b"something"
            )
        self.assertEqual(caught.exception.code, "timeout")
        self.assertNotIsInstance(caught.exception, EngineError)
        self.assertTrue(caught.exception.retryable)
        self.assertLess(time.monotonic() - started, 10)
        # The same connection goes on.
        found = client.expect_bytes(
            port=second, pattern=b"ok", timeout_ms=5000, send_port=first, send=b"all ok"
        )
        self.assertEqual(found["offset"], 4)
        self.assertTrue(client.status()["agent"]["running"])

    def test_an_engine_error(self):
        client = self.connect()
        port = client.create_port(unique_name("gone"))
        client.remove_port(port)
        with self.assertRaises(EngineError) as caught:
            client.remove_port(port)
        self.assertEqual(caught.exception.code, "engine.not_found")
        self.assertIsInstance(caught.exception, ControlError)
        self.assertFalse(caught.exception.retryable)
        # The gateway's own refusals are not the engine's.
        with self.assertRaises(ControlError) as caught:
            client.remove_port("no such port")
        self.assertEqual(caught.exception.code, "invalid_argument")
        self.assertNotIsInstance(caught.exception, EngineError)
        self.assertTrue(client.status()["agent"]["running"])

    def test_next_events_without_a_stream_times_out_and_the_connection_lives(self):
        client = self.connect()
        started = time.monotonic()
        self.assertIsNone(client.next_events(timeout=0.3))
        elapsed = time.monotonic() - started
        self.assertGreaterEqual(elapsed, 0.25)
        self.assertLess(elapsed, 10)
        self.assertTrue(client.status()["agent"]["running"])

    def test_the_traffic_stream_carries_the_bytes(self):
        client = self.connect()
        first, second = self.linked_pair(client)
        started = client.subscribe_traffic(interval_ms=20, ports=[second])
        self.assertGreaterEqual(started["interval_ms"], 10)
        self.assertGreaterEqual(started["next_seq"], 1)
        sent = b"\x00hello traffic\xff\r\n"
        client.send(first, sent)

        received = b""
        last_seq = 0
        deadline = time.monotonic() + LONG
        while sent not in received and time.monotonic() < deadline:
            batch = client.next_events(timeout=1.0)
            if batch is None:
                continue
            self.assertGreater(batch["seq"], last_seq)
            last_seq = batch["seq"]
            for event in batch["events"]:
                # The stream names one port, so nothing is about another.
                self.assertEqual(event["port"], second)
                if event["kind"] == "data" and event["direction"] == "rx":
                    self.assertEqual(len(event["data"]), event["size"])
                    received += event["data"]
        self.assertIn(sent, received)
        client.unsubscribe_traffic()
        self.assertTrue(client.status()["agent"]["running"])

    def test_the_event_stream_says_how_many_bytes_and_not_which(self):
        client = self.connect()
        first, second = self.linked_pair(client)
        client.subscribe_events(interval_ms=20, ports=[second])
        client.send(first, b"12345")
        deadline = time.monotonic() + LONG
        found = None
        while found is None and time.monotonic() < deadline:
            batch = client.next_events(timeout=1.0)
            for event in (batch or {"events": []})["events"]:
                if event["kind"] == "data" and event["direction"] == "rx":
                    found = event
        self.assertIsNotNone(found)
        self.assertEqual(found["size"], 5)
        self.assertEqual(found["data"], b"")
        client.unsubscribe_events()

    def test_a_run_with_a_pass_and_a_fail_makes_a_junit_report(self):
        client = self.connect()
        first, second = self.linked_pair(client)
        self.addCleanup(self.end_run_quietly)
        run = client.begin_run("python adapter run")
        self.assertTrue(run["open"])
        self.assertTrue(run["run"].startswith("r_"), run["run"])
        with self.assertRaises(ControlError) as caught:
            client.begin_run("a second one")
        self.assertEqual(caught.exception.code, "conflict")

        client.expect_bytes(
            port=second,
            pattern=b"PONG",
            timeout_ms=5000,
            send_port=first,
            send=b"PONG",
            label="pong comes back",
        )
        with self.assertRaises(ControlError) as caught:
            client.expect_bytes(
                port=second,
                pattern=b"NEVER",
                timeout_ms=200,
                send_port=first,
                send=b"x",
                label="never comes",
            )
        self.assertEqual(caught.exception.code, "timeout")
        client.record_in_run("own check", "passed", duration_ms=3)

        ended = client.end_run()
        self.assertFalse(ended["open"])
        self.assertEqual((ended["tests"], ended["failures"]), (3, 1))

        for xml in (client.report_junit(), client.report_junit(run=run["run"])):
            self.assertIn("pong comes back", xml)
            self.assertIn("never comes", xml)
            self.assertIn("own check", xml)
            root = ElementTree.fromstring(xml)
            cases = {case.get("name"): case for case in root.iter("testcase")}
            self.assertEqual(set(cases), {"pong comes back", "never comes", "own check"})
            self.assertIsNone(cases["pong comes back"].find("failure"))
            self.assertIsNotNone(cases["never comes"].find("failure"))
        with self.assertRaises(ControlError) as caught:
            client.end_run()
        self.assertEqual(caught.exception.code, "not_found")

    def test_a_run_begun_on_one_connection_is_filled_and_ended_on_another(self):
        self.addCleanup(self.end_run_quietly)
        with self.rig.connect() as beginner:
            run = beginner.begin_run("across connections")
        # The first connection is closed; the run belongs to the user.
        filler = self.connect()
        filler.record_in_run("from the second", "failed", message="late", duration_ms=2)
        with self.assertRaises(ControlError) as caught:
            filler.begin_run("another")
        self.assertEqual(caught.exception.code, "conflict")

        finisher = self.connect()
        ended = finisher.end_run()
        self.assertEqual(ended["run"], run["run"])
        self.assertFalse(ended["open"])
        self.assertEqual((ended["tests"], ended["failures"]), (1, 1))
        root = ElementTree.fromstring(finisher.report_junit(run["run"]))
        self.assertEqual(root.find("testsuite").get("name"), "across connections")
        cases = {case.get("name"): case for case in root.iter("testcase")}
        self.assertEqual(set(cases), {"from the second"})
        self.assertEqual(cases["from the second"].find("failure").get("message"), "late")

    def end_run_quietly(self):
        try:
            with self.rig.connect() as client:
                client.end_run()
        except ControlError:
            pass

    def test_a_wrong_token_is_unauthenticated_and_is_not_in_the_error(self):
        token = "vsst1_" + "a" * 13 + "_" + "a" * 52
        with self.assertRaises(ControlError) as caught:
            self.rig.connect(token=token)
        self.assertEqual(caught.exception.code, "unauthenticated")
        self.assertNotIsInstance(caught.exception, TransportError)
        shown = [
            str(caught.exception),
            repr(caught.exception),
            caught.exception.message,
            repr(caught.exception.args),
            "".join(
                traceback.format_exception(
                    type(caught.exception), caught.exception, caught.exception.__traceback__
                )
            ),
        ]
        for text in shown:
            self.assertNotIn(token, text)
            self.assertNotIn("a" * 52, text)
        # The gateway is still there for the local user.
        self.assertTrue(self.connect().status()["agent"]["running"])

    def test_a_server_that_runs_as_another_user_is_refused_before_anything_is_sent(self):
        if os.name == "nt":
            refuse = mock.patch.object(_peer, "_same_user", return_value=False)
        else:
            refuse = mock.patch.object(_peer, "unix_peer_uid", return_value=os.geteuid() + 1)
        with refuse:
            # If the client got as far as saying hello, this would fail.
            with mock.patch.object(Client, "_greet", side_effect=AssertionError("hello sent")):
                with self.assertRaises(TransportError) as caught:
                    Client.connect(instance=self.rig.instance)
        self.assertEqual(caught.exception.code, "not_same_user")
        # The connection that was opened is closed again, and the gateway is
        # still there for the right user.
        self.assertTrue(self.connect().status()["agent"]["running"])

    def test_no_gateway_is_not_running(self):
        with self.assertRaises(TransportError) as caught:
            Client.connect(instance=unique_name("none"), timeout=2.0)
        self.assertEqual(caught.exception.code, "not_running")

    def test_a_closed_client_says_so(self):
        client = self.rig.connect()
        client.close()
        with self.assertRaises(TransportError) as caught:
            client.status()
        self.assertEqual(caught.exception.code, "connection_closed")


class DefaultRoleTests(RigCase):
    """A gateway with the default local role, which may only look."""

    def test_hello_holds_no_scope_to_change_anything(self):
        client = self.connect()
        self.assertIn("monitor", client.hello["scopes"])
        self.assertNotIn("control.ports", client.hello["scopes"])
        self.assertNotIn("control.data", client.hello["scopes"])

    def test_create_port_is_forbidden_and_the_connection_lives(self):
        client = self.connect()
        with self.assertRaises(ControlError) as caught:
            client.create_port(unique_name("denied"))
        self.assertEqual(caught.exception.code, "forbidden")
        self.assertNotIsInstance(caught.exception, EngineError)
        self.assertFalse(caught.exception.retryable)
        self.assertEqual(client.list_ports()["ports"], [])

    def test_the_other_changes_are_forbidden_too(self):
        client = self.connect()
        for call in (
            lambda: client.remove_port("p_1"),
            lambda: client.connect_ports("p_1", "p_2"),
            lambda: client.send("p_1", b"x"),
            lambda: client.expect_bytes("p_1", b"x", timeout_ms=100),
            lambda: client.subscribe_traffic(),
        ):
            with self.assertRaises(ControlError) as caught:
                call()
            self.assertEqual(caught.exception.code, "forbidden")
        self.assertTrue(client.status()["agent"]["running"])


@unittest.skipUnless(CLI_EXE, "VSS_CLI_EXE is not set; a token is made with vss_cli")
class TokenTests(RigCase):
    """A token made with the real `vss_cli control token create`."""

    local_role = None

    def make_token_file(self, role="control"):
        path = os.path.join(self.rig.directory, "t%s.token" % secrets.token_hex(3))
        done = subprocess.run(
            [
                CLI_EXE,
                "control",
                "token",
                "create",
                "--label=python tests",
                "--role=" + role,
                "--out=" + path,
                "--instance=" + self.rig.instance,
            ],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=LONG,
        )
        output = done.stdout.decode("utf-8", "replace")
        self.assertEqual(done.returncode, 0, output)
        self.assertNotIn("vsst1_", output)
        with open(path, "rb") as stream:
            text = stream.read().decode("ascii")
        self.assertTrue(text.endswith("\n"), "the token file ends with a line ending")
        return path, text.rstrip("\n")

    def test_a_token_file_connects_as_the_token(self):
        path, token = self.make_token_file("control")
        client = self.connect(token_file=path)
        self.assertTrue(client.hello["principal"].startswith("token:"), client.hello["principal"])
        self.assertIn("control.ports", client.hello["scopes"])
        # The token may do what its scopes say, and the client does not hold it.
        port = self.make_port(client)
        self.assertTrue(port.startswith("p_"))
        self.assertNotIn(token, repr(vars(client)))
        self.assertNotIn(token, repr(client))

    def test_a_token_as_text_connects_as_the_token(self):
        _path, token = self.make_token_file("monitor")
        client = self.connect(token=token)
        self.assertTrue(client.hello["principal"].startswith("token:"))
        self.assertNotIn("control.ports", client.hello["scopes"])
        with self.assertRaises(ControlError) as caught:
            client.create_port(unique_name("denied"))
        self.assertEqual(caught.exception.code, "forbidden")

    def test_a_token_that_is_one_character_off_is_unauthenticated(self):
        _path, token = self.make_token_file("control")
        wrong = token[:-1] + ("b" if token[-1] == "a" else "a")
        self.assertNotEqual(wrong, token)
        with self.assertRaises(ControlError) as caught:
            self.rig.connect(token=wrong)
        self.assertEqual(caught.exception.code, "unauthenticated")
        for text in (str(caught.exception), repr(caught.exception), caught.exception.message):
            self.assertNotIn(wrong, text)
            self.assertNotIn(token, text)
            self.assertNotIn(token[-52:], text)
        # The real one still works.
        self.assertTrue(self.connect(token=token).hello["principal"].startswith("token:"))


if __name__ == "__main__":
    unittest.main()
