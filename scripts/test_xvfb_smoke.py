#!/usr/bin/env python3
"""Isolated real TLS/input/clipboard smoke test; never uses the user's DISPLAY."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import select
import shutil
import signal
import socket
import subprocess
import tempfile
import time
import io
import ctypes
import ctypes.util
from PIL import Image
from urllib.parse import urlparse, unquote


def wait_for(predicate, description, timeout=12):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.1)
    raise RuntimeError("Timed out: " + description)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, required=True)
    parser.add_argument("--cycles", type=int, default=1, help="Additional cross-screen/return cycles (1-10000)")
    parser.add_argument("--file-mib", type=int, default=0,
                        help="Synthetic file size in MiB (0 retains the 112 KiB smoke fixture; max 64)")
    parser.add_argument("--motion-burst", action="store_true",
                        help="Pause only the isolated client and verify hover backlog coalescing")
    parser.add_argument("--clipboard-policy", action="store_true",
                        help="Verify a device opt-out blocks both directions while input remains active")
    args = parser.parse_args()
    if not 1 <= args.cycles <= 10000:
        parser.error("--cycles must be between 1 and 10000")
    if not 0 <= args.file_mib <= 64:
        parser.error("--file-mib must be between 0 and 64")
    binaries = args.bin_dir.resolve()
    for name in ("Xvfb", "xdotool", "xclip", "openssl"):
        if not shutil.which(name):
            parser.error("Missing dependency: " + name)
    root = Path(tempfile.mkdtemp(prefix="weave-xvfb-smoke-"))
    root.chmod(0o700)
    processes, streams = [], []
    burst_window = None
    report = {"artifacts": str(root), "checks": [], "ok": False}

    def run(command, env=None, **kwargs):
        return subprocess.run(command, env=env, check=True, capture_output=True,
                              timeout=10, **kwargs)

    def start(command, name, env=None, **kwargs):
        stream = open(root / (name + ".log"), "wb")
        streams.append(stream)
        process = subprocess.Popen(command, env=env, stdout=stream, stderr=stream,
                                   start_new_session=True, **kwargs)
        processes.append(process)
        return process

    def logs(name):
        return (root / (name + ".log")).read_text(errors="replace")

    def xdisplay(name):
        read_fd, write_fd = os.pipe()
        try:
            start(["Xvfb", "-displayfd", str(write_fd), "-screen", "0", "800x600x24",
                   "-nolisten", "tcp", "-noreset"], name, pass_fds=(write_fd,))
            os.close(write_fd)
            write_fd = None
            if not select.select([read_fd], [], [], 5)[0]:
                raise RuntimeError("Xvfb did not allocate display")
            number = os.read(read_fd, 32).decode().strip()
            if not number.isdigit():
                raise RuntimeError("Invalid Xvfb display")
        finally:
            os.close(read_fd)
            if write_fd is not None:
                os.close(write_fd)
        env = os.environ.copy()
        env.update(DISPLAY=":" + number, XDG_DATA_HOME=str(root / name / "data"),
                   XDG_CONFIG_HOME=str(root / name / "config"), TMPDIR=str(root))
        env.pop("XAUTHORITY", None)
        return env

    def profile(name):
        directory = root / name
        ssl = directory / "SSL"
        (ssl / "Fingerprints").mkdir(parents=True)
        run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
             "-subj", "/CN=" + name, "-keyout", str(ssl / "key.pem"), "-out", str(ssl / "cert.pem")])
        (ssl / "Barrier.pem").write_bytes((ssl / "key.pem").read_bytes() + (ssl / "cert.pem").read_bytes())
        der = run(["openssl", "x509", "-in", str(ssl / "cert.pem"), "-outform", "DER"]).stdout
        fingerprint = "v2:sha256:" + hashlib.sha256(der).hexdigest() + "\n"
        (ssl / "Fingerprints" / "Local.txt").write_text(fingerprint)
        return directory, fingerprint

    try:
        server_env, client_env = xdisplay("server-x"), xdisplay("client-x")
        server_profile, server_fp = profile("server-profile")
        client_profile, client_fp = profile("client-profile")
        (server_profile / "SSL/Fingerprints/TrustedClients.txt").write_text(server_fp)
        (client_profile / "SSL/Fingerprints/TrustedServers.txt").write_text(server_fp)
        config = root / "server.conf"
        config.write_text("section: screens\n smoke-server:\n smoke-client:\nend\n"
                          "section: links\n smoke-server:\n  right = smoke-client\n"
                          " smoke-client:\n  left = smoke-server\nend\n")
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        if port == 24800:
            raise RuntimeError("Refusing reserved production port")
        report["port"] = port
        server = start([str(binaries / "weaves"), "-f", "--no-restart", "--enable-drag-drop", "--debug", "DEBUG",
               *(["--low-latency-mode"] if args.motion_burst else []),
               "--name", "smoke-server", "--profile-dir", str(server_profile),
               "--config", str(config), "--address", f"127.0.0.1:{port}"], "server", server_env)
        wait_for(lambda: "started server" in logs("server"), "server start")
        client_command = [str(binaries / "weavec"), "-f", "--no-restart", "--enable-drag-drop", "--debug", "DEBUG",
               "--name", "smoke-client", "--profile-dir", str(client_profile),
               f"127.0.0.1:{port}"]
        untrusted = start(client_command, "untrusted-client", client_env)
        wait_for(lambda: "peer certificate fingerprint does not match" in logs("server"), "untrusted client refusal")
        if "connected to server" in logs("untrusted-client"):
            raise RuntimeError("Untrusted client reached application handshake")
        if untrusted.poll() is None:
            os.killpg(untrusted.pid, signal.SIGTERM)
            untrusted.wait(timeout=3)
        report["checks"].append("untrusted client certificate rejected")
        (server_profile / "SSL/Fingerprints/TrustedClients.txt").write_text(client_fp)
        if args.motion_burst:
            client_env["WEAVE_INPUT_TRACE_PREFIX"] = str(root / "motion-trace")
        client = start(client_command, "client", client_env)
        wait_for(lambda: "connected to server" in logs("client"), "authenticated application handshake")
        report["checks"].append("mutually-trusted TLS application handshake")
        token = "weave-isolated-clipboard-" + root.name
        clipboard = start(["xclip", "-selection", "clipboard", "-in", "-quiet"], "clipboard-owner", server_env,
                          stdin=subprocess.PIPE)
        clipboard.stdin.write(token.encode())
        clipboard.stdin.close()
        run(["xdotool", "mousemove", "400", "300"], server_env)
        run(["xdotool", "mousemove_relative", "--", "450", "0"], server_env)
        wait_for(lambda: 'switch from "smoke-server" to "smoke-client"' in logs("server"), "cross-screen movement")
        report["checks"].append("mouse entered client")
        def pointer(env):
            fields = run(["xdotool", "getmouselocation", "--shell"], env).stdout.decode().splitlines()
            values = dict(field.split("=", 1) for field in fields)
            return [int(values["X"]), int(values["Y"])]
        initial_pointer = pointer(client_env)
        run(["xdotool", "mousemove_relative", "--", "40", "0"], server_env)
        wait_for(lambda: pointer(client_env) != initial_pointer, "client X pointer moved")
        report["client_pointer_before"] = initial_pointer
        report["client_pointer_after"] = pointer(client_env)
        report["checks"].append("client X pointer changed from injected server motion")
        if args.motion_burst:
            before = pointer(client_env)
            begin = time.time_ns()
            # This is our child on a private Xvfb display, never a live desktop.
            os.kill(client.pid, signal.SIGSTOP)
            try:
                for _ in range(14):
                    run(["xdotool", "mousemove_relative", "--", "3", "0"], server_env)
                    time.sleep(0.005)
                time.sleep(0.02)
            finally:
                os.kill(client.pid, signal.SIGCONT)
            expected = [before[0] + 42, before[1]]
            wait_for(lambda: pointer(client_env) == expected, "latest queued pointer position")
            burst_window = (begin, time.time_ns(), client.pid)
            report["burst_pointer_expected"] = expected
            report["burst_pointer_actual"] = pointer(client_env)

        def clipboard_matches():
            try:
                received = subprocess.run(["xclip", "-selection", "clipboard", "-out"],
                                          env=client_env, capture_output=True, timeout=1)
                return received.returncode == 0 and received.stdout.decode() == token
            except subprocess.TimeoutExpired:
                return False
        wait_for(clipboard_matches, "exact clipboard publication")
        report["checks"].append("exact text clipboard propagated")
        fixture = Image.new("RGB", (64, 48))
        fixture.putdata([((x * 17) % 256, (y * 29) % 256, ((x + y) * 11) % 256)
                         for y in range(48) for x in range(64)])
        encoded = io.BytesIO()
        fixture.save(encoded, format="PNG")
        image_owner = start(["xclip", "-selection", "clipboard", "-target", "image/png", "-in", "-quiet"],
                            "image-owner", server_env, stdin=subprocess.PIPE)
        image_owner.stdin.write(encoded.getvalue())
        image_owner.stdin.close()
        def image_matches():
            try:
                received = subprocess.run(["xclip", "-selection", "clipboard", "-target", "image/png", "-out"],
                                          env=client_env, capture_output=True, timeout=1)
                if received.returncode:
                    return False
                actual = Image.open(io.BytesIO(received.stdout)).convert("RGB")
                return actual.size == fixture.size and actual.tobytes() == fixture.tobytes()
            except (subprocess.TimeoutExpired, OSError):
                return False
        wait_for(image_matches, "PNG clipboard decoded pixel equality")
        report["checks"].append("PNG clipboard decoded pixels exactly equal")
        source = root / "synthetic-transfer.txt"
        content = ("isolated weave file payload\n" * 4096).encode()
        if args.file_mib:
            size = args.file_mib * 1024 * 1024
            content = (content * ((size + len(content) - 1) // len(content)))[:size]
        report["file_bytes"] = len(content)
        report["file_sha256"] = hashlib.sha256(content).hexdigest()
        source.write_bytes(content)
        file_owner = start(["xclip", "-selection", "clipboard", "-target", "text/uri-list", "-in", "-quiet"],
                           "file-owner", server_env, stdin=subprocess.PIPE)
        file_owner.stdin.write((source.as_uri() + "\r\n").encode())
        file_owner.stdin.close()
        wait_for(lambda: "cached local file clipboard" in logs("server"), "file clipboard captured")
        run(["xdotool", "mousemove_relative", "--", "-900", "0"], server_env)
        wait_for(lambda: 'switch from "smoke-client" to "smoke-server"' in logs("server"), "return before file paste")
        run(["xdotool", "mousemove_relative", "--", "900", "0"], server_env)
        wait_for(lambda: logs("server").count('switch from "smoke-server" to "smoke-client"') >= 2,
                 "reenter for file paste")
        def file_matches():
            try:
                result = subprocess.run(["xclip", "-selection", "clipboard", "-target", "text/uri-list", "-out"],
                                        env=client_env, capture_output=True, timeout=1)
                for line in result.stdout.decode().splitlines():
                    parsed = urlparse(line)
                    if parsed.scheme != "file":
                        continue
                    destination = Path(unquote(parsed.path)).resolve()
                    if destination == source or root not in destination.parents:
                        continue
                    if destination.is_file() and destination.read_bytes() == content:
                        report["file_materialized_relative_path"] = str(destination.relative_to(root))
                        report["materialized_file_sha256"] = hashlib.sha256(destination.read_bytes()).hexdigest()
                        return True
                return False
            except (subprocess.TimeoutExpired, OSError, UnicodeError):
                return False
        wait_for(file_matches, "file URI materialization and content integrity", timeout=20)
        report["checks"].append("file clipboard materialized distinct temporary file with exact contents")
        # Stress input independently after validating the large-file data path.
        text_owner = start(["xclip", "-selection", "clipboard", "-in", "-quiet"],
                           "restored-text-owner", server_env, stdin=subprocess.PIPE)
        text_owner.stdin.write(token.encode())
        text_owner.stdin.close()
        wait_for(clipboard_matches, "text clipboard restored before input stress")
        if args.clipboard_policy:
            original_config = config.read_text()
            def reload_config(contents):
                before = logs("server").count("reloaded configuration")
                config.write_text(contents)
                os.kill(server.pid, signal.SIGHUP)
                wait_for(lambda: logs("server").count("reloaded configuration") > before,
                         "isolated policy reload")
            def own_text(env, value, label):
                owner = start(["xclip", "-selection", "clipboard", "-in", "-quiet"],
                              label, env, stdin=subprocess.PIPE)
                owner.stdin.write(value.encode()); owner.stdin.close()
            def read_text(env):
                return run(["xclip", "-selection", "clipboard", "-out"], env).stdout.decode()
            disabled_before = logs("client").count("clipboard sharing is disabled")
            reload_config(original_config.replace(" smoke-client:\n", " smoke-client:\n  clipboardSharing = false\n", 1)
                          + "section: options\n clipboardSharing = true\nend\n")
            wait_for(lambda: logs("client").count("clipboard sharing is disabled") > disabled_before,
                     "client received effective disabled policy")
            # Only this isolated child is restarted. A fresh bulk attachment
            # used to bypass routing policy and retry an initial empty snapshot.
            os.kill(client.pid, signal.SIGTERM); client.wait(timeout=3)
            client = start(client_command, "client", client_env)
            wait_for(lambda: "connected to server" in logs("client") and
                     "clipboard sharing is disabled" in logs("client"),
                     "excluded client reconnected with policy")
            entered = logs("server").count('switch from "smoke-server" to "smoke-client"')
            run(["xdotool", "mousemove", "400", "300"], server_env)
            run(["xdotool", "mousemove_relative", "--", "900", "0"], server_env)
            wait_for(lambda: logs("server").count('switch from "smoke-server" to "smoke-client"') > entered,
                     "entry into reconnected excluded client")
            server_only, client_only = token + "-server-only", token + "-client-only"
            own_text(client_env, client_only, "excluded-client-copy")
            own_text(server_env, server_only, "server-policy-copy")
            time.sleep(1.5)
            if read_text(client_env) != client_only or read_text(server_env) != server_only:
                raise RuntimeError("Excluded device clipboard was overwritten")
            if "snapshot was rejected" in logs("client") or "snapshot was queued for publication" in logs("client"):
                raise RuntimeError("Clipboard retry bypassed excluded-device policy after reconnect")
            report["checks"].append("excluded client reconnect produced no clipboard publication or rejection")
            before = pointer(client_env)
            run(["xdotool", "mousemove_relative", "--", "25", "0"], server_env)
            wait_for(lambda: pointer(client_env) != before, "input still delivered to clipboard-excluded client")
            returned = logs("server").count('switch from "smoke-client" to "smoke-server"')
            run(["xdotool", "mousemove_relative", "--", "-900", "0"], server_env)
            wait_for(lambda: logs("server").count('switch from "smoke-client" to "smoke-server"') > returned,
                     "return from excluded client")
            time.sleep(1.2)
            if read_text(server_env) != server_only:
                raise RuntimeError("Excluded client clipboard leaked on return")
            report["checks"].append("device opt-out blocked both clipboard directions; pointer and return still worked")
            reload_config(original_config)
            token += "-reenabled"
            own_text(server_env, token, "reenabled-server-copy")
            entered = logs("server").count('switch from "smoke-server" to "smoke-client"')
            run(["xdotool", "mousemove_relative", "--", "900", "0"], server_env)
            wait_for(lambda: logs("server").count('switch from "smoke-server" to "smoke-client"') > entered,
                     "entry after clipboard opt-in")
            wait_for(clipboard_matches, "clipboard policy opt-in restored exact text")
            report["checks"].append("clipboard opt-in restored on the existing connection")
        inward_before = logs("server").count('switch from "smoke-client" to "smoke-server"')
        outward_before = logs("server").count('switch from "smoke-server" to "smoke-client"')
        run(["xdotool", "mousemove_relative", "--", "-900", "0"], server_env)
        wait_for(lambda: logs("server").count('switch from "smoke-client" to "smoke-server"') > inward_before, "mouse return across edge")
        report["checks"].append("mouse returned across left edge")
        run(["xdotool", "mousemove_relative", "--", "900", "0"], server_env)
        wait_for(lambda: logs("server").count('switch from "smoke-server" to "smoke-client"') > outward_before,
                 "second client entry")
        x11 = ctypes.CDLL(ctypes.util.find_library("X11"))
        x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x11.XOpenDisplay.restype = ctypes.c_void_p
        x11.XQueryKeymap.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        def keys_pressed(env):
            display = x11.XOpenDisplay(env["DISPLAY"].encode())
            if not display:
                raise RuntimeError("Cannot inspect isolated keymap")
            try:
                keymap = ctypes.create_string_buffer(32)
                x11.XQueryKeymap(display, keymap)
                report.setdefault("pressed_keycodes", {})[env["DISPLAY"]] = [
                    byte * 8 + bit for byte, value in enumerate(keymap.raw)
                    for bit in range(8) if value & (1 << bit)]
                return any(keymap.raw)
            finally:
                x11.XCloseDisplay(display)
        run(["xdotool", "keydown", "ctrl", "shift"], server_env)
        wait_for(lambda: keys_pressed(client_env), "held modifiers reached client")
        run(["xdotool", "key", "ctrl+alt+shift+F12"], server_env)
        wait_for(lambda: "emergency return to primary" in logs("server"), "emergency return")
        report["checks"].append("Ctrl+Alt+Shift+F12 returned primary")
        run(["xdotool", "keyup", "ctrl", "shift", "alt", "F12"], server_env)
        def all_keys_released():
            client_pressed = keys_pressed(client_env)
            server_pressed = keys_pressed(server_env)
            return not client_pressed and not server_pressed
        wait_for(all_keys_released, "all held keys released")
        report["checks"].append("held modifiers released on both X displays after emergency return")
        stress_started = time.monotonic()
        for cycle in range(args.cycles):
            outward = logs("server").count('switch from "smoke-server" to "smoke-client"')
            inward = logs("server").count('switch from "smoke-client" to "smoke-server"')
            run(["xdotool", "mousemove_relative", "--", "900", "0"], server_env)
            wait_for(lambda: logs("server").count('switch from "smoke-server" to "smoke-client"') > outward,
                     "stress outward crossing")
            run(["xdotool", "mousemove_relative", "--", "-900", "0"], server_env)
            wait_for(lambda: logs("server").count('switch from "smoke-client" to "smoke-server"') > inward,
                     "stress inward crossing")
            report["cycles_completed"] = cycle + 1
            if (cycle + 1) % 100 == 0:
                print(f"Completed {cycle + 1}/{args.cycles} isolated crossing cycles", flush=True)
        report["checks"].append("repeated crossing and return cycles completed")
        report["stress_seconds"] = round(time.monotonic() - stress_started, 3)
        if any(process.poll() is not None for process in [processes[0], processes[1], server, client]):
            raise RuntimeError("Core/display process exited prematurely")
        report["ok"] = True
    except Exception as error:
        report["error"] = str(error)
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=3)
        for stream in streams:
            stream.close()
        if burst_window is not None:
            try:
                begin, end, pid = burst_window
                with (root / f"motion-trace.{pid}.csv").open() as trace:
                    rows = [row for row in csv.DictReader(trace)
                            if begin <= int(row["wall_ns"]) <= end]
                received = sum(row["stage"] == "R" for row in rows)
                posted = sum(row["stage"] == "P" for row in rows)
                report["burst_received_positions"] = received
                report["burst_native_posts"] = posted
                if received < 8 or not 0 < posted < received:
                    raise RuntimeError(f"hover backlog was not coalesced: received={received}, posts={posted}")
                report["checks"].append("queued hover positions coalesced with exact final X pointer")
            except Exception as error:
                report["ok"] = False
                report["error"] = str(error)
        # The ephemeral certificate keys are not useful artifacts.
        for name in ("server-profile", "client-profile"):
            if (root / name).exists():
                shutil.rmtree(root / name)
        (root / "result.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, indent=2))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
