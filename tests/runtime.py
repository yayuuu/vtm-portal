"""Optional real VTM/Elio smoke test, using a private desktop and session bus.

Usage: /usr/bin/python3 tests/runtime.py build/xdg-desktop-portal-vtm /path/to/vtm /path/to/elio [frontend]
No active desktop, MIME associations or existing portal services are changed.
"""
import concurrent.futures
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import shlex
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
from urllib.parse import unquote, urlsplit

if os.environ.get("VTM_PORTAL_RUNTIME_PRIVATE_BUS") != "1":
    env = {**os.environ, "VTM_PORTAL_RUNTIME_PRIVATE_BUS": "1"}
    raise SystemExit(subprocess.call(["dbus-run-session", "--", sys.executable, *sys.argv], env=env))

from gi.repository import Gio, GLib

binary, vtm, elio = (str(Path(arg).resolve()) for arg in sys.argv[1:4])
frontend_binary = str(Path(sys.argv[4]).resolve()) if len(sys.argv) > 4 else None
project = Path(__file__).resolve().parents[1]
NAME = "org.freedesktop.impl.portal.desktop.vtm"
ROOT = "/org/freedesktop/portal/desktop"
IFACE = "org.freedesktop.impl.portal.FileChooser"
pin = "vtm-portal-runtime-" + str(os.getpid())

with tempfile.TemporaryDirectory(prefix="vtm-portal-runtime-") as folder:
    tmp = Path(folder)
    dataset = tmp / "files"
    dataset.mkdir()
    chosen = dataset / "chosen file.txt"
    chosen.write_text("Test portal file\n")
    (tmp / "runtime").mkdir(mode=0o700)
    (tmp / "config/xdg-desktop-portal").mkdir(parents=True)
    (tmp / "config/xdg-desktop-portal/portals.conf").write_text(
        "[preferred]\norg.freedesktop.impl.portal.FileChooser=vtm\n")
    config = tmp / "config.toml"
    config.write_text(f'[vtm]\ncommand=["{vtm}"]\nsession="{pin}"\n'
                      f'[picker]\ncommand=["{elio}"]\nsize="110:34"\n')
    desktop_config = tmp / "desktop.xml"
    desktop_config.write_text('<config><desktop><taskbar><item*/><autorun run*/></taskbar>'
                             '<panel><cmd=""/></panel></desktop></config>')
    env = {**os.environ, "XDG_RUNTIME_DIR": str(tmp / "runtime"),
           "XDG_CONFIG_HOME": str(tmp / "config"), "XDG_CURRENT_DESKTOP": "vtm",
           "VTM_PORTAL_CONFIG": str(config), "VTM_PORTAL_HELPER": str(Path(binary).parent / "vtm-portal-helper"),
           "VTM": "", "VTM_CONFIG": "", "VTM_DESKTOP_PIN": "", "TERM": "xterm-256color"}
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 45, 140, 1120, 720))

    def prepare():
        os.setsid()
        fcntl.ioctl(slave, termios.TIOCSCTTY, 0)

    desktop = subprocess.Popen([vtm, "-t", "-p", pin, "-c", str(desktop_config)],
        stdin=slave, stdout=slave, stderr=slave, preexec_fn=prepare, env=env)
    os.close(slave)
    backend = subprocess.Popen([binary], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    frontend = None
    output = bytearray()
    conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    pool = concurrent.futures.ThreadPoolExecutor(max_workers=2)

    def drain():
        while select.select([master], [], [], .01)[0]:
            try: data = os.read(master, 1048576)
            except OSError: return
            if not data: return
            output.extend(data)
            del output[:-1048576]
            if b"\x1b[c" in data: os.write(master, b"\x1b[?1;2c")
            if b"\x1b[?u" in data: os.write(master, b"\x1b[?0u")
            if b"\x1b[6n" in data: os.write(master, b"\x1b[1;1R")

    def wait(predicate, timeout=15):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            drain()
            value = predicate()
            if value: return value
            time.sleep(.03)
        raise AssertionError("Timed out; terminal output: " + output.decode(errors="replace")[-3000:])

    def call(method, params, iface=IFACE, path=ROOT, name=NAME):
        return conn.call_sync(name, path, iface, method, params, None,
                             Gio.DBusCallFlags.NO_AUTO_START, 40000, None)

    def ready():
        try:
            return call("Get", GLib.Variant("(ss)", (IFACE, "version")),
                        "org.freedesktop.DBus.Properties").unpack()[0] == 4
        except GLib.Error: return False

    def job():
        return next(iter((tmp / "runtime").glob("vtm-portal-*/picker-*")), None)

    def elio_active(item):
        if not (item / "started").exists(): return False
        worker = (item / "started").read_text()
        for proc in Path("/proc").glob("[0-9]*"):
            try:
                if f"PPid:\t{worker}\n" in (proc / "status").read_text() and \
                   os.fsencode(item / "selection.txt") in (proc / "cmdline").read_bytes().split(b"\0"):
                    return int(proc.name)
            except OSError: pass
        return False

    try:
        wait(ready)
        time.sleep(.5)
        drain()
        options = {"current_folder": GLib.Variant("ay", list(os.fsencode(dataset) + b"\0"))}
        handle = ROOT + "/request/runtime/open"
        future = pool.submit(call, "OpenFile", GLib.Variant("(osssa{sv})", (handle, "test", "", "Real Elio", options)))
        item = wait(job)
        wait(lambda: elio_active(item))
        time.sleep(.5)
        drain()
        os.write(master, b"\r")
        wait(future.done)
        result = future.result().unpack()
        assert result[0] == 0 and result[1]["uris"] == [chosen.as_uri()], result
        wait(lambda: not item.exists())
        print("PASS: compiled backend -> real VTM -> real Elio -> selected file URI", flush=True)

        filter_options = {**options,
            "filters": GLib.Variant("a(sa(us))", [("Images", [(0, "*.png")]), ("Text", [(1, "text/plain")])]),
            "choices": GLib.Variant("a(ssa(ss)s)", [
                ("encoding", "Encoding", [("utf8", "UTF-8"), ("latin", "Latin")], "utf8"),
                ("readonly", "Read only", [], "false")])}
        handle = ROOT + "/request/runtime/filters"
        future = pool.submit(call, "OpenFile", GLib.Variant("(osssa{sv})",
            (handle, "test", "", "Options Elio", filter_options)))
        item = wait(job)
        wait(lambda: (item / "started").exists())
        time.sleep(.5)
        drain()
        os.write(master, b"2\r2\ryes\r")
        wait(lambda: elio_active(item))
        time.sleep(.5)
        drain()
        os.write(master, b"\r")
        wait(future.done)
        result = future.result().unpack()
        assert result[0] == 0 and result[1]["uris"] == [chosen.as_uri()], result
        assert result[1]["current_filter"] == ("Text", [(1, "text/plain")]), result
        assert result[1]["choices"] == [("encoding", "latin"), ("readonly", "true")], result
        wait(lambda: not item.exists())
        print("PASS: real picker filter and custom-choice prompts", flush=True)

        options["current_name"] = GLib.Variant("s", "saved file.txt")
        handle = ROOT + "/request/runtime/save"
        future = pool.submit(call, "SaveFile", GLib.Variant("(osssa{sv})", (handle, "test", "", "Save Elio", options)))
        item = wait(job)
        picker_pid = wait(lambda: elio_active(item))
        time.sleep(.5)
        drain()
        os.write(master, b"\r")
        wait(lambda: not Path(f"/proc/{picker_pid}").exists())
        time.sleep(.3)
        drain()
        os.write(master, b"\r")
        wait(future.done)
        result = future.result().unpack()
        assert result[0] == 0 and result[1]["uris"] == [(dataset / "saved file.txt").as_uri()], result
        assert not (dataset / "saved file.txt").exists()
        wait(lambda: not item.exists())
        print("PASS: real SaveFile directory selection and filename prompt", flush=True)

        options["files"] = GLib.Variant("aay", [list(os.fsencode(chosen.name) + b"\0")])
        handle = ROOT + "/request/runtime/savefiles"
        future = pool.submit(call, "SaveFiles", GLib.Variant("(osssa{sv})", (handle, "test", "", "Save several", options)))
        item = wait(job)
        wait(lambda: elio_active(item))
        time.sleep(.5)
        drain()
        os.write(master, b"\r")
        wait(future.done)
        result = future.result().unpack()
        assert result[0] == 0 and [unquote(urlsplit(uri).path) for uri in result[1]["uris"]] == [str(dataset / "chosen file (1).txt")], result
        wait(lambda: not item.exists())
        print("PASS: real SaveFiles returns unique target names", flush=True)

        handle = ROOT + "/request/runtime/cancel"
        future = pool.submit(call, "OpenFile", GLib.Variant("(osssa{sv})", (handle, "test", "", "Cancel Elio", options)))
        item = wait(job)
        pid = wait(lambda: elio_active(item))
        call("Close", None, "org.freedesktop.impl.portal.Request", handle)
        wait(future.done)
        assert future.result().unpack()[0] == 1
        wait(lambda: not item.exists())
        wait(lambda: not Path(f"/proc/{pid}").exists())
        print("PASS: Request.Close terminates the actual Elio picker", flush=True)

        if frontend_binary:
            env["XDG_DESKTOP_PORTAL_DIR"] = str(project / "data")
            frontend = subprocess.Popen([frontend_binary, "--verbose"], env=env,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)

            def frontend_ready():
                try:
                    return bool(call("Get", GLib.Variant("(ss)",
                                ("org.freedesktop.portal.FileChooser", "version")),
                                "org.freedesktop.DBus.Properties", name="org.freedesktop.portal.Desktop"))
                except GLib.Error:
                    if frontend.poll() is not None:
                        raise AssertionError(frontend.stderr.read().decode())
                    return False

            wait(frontend_ready)
            owner_pid = conn.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus",
                "org.freedesktop.DBus", "GetConnectionUnixProcessID",
                GLib.Variant("(s)", ("org.freedesktop.portal.Desktop",)), None,
                Gio.DBusCallFlags.NONE, 5000, None).unpack()[0]
            assert owner_pid == frontend.pid, "Unexpected frontend owns the bus name"
            responses = []
            conn.signal_subscribe("org.freedesktop.portal.Desktop", "org.freedesktop.portal.Request",
                "Response", None, None, Gio.DBusSignalFlags.NONE,
                lambda c, sender, path, iface, name, params: responses.append(params.unpack()))
            options["handle_token"] = GLib.Variant("s", "frontendtest")
            call("OpenFile", GLib.Variant("(ssa{sv})", ("", "Frontend Elio", options)),
                 "org.freedesktop.portal.FileChooser", name="org.freedesktop.portal.Desktop")
            item = wait(job)
            wait(lambda: elio_active(item))
            time.sleep(.5)
            drain()
            os.write(master, b"\r")

            def response():
                context = GLib.MainContext.default()
                while context.pending(): context.iteration(False)
                return responses

            wait(response)
            assert responses[0][0] == 0 and responses[0][1]["uris"] == [chosen.as_uri()], responses
            print("PASS: standard portal frontend -> new backend -> VTM/Elio -> standard Response", flush=True)

        # The real xdg-open TTY fallback must pass the whole URI into VTM.
        log = tmp / "browser-argv.json"
        browser = tmp / "browser.py"
        browser.write_text("import json,sys\nfrom pathlib import Path\n"
                           "Path(sys.argv[1]).write_text(json.dumps(sys.argv[2:]))\n")
        with config.open("a") as stream:
            stream.write(f'\n[browser]\ncommand={json.dumps([sys.executable, str(browser), str(log)])}\n')
        bindir = tmp / "bin"
        bindir.mkdir()
        opener = bindir / "vtm-open"
        opener.write_text("#!/bin/sh\nexec " + shlex.join([
            str(Path(binary).parent / "vtm-open")]) + ' "$@"\n')
        opener.chmod(0o755)
        uri = "https://example.org/full/path?q=quoted%20text&x='value'"
        open_env = {**env, "BROWSER": "vtm-open", "PATH": str(bindir) + os.pathsep + os.environ["PATH"]}
        for key in ("DISPLAY", "WAYLAND_DISPLAY", "DE", "KDE_FULL_SESSION", "GNOME_DESKTOP_SESSION_ID"):
            open_env.pop(key, None)
        subprocess.run(["xdg-open" if not Path("/usr/bin/xdg-open").exists() else "/usr/bin/xdg-open", uri],
                       env=open_env, check=True, timeout=15)
        wait(log.exists)
        assert json.loads(log.read_text()) == [uri], (json.loads(log.read_text()), uri)
        print("PASS: real xdg-open without X11 -> configurable browser in real VTM, full URI preserved", flush=True)
    finally:
        if frontend:
            frontend.terminate()
            try: frontend.communicate(timeout=4)
            except subprocess.TimeoutExpired:
                frontend.kill()
                frontend.communicate()
        backend.terminate()
        backend.communicate(timeout=5)
        for proc in Path("/proc").glob("[0-9]*"):
            try:
                if os.fsencode(pin) in (proc / "cmdline").read_bytes().split(b"\0"):
                    os.kill(int(proc.name), signal.SIGTERM)
            except OSError: pass
        try: desktop.wait(timeout=3)
        except subprocess.TimeoutExpired:
            desktop.kill()
            desktop.wait()
        os.close(master)
        pool.shutdown(wait=True)
