# Bundled installer releases

The installer loads `v7/install-button.js`. All relative imports, including the
cycle from the dialog through ESP32-C3 and ESP32 back to the dialog, resolve inside
that same release directory without query strings.

When changing this bundle, publish the complete module graph under a new version
directory and update the installer HTML entry point. Do not add cache-busting query
strings to individual imports: browsers treat those as different module identities,
which can execute custom-element registrations twice. The HTML is revalidated via
`docs/_headers`; versioned JavaScript URLs isolate it from previously cached bundles.

Suger RGB changes in v7:

- Update the board name to ESP32-C3 Pro Mini in the serial connection help.

Suger RGB changes in v6:

- Await dialog loading before opening the selected serial port and handle rejection.
- Await installation, reset, disconnect, and reconnect in order. Terminal states are
  shown after recovery, and session failures offer closing and reconnecting.
- Release the serial writer lock even when a write fails, so disconnect can finish.
- Always attempt transport disconnect when reset fails; preserve initialization
  exception names/messages instead of treating every failure as a BOOT problem.

Run the focused regressions from the repository root:

```sh
node --test tests/installer.test.mjs
```

The tests simulate a serial port and execute the bundled installation method. They
never access Web Serial or flash hardware. Browser smoke validation additionally
imports the entry, dialog, and every chip module in one page to check actual ES
module identities and custom-element registration.
