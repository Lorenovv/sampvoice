# SampVoice v4 control component for open.mp

This is the in-progress replacement for the legacy `sampvoice.so` control
plug-in. It preserves the v4 client protocol, but must use only the supported
open.mp SDK: no `CNetGame`, YSF offsets, RakNet vtable hooks, or server-memory
patches.

The component now accepts the v4 handshake through RPC 25, opens the local
control channel expected by `sampvoice.out`, and returns `ClientInitialize`
without any SA:MP memory hooks. It still has no stream/Pawn API, so it is **not
ready for deployment**. The next milestone creates proximity streams and adds
the compatibility natives; only after a two-client test will it be integrated.

Configure a build with the SDK and the `open.mp-network` headers that match the
target server:

```sh
cmake -S omp-control -B build/omp-control \
  -DOPENMP_SDK_ROOT=/path/to/open.mp/SDK \
  -DOPENMP_NETWORK_INCLUDE=/path/to/open.mp/Shared/Network
cmake --build build/omp-control
ctest --test-dir build/omp-control
```
