Minimal stand-ins for the few libobs headers `src/http-client.c` includes, so the S2 harness can compile the real
`src/http-client.c` unmodified without libobs. `shim.c` implements them on plain libc. Test-only; never shipped.
