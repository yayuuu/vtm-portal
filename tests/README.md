# Tests

`ctest --test-dir build --output-on-failure` runs native C++ tests:

* `protocol.cpp`: compiled service on a private D-Bus, all FileChooser methods,
  version and response types, cancellation including startup races, owner-only
  Close, caller disconnection, concurrent requests and invalid responses.
* `helpers.cpp`: TOML defaults/overrides, scheme validation, Unicode and quoted
  argv round trips, Lua monitor transport, file/directory/multiple selection,
  glob/MIME filters, SaveFile and SaveFiles collision handling.

There is no Python dependency for these tests.

The optional `runtime.py` developer test requires Python and PyGObject and uses
real VTM/Elio executables supplied as arguments. It creates a private session
bus, desktop and temporary dataset; it never changes the active desktop or
portal services. With an optional frontend executable, it verifies real
frontend routing and cancellation. It also checks `xdg-open`'s TTY fallback
using the native `vtm-open`, including a URL with an apostrophe. Python is only
the test driver and test browser, not part of the installed portal.

No test embeds user-specific desktops, profiles or home directories.
The runtime test needs Linux `/proc` and PTYs.
