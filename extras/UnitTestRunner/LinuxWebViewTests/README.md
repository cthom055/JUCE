# Linux WebView stability tests

From the JUCE directory, run the lightweight POSIX fault tests:

```sh
bash extras/UnitTestRunner/linux-webview-transport-tests.sh
```

They compile the production helpers and check partial/interrupted writes,
SIGPIPE isolation, retained URI requests, descriptor closure, and bounded
normal/TERM/KILL/already-reaped child cleanup.

For real WebKitGTK callback and lifecycle tests (requires the JUCE Linux build
dependencies, WebKitGTK development headers/runtime, Xvfb and xauth):

```sh
cmake -S extras/UnitTestRunner/LinuxWebViewTests -B build/webview-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build/webview-tests --parallel 4
dbus-run-session -- xvfb-run -a build/webview-tests/LinuxWebViewTests_artefacts/Release/LinuxWebViewTests
```

The executable first runs the `Linux WebView` JUCE unit-test category for
framing, terminal EOF, interrupted reads, truncation and oversize rejection. It then loads a real
page through the asynchronous resource provider, interleaves callback-bearing
and callback-free evaluations, checks exception/undefined results, destroys
the browser from a completion callback, reopens it, and kills a paused helper
while an evaluation is pending. Failure delivery must safely allow browser
destruction too. Six additional open/close cycles assert stable `/proc/self/fd`
counts after three warmup cycles. A final case pauses the helper while a resource
response is outstanding and destroys the browser. Every measured browser close
must finish within three seconds; the final report prints the maximum close time
and full runtime duration. Pass `--protocol-only` to skip WebKit runtime scenarios.

Diagnostic-only pipe EOF and duplicate resource replies have no real-WebKit fault
injection. Diagnostic EOF cleanup is covered by inspection, while duplicate and
obsolete resource IDs are exercised directly against the production registry.

These tests validate the shared Linux WebView implementation. Plugin hosting,
runtime-only installation, and UI performance require their separate checks.

## Ordered dispatch candidate

`linux-webview-dispatch-tests.cpp` is portable C++17 and exercises the production
queue's byte/count limits, FIFO order, cancellation, two-instance isolation and
100,000 deliveries across real producer/consumer threads, including rearming an
empty queue. Compile with assertions enabled (no `NDEBUG`).

The WebKit executable additionally sends 256 native events in one JavaScript
burst and verifies order before subsequent evaluation results. Queued mode
requires native listeners to run on the message thread. Existing lifecycle cases
destroy the browser from an evaluation callback and with a resource response
pending. Set
`JUCE_WEBVIEW_LEGACY_DISPATCH=1` to repeat the other lifecycle tests using the old
reader/GUI-lock path. `JUCE_WEBVIEW_DISPATCH_DIAGNOSTICS=1` emits one summary per
closed browser. These summaries do not measure browser rendering cadence.
