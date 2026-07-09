from __future__ import annotations

import unittest
from unittest import mock

from picotrace.app import cli


class CliTests(unittest.TestCase):
    def test_trace_defaults_to_foreground_off_windows(self) -> None:
        with mock.patch.object(cli.sys, "platform", "linux"), mock.patch(
            "picotrace.app.cli._stream_channel_with_hooks", return_value=0
        ) as stream_channel_mock, mock.patch("picotrace.app.cli._spawn_monitor_window") as spawn_monitor_window_mock:
            exit_code = cli.main(["trace", "--channel", "4"])

        self.assertEqual(exit_code, 0)
        stream_channel_mock.assert_called_once_with(4)
        spawn_monitor_window_mock.assert_not_called()

    def test_configure_and_start_monitor_terminates_new_monitor_on_configure_failure(self) -> None:
        process = mock.Mock()

        with mock.patch.object(cli.sys, "platform", "win32"), mock.patch(
            "picotrace.app.cli._create_monitor_start_gate", return_value=mock.Mock()
        ), mock.patch(
            "picotrace.app.cli._create_monitor_ready_gate", return_value=mock.Mock()
        ), mock.patch(
            "picotrace.app.cli._spawn_monitor_window", return_value=process
        ), mock.patch("picotrace.app.cli._terminate_monitor_process") as terminate_monitor_process_mock, mock.patch(
            "picotrace.app.cli._wait_for_monitor_ready"
        ):
            with self.assertRaisesRegex(RuntimeError, "boom"):
                cli._configure_and_start_monitor(
                    2,
                    label="i2c channel 2",
                    foreground=False,
                    configure=mock.Mock(side_effect=RuntimeError("boom")),
                )

        terminate_monitor_process_mock.assert_called_once_with(process)

    def test_monitor_manager_clears_stale_monitor_if_reconfigure_fails(self) -> None:
        manager = cli._MonitorManager()
        old_process = mock.Mock()
        new_process = mock.Mock()
        manager._processes[3] = cli._MonitorSession(process=old_process, label="spi logical channel 3", channel=3)

        with mock.patch.object(cli.sys, "platform", "win32"), mock.patch(
            "picotrace.app.cli._create_monitor_start_gate", return_value=mock.Mock()
        ), mock.patch(
            "picotrace.app.cli._create_monitor_ready_gate", return_value=mock.Mock()
        ), mock.patch(
            "picotrace.app.cli._spawn_monitor_window", return_value=new_process
        ), mock.patch("picotrace.app.cli._terminate_monitor_process") as terminate_monitor_process_mock, mock.patch(
            "picotrace.app.cli._wait_for_monitor_ready"
        ):
            with self.assertRaisesRegex(RuntimeError, "boom"):
                manager.start_monitor(3, label="spi logical channel 3", configure=mock.Mock(side_effect=RuntimeError("boom")))

        self.assertNotIn(3, manager._processes)
        self.assertEqual(terminate_monitor_process_mock.call_args_list, [mock.call(old_process), mock.call(new_process)])

    def test_monitor_manager_stops_existing_monitor_before_releasing_new_monitor(self) -> None:
        manager = cli._MonitorManager()
        old_process = mock.Mock()
        new_process = mock.Mock()
        start_gate = mock.Mock()
        events: list[str] = []
        stop_old = mock.Mock(side_effect=lambda: events.append("stop_old_command"))
        manager._processes[7] = cli._MonitorSession(
            process=old_process,
            label="trace channel 7",
            channel=7,
            stop=stop_old,
        )

        def terminate(process) -> None:
            events.append("stop_old" if process is old_process else "stop_new")

        def configure() -> None:
            events.append("configure")

        def release(_gate) -> None:
            events.append("release_gate")

        with mock.patch.object(cli.sys, "platform", "win32"), mock.patch(
            "picotrace.app.cli._create_monitor_start_gate", return_value=start_gate
        ), mock.patch(
            "picotrace.app.cli._create_monitor_ready_gate", return_value=mock.Mock()
        ), mock.patch("picotrace.app.cli._spawn_monitor_window", return_value=new_process), mock.patch(
            "picotrace.app.cli._terminate_monitor_process", side_effect=terminate
        ), mock.patch("picotrace.app.cli._release_monitor_start_gate", side_effect=release), mock.patch(
            "picotrace.app.cli._wait_for_monitor_ready", side_effect=lambda _gate, _process: events.append("wait_ready")):
            manager.start_monitor(7, label="trace channel 7", configure=configure)

        self.assertEqual(events, ["stop_old", "stop_old_command", "configure", "release_gate", "wait_ready", "release_gate"])
        self.assertIs(manager._processes[7].process, new_process)

    def test_start_monitor_replaces_active_spi_session_via_bus_group_restart(self) -> None:
        manager = cli._MonitorManager()
        old_processes = {channel: mock.Mock(name=f"old_{channel}") for channel in (0, 1, 2)}
        new_process = mock.Mock(name="new_trace")
        spi_config = cli._SpiMonitorConfig(capture=cli.SpiCaptureMode.MOSI_MISO, spi_mode=0, timeout_us=100)
        for channel in (0, 1, 2):
            manager._processes[channel] = cli._MonitorSession(
                process=old_processes[channel],
                label=f"spi logical channel {channel}",
                channel=channel,
                stop=lambda channel=channel: None,
                stop_config=cli._MonitorStopConfig(kind="spi", channel=channel),
                spi_config=spi_config,
            )

        with mock.patch.object(cli.sys, "platform", "win32"), mock.patch(
            "picotrace.app.cli._terminate_monitor_process"
        ) as terminate_monitor_process_mock, mock.patch(
            "picotrace.app.cli._stop_spi_logical_channel"
        ) as stop_spi_mock, mock.patch("picotrace.app.cli._configure_spi_bus_sessions") as configure_spi_mock, mock.patch(
            "picotrace.app.cli._start_detached_monitor",
            side_effect=[new_process, old_processes[2]],
        ) as start_detached_monitor_mock:
            manager.start_monitor(1, label="trace channel 1")

        self.assertEqual(
            terminate_monitor_process_mock.call_args_list,
            [mock.call(old_processes[0]), mock.call(old_processes[2])],
        )
        stop_spi_mock.assert_called_once_with(1)
        restarted_sessions = configure_spi_mock.call_args.args[0]
        self.assertEqual([session.channel for session in restarted_sessions], [0, 2])
        self.assertEqual(start_detached_monitor_mock.call_count, 2)

    def test_stop_monitor_ignores_stop_callback_failure(self) -> None:
        manager = cli._MonitorManager()
        process = mock.Mock()
        manager._processes[2] = cli._MonitorSession(
            process=process,
            label="i2c channel 2",
            channel=2,
            stop=mock.Mock(side_effect=RuntimeError("stop failed")),
        )

        with mock.patch("picotrace.app.cli._terminate_monitor_process") as terminate_monitor_process_mock:
            stopped = manager.stop_monitor(2)

        self.assertTrue(stopped)
        terminate_monitor_process_mock.assert_called_once_with(process)

    def test_stop_all_ignores_i2c_stop_callback_failure(self) -> None:
        manager = cli._MonitorManager()
        manager._processes[2] = cli._MonitorSession(
            process=mock.Mock(),
            label="i2c channel 2",
            channel=2,
            stop=mock.Mock(side_effect=RuntimeError("stop failed")),
        )

        with mock.patch("picotrace.app.cli._terminate_monitor_process") as terminate_monitor_process_mock:
            manager.stop_all()

        terminate_monitor_process_mock.assert_called_once()
        self.assertEqual(manager._processes, {})

    def test_configure_and_start_monitor_disables_stream_when_foreground_open_fails(self) -> None:
        configure = mock.Mock()

        with mock.patch("picotrace.app.cli._stream_channel_with_hooks", side_effect=RuntimeError("bulk open failed")), mock.patch(
            "picotrace.app.cli._disable_stream_best_effort"
        ) as disable_stream_mock:
            with self.assertRaisesRegex(RuntimeError, "bulk open failed"):
                cli._configure_and_start_monitor(2, label="i2c channel 2", foreground=True, configure=configure)

        configure.assert_called_once_with()
        disable_stream_mock.assert_called_once_with()

    def test_start_detached_monitor_requires_ready_ack_before_success(self) -> None:
        process = mock.Mock()
        events: list[str] = []

        with mock.patch.object(cli.sys, "platform", "win32"), mock.patch(
            "picotrace.app.cli._create_monitor_start_gate", return_value=mock.Mock()
        ), mock.patch(
            "picotrace.app.cli._create_monitor_ready_gate", return_value=mock.Mock()
        ), mock.patch(
            "picotrace.app.cli._spawn_monitor_window", return_value=process
        ), mock.patch("picotrace.app.cli._release_monitor_start_gate", side_effect=lambda _gate: events.append("release_gate")), mock.patch(
            "picotrace.app.cli._wait_for_monitor_ready", side_effect=lambda _gate, _process: events.append("wait_ready")):
            started_process = cli._start_detached_monitor(
                5,
                label="trace channel 5",
                configure=None,
                old_process=None,
                stop=None,
                stop_config=None,
            )

        self.assertIs(started_process, process)
        self.assertEqual(events, ["release_gate", "wait_ready", "release_gate"])

    def test_start_detached_monitor_timeout_terminates_child(self) -> None:
        process = mock.Mock()
        stop = mock.Mock()

        with mock.patch.object(cli.sys, "platform", "win32"), mock.patch(
            "picotrace.app.cli._create_monitor_start_gate", return_value=mock.Mock()
        ), mock.patch(
            "picotrace.app.cli._create_monitor_ready_gate", return_value=mock.Mock()
        ), mock.patch(
            "picotrace.app.cli._spawn_monitor_window", return_value=process
        ), mock.patch("picotrace.app.cli._terminate_monitor_process") as terminate_monitor_process_mock, mock.patch(
            "picotrace.app.cli._wait_for_monitor_ready", side_effect=RuntimeError("ready timeout")
        ):
            with self.assertRaisesRegex(RuntimeError, "ready timeout"):
                cli._start_detached_monitor(
                    5,
                    label="trace channel 5",
                    configure=None,
                    old_process=None,
                    stop=stop,
                    stop_config=cli._MonitorStopConfig(kind="i2c", channel=5),
                )

        terminate_monitor_process_mock.assert_called_once_with(process)
        stop.assert_called_once_with()

    def test_stop_monitor_sends_stop_command_for_configured_session(self) -> None:
        manager = cli._MonitorManager()
        process = mock.Mock()
        stop = mock.Mock()
        manager._processes[2] = cli._MonitorSession(process=process, label="i2c channel 2", channel=2, stop=stop)

        with mock.patch("picotrace.app.cli._terminate_monitor_process") as terminate_monitor_process_mock:
            stopped = manager.stop_monitor(2)

        self.assertTrue(stopped)
        terminate_monitor_process_mock.assert_called_once_with(process)
        stop.assert_called_once_with()

    def test_run_monitor_sends_stop_command_on_exit(self) -> None:
        args = mock.Mock(channel=2, label="i2c channel 2", start_gate=None, ready_gate=None, stop_kind="i2c", stop_channel=2)

        with mock.patch("picotrace.app.cli._stream_channel_with_hooks", return_value=0), mock.patch(
            "picotrace.app.cli._stop_monitor_best_effort"
        ) as stop_monitor_mock:
            exit_code = cli._run_monitor(args)

        self.assertEqual(exit_code, 0)
        stop_monitor_mock.assert_called_once()

    def test_run_foreground_configured_monitor_stops_channel_after_stream_returns(self) -> None:
        configure = mock.Mock()
        stop = mock.Mock()

        def stream_side_effect(_channel: int, *, on_started=None):
            self.assertIsNotNone(on_started)
            if on_started is not None:
                on_started()
            return 0

        with mock.patch("picotrace.app.cli._stream_channel_with_hooks", side_effect=stream_side_effect):
            exit_code = cli._run_foreground_configured_monitor(2, configure, stop=stop)

        self.assertEqual(exit_code, 0)
        configure.assert_called_once_with()
        stop.assert_called_once_with()

    def test_run_foreground_configured_monitor_ignores_stop_failure_after_stream_returns(self) -> None:
        configure = mock.Mock()
        stop = mock.Mock(side_effect=RuntimeError("stop failed"))

        def stream_side_effect(_channel: int, *, on_started=None):
            self.assertIsNotNone(on_started)
            if on_started is not None:
                on_started()
            return 0

        with mock.patch("picotrace.app.cli._stream_channel_with_hooks", side_effect=stream_side_effect):
            exit_code = cli._run_foreground_configured_monitor(2, configure, stop=stop)

        self.assertEqual(exit_code, 0)
        configure.assert_called_once_with()
        stop.assert_called_once_with()

    def test_run_foreground_configured_monitor_ignores_stop_failure_after_startup_error(self) -> None:
        configure = mock.Mock()
        stop = mock.Mock(side_effect=RuntimeError("stop failed"))

        with mock.patch("picotrace.app.cli._stream_channel_with_hooks", side_effect=RuntimeError("bulk open failed")):
            with self.assertRaisesRegex(RuntimeError, "bulk open failed"):
                cli._run_foreground_configured_monitor(2, configure, stop=stop)

        configure.assert_called_once_with()
        stop.assert_called_once_with()

    def test_stop_spi_monitor_restarts_remaining_sibling_windows(self) -> None:
        manager = cli._MonitorManager()
        old_processes = {channel: mock.Mock(name=f"old_{channel}") for channel in (0, 1, 2)}
        new_processes = {channel: mock.Mock(name=f"new_{channel}") for channel in (1, 2)}
        spi_config = cli._SpiMonitorConfig(capture=cli.SpiCaptureMode.MOSI_MISO, spi_mode=0, timeout_us=100)
        for channel in (0, 1, 2):
            manager._processes[channel] = cli._MonitorSession(
                process=old_processes[channel],
                label=f"spi logical channel {channel}",
                channel=channel,
                stop=lambda channel=channel: None,
                stop_config=cli._MonitorStopConfig(kind="spi", channel=channel),
                spi_config=spi_config,
            )

        with mock.patch("picotrace.app.cli._terminate_monitor_process") as terminate_monitor_process_mock, mock.patch(
            "picotrace.app.cli._stop_spi_logical_channel"
        ) as stop_spi_mock, mock.patch("picotrace.app.cli._configure_spi_bus_sessions") as configure_spi_mock, mock.patch(
            "picotrace.app.cli._start_detached_monitor",
            side_effect=[new_processes[1], new_processes[2]],
        ) as start_detached_monitor_mock:
            stopped = manager.stop_monitor(0)

        self.assertTrue(stopped)
        self.assertEqual(
            terminate_monitor_process_mock.call_args_list,
            [mock.call(old_processes[1]), mock.call(old_processes[2]), mock.call(old_processes[0])],
        )
        stop_spi_mock.assert_called_once_with(0)
        restarted_sessions = configure_spi_mock.call_args.args[0]
        self.assertEqual([session.channel for session in restarted_sessions], [1, 2])
        self.assertEqual(start_detached_monitor_mock.call_count, 2)
        self.assertEqual(sorted(manager._processes), [1, 2])
        self.assertIs(manager._processes[1].process, new_processes[1])
        self.assertIs(manager._processes[2].process, new_processes[2])

    def test_stop_spi_monitor_ignores_stop_failure_and_restarts_siblings(self) -> None:
        manager = cli._MonitorManager()
        old_processes = {channel: mock.Mock(name=f"old_{channel}") for channel in (0, 1, 2)}
        new_processes = {channel: mock.Mock(name=f"new_{channel}") for channel in (1, 2)}
        spi_config = cli._SpiMonitorConfig(capture=cli.SpiCaptureMode.MOSI_MISO, spi_mode=0, timeout_us=100)
        for channel in (0, 1, 2):
            manager._processes[channel] = cli._MonitorSession(
                process=old_processes[channel],
                label=f"spi logical channel {channel}",
                channel=channel,
                stop=lambda channel=channel: None,
                stop_config=cli._MonitorStopConfig(kind="spi", channel=channel),
                spi_config=spi_config,
            )

        with mock.patch("picotrace.app.cli._terminate_monitor_process") as terminate_monitor_process_mock, mock.patch(
            "picotrace.app.cli._stop_spi_logical_channel", side_effect=RuntimeError("stop failed")
        ) as stop_spi_mock, mock.patch("picotrace.app.cli._configure_spi_bus_sessions") as configure_spi_mock, mock.patch(
            "picotrace.app.cli._start_detached_monitor",
            side_effect=[new_processes[1], new_processes[2]],
        ):
            stopped = manager.stop_monitor(0)

        self.assertTrue(stopped)
        self.assertEqual(
            terminate_monitor_process_mock.call_args_list,
            [mock.call(old_processes[1]), mock.call(old_processes[2]), mock.call(old_processes[0])],
        )
        stop_spi_mock.assert_called_once_with(0)
        restarted_sessions = configure_spi_mock.call_args.args[0]
        self.assertEqual([session.channel for session in restarted_sessions], [1, 2])
        self.assertEqual(sorted(manager._processes), [1, 2])

    def test_restart_spi_bus_sessions_preserves_original_failure_when_stop_cleanup_fails(self) -> None:
        manager = cli._MonitorManager()
        existing_process = mock.Mock(name="old_1")
        new_process = mock.Mock(name="new_1")
        shared_config = cli._SpiMonitorConfig(capture=cli.SpiCaptureMode.MOSI_MISO, spi_mode=0, timeout_us=100)
        manager._processes[1] = cli._MonitorSession(
            process=existing_process,
            label="spi logical channel 1",
            channel=1,
            stop=lambda: None,
            stop_config=cli._MonitorStopConfig(kind="spi", channel=1),
            spi_config=shared_config,
        )
        desired_sessions = [
            cli._MonitorSession(
                process=None,
                label="spi logical channel 1",
                channel=1,
                stop=lambda: None,
                stop_config=cli._MonitorStopConfig(kind="spi", channel=1),
                spi_config=shared_config,
            ),
            cli._MonitorSession(
                process=None,
                label="spi logical channel 2",
                channel=2,
                stop=lambda: None,
                stop_config=cli._MonitorStopConfig(kind="spi", channel=2),
                spi_config=shared_config,
            )
        ]

        with mock.patch("picotrace.app.cli._terminate_monitor_process") as terminate_monitor_process_mock, mock.patch(
            "picotrace.app.cli._configure_spi_bus_sessions"
        ), mock.patch(
            "picotrace.app.cli._start_detached_monitor",
            side_effect=[new_process, RuntimeError("start failed")],
        ), mock.patch(
            "picotrace.app.cli._stop_spi_logical_channel", side_effect=RuntimeError("stop failed")
        ) as stop_spi_mock:
            with self.assertRaisesRegex(RuntimeError, "start failed"):
                manager._restart_spi_bus_sessions(0, desired_sessions, stopped_channel=0)

        self.assertEqual(stop_spi_mock.call_args_list, [mock.call(0), mock.call(1)])
        self.assertEqual(terminate_monitor_process_mock.call_args_list, [mock.call(existing_process), mock.call(new_process)])
        self.assertEqual(manager._processes, {})

    def test_start_spi_monitor_restarts_existing_bus_group(self) -> None:
        manager = cli._MonitorManager()
        old_processes = {channel: mock.Mock(name=f"old_{channel}") for channel in (0, 2)}
        new_processes = {channel: mock.Mock(name=f"new_{channel}") for channel in (0, 1, 2)}
        spi_config = cli._SpiMonitorConfig(capture=cli.SpiCaptureMode.MOSI_MISO, spi_mode=0, timeout_us=100)
        for channel in (0, 2):
            manager._processes[channel] = cli._MonitorSession(
                process=old_processes[channel],
                label=f"spi logical channel {channel}",
                channel=channel,
                stop=lambda channel=channel: None,
                stop_config=cli._MonitorStopConfig(kind="spi", channel=channel),
                spi_config=spi_config,
            )

        with mock.patch.object(cli.sys, "platform", "win32"), mock.patch(
            "picotrace.app.cli._terminate_monitor_process"
        ) as terminate_monitor_process_mock, mock.patch(
            "picotrace.app.cli._stop_spi_logical_channel"
        ) as stop_spi_mock, mock.patch("picotrace.app.cli._configure_spi_bus_sessions") as configure_spi_mock, mock.patch(
            "picotrace.app.cli._start_detached_monitor",
            side_effect=[new_processes[0], new_processes[2], new_processes[1]],
        ) as start_detached_monitor_mock:
            manager.start_spi_monitor(1, label="spi logical channel 1", capture=cli.SpiCaptureMode.MOSI_MISO, spi_mode=0, timeout_us=100)

        self.assertEqual(
            terminate_monitor_process_mock.call_args_list,
            [mock.call(old_processes[0]), mock.call(old_processes[2])],
        )
        stop_spi_mock.assert_not_called()
        restarted_sessions = configure_spi_mock.call_args.args[0]
        self.assertEqual([session.channel for session in restarted_sessions], [0, 2, 1])
        self.assertEqual(
            {session.channel: session.spi_config for session in restarted_sessions},
            {
                0: cli._SpiMonitorConfig(capture=cli.SpiCaptureMode.MOSI_MISO, spi_mode=0, timeout_us=100),
                1: cli._SpiMonitorConfig(capture=cli.SpiCaptureMode.MOSI_MISO, spi_mode=0, timeout_us=100),
                2: cli._SpiMonitorConfig(capture=cli.SpiCaptureMode.MOSI_MISO, spi_mode=0, timeout_us=100),
            },
        )
        self.assertEqual(start_detached_monitor_mock.call_count, 3)
        self.assertEqual(sorted(manager._processes), [0, 1, 2])

    def test_start_spi_monitor_reconfigures_sibling_windows_to_new_shared_bus_settings(self) -> None:
        manager = cli._MonitorManager()
        old_processes = {channel: mock.Mock(name=f"old_{channel}") for channel in (0, 2)}
        old_config = cli._SpiMonitorConfig(capture=cli.SpiCaptureMode.MOSI, spi_mode=0, timeout_us=100)
        for channel in (0, 2):
            manager._processes[channel] = cli._MonitorSession(
                process=old_processes[channel],
                label=f"spi logical channel {channel}",
                channel=channel,
                stop=lambda channel=channel: None,
                stop_config=cli._MonitorStopConfig(kind="spi", channel=channel),
                spi_config=old_config,
            )

        with mock.patch.object(cli.sys, "platform", "win32"), mock.patch(
            "picotrace.app.cli._terminate_monitor_process"
        ), mock.patch("picotrace.app.cli._start_detached_monitor", side_effect=[mock.Mock(), mock.Mock(), mock.Mock()]), mock.patch(
            "picotrace.app.cli._configure_spi_bus_sessions"
        ) as configure_spi_mock:
            manager.start_spi_monitor(1, label="spi logical channel 1", capture=cli.SpiCaptureMode.MOSI_MISO, spi_mode=3, timeout_us=250)

        restarted_sessions = configure_spi_mock.call_args.args[0]
        new_config = cli._SpiMonitorConfig(capture=cli.SpiCaptureMode.MOSI_MISO, spi_mode=3, timeout_us=250)
        self.assertEqual(
            {session.channel: session.spi_config for session in restarted_sessions},
            {0: new_config, 1: new_config, 2: new_config},
        )

    def test_stop_all_stops_spi_bus_group_without_restarting_windows(self) -> None:
        manager = cli._MonitorManager()
        spi_config = cli._SpiMonitorConfig(capture=cli.SpiCaptureMode.MOSI_MISO, spi_mode=0, timeout_us=100)
        for channel in (0, 1, 2):
            manager._processes[channel] = cli._MonitorSession(
                process=mock.Mock(name=f"spi_{channel}"),
                label=f"spi logical channel {channel}",
                channel=channel,
                stop=lambda channel=channel: None,
                stop_config=cli._MonitorStopConfig(kind="spi", channel=channel),
                spi_config=spi_config,
            )

        with mock.patch("picotrace.app.cli._stop_spi_bus_group") as stop_spi_bus_group_mock, mock.patch(
            "picotrace.app.cli._start_detached_monitor"
        ) as start_detached_monitor_mock:
            manager.stop_all()

        self.assertEqual(manager._processes, {})
        stop_spi_bus_group_mock.assert_called_once()
        stopped_sessions = stop_spi_bus_group_mock.call_args.args[0]
        self.assertEqual(sorted(session.channel for session in stopped_sessions), [0, 1, 2])
        start_detached_monitor_mock.assert_not_called()

    def test_stop_all_ignores_spi_bus_group_stop_failure(self) -> None:
        manager = cli._MonitorManager()
        spi_config = cli._SpiMonitorConfig(capture=cli.SpiCaptureMode.MOSI_MISO, spi_mode=0, timeout_us=100)
        for channel in (0, 1, 2):
            manager._processes[channel] = cli._MonitorSession(
                process=mock.Mock(name=f"spi_{channel}"),
                label=f"spi logical channel {channel}",
                channel=channel,
                stop=lambda channel=channel: None,
                stop_config=cli._MonitorStopConfig(kind="spi", channel=channel),
                spi_config=spi_config,
            )

        with mock.patch("picotrace.app.cli._terminate_monitor_process") as terminate_monitor_process_mock, mock.patch(
            "picotrace.app.cli._stop_spi_logical_channel", side_effect=RuntimeError("stop failed")
        ):
            manager.stop_all()

        self.assertEqual(manager._processes, {})
        self.assertEqual(terminate_monitor_process_mock.call_count, 3)