# SampVoice v4 control component for open.mp

This is the open.mp replacement for the legacy `sampvoice.so` control plug-in.
It preserves the v4 client protocol, but uses only the supported open.mp SDK:
no `CNetGame`, YSF offsets, RakNet vtable hooks, or server-memory patches.

The component accepts the v4 handshake through RPC 25, opens the local control
channel expected by `sampvoice.out`, and provides 20-metre proximity voice on
the Z key. It also exposes MANIKULAR CEF settings and private two-player phone
calls. Phone calls use a non-spatial stream and switch both participants away
from proximity transmission until the call ends.

Pawn natives:

```pawn
native MV_RequestVoiceSettings(playerid);
native MV_SetVoiceOption(playerid, option, value);
native MV_SetVoiceBlacklist(playerid, const name[], bool:add);
native bool:MV_IsVoiceReady(playerid);
native bool:MV_StartPhoneCall(playerid, targetid);
native bool:MV_StopPhoneCall(playerid);
```

`MV_StartPhoneCall` succeeds only when both players completed the voice
handshake and neither is already in another voice call. `MV_StopPhoneCall` may
be called for either participant and restores proximity transmission for both.

Configure a build with the SDK and the `open.mp-network` headers that match the
target server:

```sh
cmake -S omp-control -B build/omp-control \
  -DOPENMP_SDK_ROOT=/path/to/open.mp/SDK \
  -DOPENMP_NETWORK_INCLUDE=/path/to/open.mp/Shared/Network
cmake --build build/omp-control
ctest --test-dir build/omp-control
```
