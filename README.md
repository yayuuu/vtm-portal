# xdg-desktop-portal-vtm

A FileChooser backend for [xdg-desktop-portal](https://github.com/flatpak/xdg-desktop-portal),
using Elio inside an existing VTM desktop.
Includes a configurable browser handler so ordinary `xdg-open` opens links
inside VTM. The default browser is Carbonyl; w3m, lynx and links can also be used.

The backend is a native C/GLib service. C++17 programs handle VTM's Lua monitor
transport, browser/terminal launching and Elio's chooser mode. Building and
running the portal does not require Python or PyGObject.
There are no absolute home-directory paths, desktop names or machine-specific
applications embedded in the project.

## Build on any Linux distribution

The build uses CMake >= 3.16, pkg-config, C11/C++17 compilers and GLib/GIO >= 2.70.
The TOML parser, [toml++ 3.4.0](https://github.com/marzer/tomlplusplus), is vendored
under its MIT license, so configuring/building needs no network downloads.
Native tests also use `dbus-run-session`. Pass `-DBUILD_TESTING=OFF` to build
without tests. There is no APT/dpkg dependency or distro-specific packaging
directory in the code or build system.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --parallel
ctest --test-dir build --output-on-failure
sudo cmake --install build
```

Installation paths follow CMake's GNUInstallDirs and can all be changed.
On systems without systemd, pass `-DWITH_SYSTEMD=OFF`; the standard D-Bus
activation entry still starts the backend directly. When using systemd you
can override `-DSYSTEMD_USER_UNIT_DIR=/path/to/systemd/user`.

The session itself needs a D-Bus session bus, the standard `xdg-desktop-portal`
frontend and `xdg-utils`, in addition to VTM and Elio. Elio must support
`--chooser-file` and `--cwd-file`. These are runtime dependencies independent
of the distribution. The default browser is optional and configurable.

If your terminal session has no D-Bus session bus, start VTM under
`dbus-run-session -- vtm` and run portal-using applications from that desktop.
On systems without systemd, source `session.sh` to import the environment into
D-Bus; skip the `systemctl` commands in the systemd-specific example below.

### Debian build example

Debian 13:

```sh
sudo apt install build-essential cmake pkg-config libglib2.0-dev dbus-daemon
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBEXECDIR=libexec -DCMAKE_INSTALL_SYSCONFDIR=/etc
cmake --build build --parallel
ctest --test-dir build --output-on-failure
sudo cmake --install build
```

Runtime requirements: `xdg-desktop-portal`, `xdg-utils`, GLib/GIO,
VTM with `vtm.desktop.Run` Lua support,
and Elio supporting `--chooser-file` and `--cwd-file`.
Install a terminal browser of your choice, for example `carbonyl-yayuuu`.
Neither VTM nor Elio needs to be patched by this project. No X server is needed.

Debian packaging is maintained separately in the repository-builder project,
not in these portable sources. Package metadata requires `vtm-yayuuu | vtm`
and `elio-yayuuu | elio`; `carbonyl-yayuuu | carbonyl` is recommended because
the browser can be changed.

Staging an installation does not affect the current session:

```sh
DESTDIR="$PWD/dist" cmake --install build
```

For a different install prefix, keep its `bin` directory in your session's PATH.
The D-Bus activation file and systemd unit use the paths configured by CMake.

## Configuration

Defaults: `/etc/xdg-desktop-portal-vtm/config.toml` with the build options above.
User overrides: `$XDG_CONFIG_HOME/xdg-desktop-portal-vtm/config.toml`
(normally `~/.config/xdg-desktop-portal-vtm/config.toml`). Only override the
settings you want to change:

```toml
[vtm]
# For a desktop started with `vtm -p my-desktop`:
session = "my-desktop"

[browser]
command = ["w3m"]
size = "140:45"

[picker]
command = ["elio"]
size = "150:50"
```

Without a `session`, the monitor uses VTM's default session for the current
user. `VTM_DESKTOP_PIN` takes precedence over the configured session. A service
activated through D-Bus cannot inherit variables from an arbitrary shell:
put the session name in the config, or import the session environment as below.
The VTM desktop must already be running under the same user. Helpers fail with
an error when they cannot connect; they do not start a second desktop.

Commands are arrays of arguments, not shell expressions. URL/filename arguments
are appended as separate arguments. For example:

```toml
[browser]
command = ["lynx", "-accept_all_cookies"]
schemes = ["http", "https", "file"]
```

See [data/config.toml](data/config.toml) for all settings. Config is read for
every launch; changing the browser does not require restarting the portal.

## Enable the FileChooser backend

This project implements `org.freedesktop.impl.portal.FileChooser` on the session
bus name `org.freedesktop.impl.portal.desktop.vtm`. The regular
`xdg-desktop-portal` frontend keeps owning `org.freedesktop.portal.Desktop`.
Applications use its standard API, with no knowledge of Elio or VTM.

Install the frontend, then select the backend for your VTM session:

```sh
sudo apt install xdg-desktop-portal xdg-utils
export XDG_CURRENT_DESKTOP=vtm
export BROWSER=vtm-open
dbus-update-activation-environment --systemd XDG_CURRENT_DESKTOP BROWSER PATH
systemctl --user daemon-reload
systemctl --user restart xdg-desktop-portal.service
```

Run the export/import commands when starting your VTM session. The supplied
`/usr/share/xdg-desktop-portal-vtm/session.sh` can be sourced from your VTM
startup script to do this. `BROWSER=vtm-open` is necessary for `xdg-open`'s
generic fallback on sessions without DISPLAY/WAYLAND_DISPLAY (kmscon/SSH).
No fake display variable needs to be set. The supplied
`vtm-portals.conf` selects this backend only for the FileChooser interface;
other interfaces can use other installed backends. Alternatively, merge this
entry into `~/.config/xdg-desktop-portal/portals.conf`:

```ini
[preferred]
org.freedesktop.impl.portal.FileChooser=vtm
```

The service is activated on demand by D-Bus/systemd. It does not need to be
enabled as an always-running service. Diagnose with:

```sh
journalctl --user -u xdg-desktop-portal-vtm -f
```

**Migrating from the old local Python portal:** stop/disable the old
`vtm-portal.service` and remove its custom per-user D-Bus activation file named
`org.freedesktop.portal.Desktop.service` before restarting the regular frontend.
That earlier implementation owned the frontend bus name and conflicts with it.
This repository does not automatically delete or change your existing services.

## Open links with xdg-open

Register the included desktop handler as the browser for this user:

```sh
xdg-mime default vtm-browser.desktop x-scheme-handler/http
xdg-mime default vtm-browser.desktop x-scheme-handler/https
xdg-mime default vtm-browser.desktop text/html
xdg-mime default vtm-browser.desktop application/xhtml+xml
xdg-open 'https://example.org/some-page'
```

`vtm-browser.desktop` runs `vtm-open`, which launches the configured browser in
VTM and passes the complete requested URL. Multiple URLs and local HTML files
are supported. Existing MIME handlers for other kinds of files stay in place.
This project does **not** install or replace `xdg-open`. If a previous local
setup put an `xdg-open` wrapper ahead of `/usr/bin/xdg-open`, remove that wrapper
from PATH before using the normal XDG MIME association mechanism.

There is no `org.freedesktop.impl.portal.OpenURI` backend interface in the XDG
portal protocol. URL opening uses desktop/MIME handlers, which are also used
by the regular OpenURI frontend. Its application-selection UI, if needed,
is provided by an installed AppChooser backend; this project does not implement
AppChooser. See the official [backend documentation](https://flatpak.github.io/xdg-desktop-portal/docs/writing-a-new-backend.html)
and [OpenURI API](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.OpenURI.html).

To launch a terminal command in VTM (for example through an existing generic
`xdg-run` launcher):

```sh
vtm-run -- nvim '/path with spaces/file.txt'
```

For a generic `xdg-run` supporting `terminal_launcher`, use `["vtm-run", "--"]`
as that setting. `xdg-run` is not a standardized XDG utility and is not installed
or replaced by this project.

## Picker behaviour

* OpenFile: one or several existing files, or directories with `directory=true`.
* SaveFile: choose the destination directory in Elio, then enter a filename;
  existing targets require explicit overwrite confirmation. No file is created.
* SaveFiles: choose a destination directory; colliding or duplicate names get
  unique suffixes, preserving the original ordering. No files are created.
* Elio uses its normal navigation/mouse support. Enter confirms selection;
  Space selects multiple items; Ctrl+C cancels. To choose the current directory,
  use Elio's choose action for its highlighted directory (or choose its `.` entry
  when available). For saves, a highlighted file falls back to Elio's current
  directory.
* Multiple portal filters and custom choice controls are offered in a small
  terminal prompt before Elio opens. Glob/MIME filters validate selected files;
  they do not hide nonmatching entries in Elio. Save filename entry remains free.
* Request.Close and a disappearing requesting portal cancel the picker and
  terminate Elio. Separate requests get private mode-0700 job directories.
* Elio's output is line-delimited; filenames containing newlines cannot be
  represented reliably by this chooser protocol.
* The request's `parent_window` and `modal` options do not make VTM windows modal
  or attach them to GUI windows in another compositor. The picker is a normal
  VTM window; cancellation and responses still use the standard portal API.

## Development

`ctest` exercises the compiled backend on a private D-Bus session, cancellation,
caller ownership, invalid responses and concurrent requests. Helper tests cover
Elio translation, filters, collisions and safe argument passing.
They use deterministic stand-ins for VTM/Elio and do not modify the live desktop.

For a source-tree backend run:

```sh
VTM_PORTAL_HELPER="$PWD/build/vtm-portal-helper" \
VTM_PORTAL_SYSTEM_CONFIG="$PWD/data/config.toml" \
./build/xdg-desktop-portal-vtm
```

`VTM_PORTAL_CONFIG` selects an additional explicit TOML file for testing.

An optional end-to-end test uses a separate VTM desktop and a private session
bus with the real Elio binary. Only this optional developer test uses Python
and PyGObject; neither is installed or used by the portal itself:

```sh
python3 tests/runtime.py build/xdg-desktop-portal-vtm /path/to/vtm /path/to/elio
```

Pass a fourth path to the regular `xdg-desktop-portal` executable to also check
the frontend API. The test checks actual OpenFile/SaveFile/SaveFiles selection,
cancellation, and a complete URL passed by `xdg-open` into VTM without X11.
