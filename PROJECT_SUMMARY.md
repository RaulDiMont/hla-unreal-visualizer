# HLA Visualizer — Project Summary

Technical overview of what has been built, how the system works, and what remains pending.
For setup and run instructions see [README.md](README.md).

---

## Objective

Connect a distributed C++ HLA aircraft simulation (running in WSL2) to Unreal Engine 5.5 so that
an Airbus A320 on final approach to Madrid-Barajas is rendered in real time over georeferenced terrain.

The full stack spans two operating systems and four HLA federates:

```
WSL2 (Linux)                              Windows (Unreal Engine 5.5)
─────────────────────────────────         ─────────────────────────────────
AircraftFederate  ─┐                      AUnrealFederateActor
  JSBSim A320       │   HLA 1516e            ├── BeginPlay → AsyncTask (background)
  publishes         │   over TCP             │       connect / join / subscribe
  Lat/Lon/Alt       ├─► rtinode ──────────► │       up to 30 retries × 2 s
                    │   port 14321          │
RadarFederate     ─┘                        ├── Tick (GameThread)
  publishes                                 │     evokeCallback(0.0)
  Distance/Bearing/IsInRange                │       → FHLAAmbassador callbacks
                                            │         → OnAircraftStateReceived
                                            │         → OnRadarContactReceived
                                            │     GlobeAnchor.MoveToLongLatHeight
                                            └── ARadarVisualizationActor
MonitorFederate                                   procedural 10 km ring at LEMD
  (console only, WSL2)
```

---

## Implemented features

### Phase 1 — OpenRTI Windows connectivity
- OpenRTI 0.10.0 compiled for Windows (MSVC) and integrated as a Unreal ThirdParty module
- WSL2 ↔ Windows HLA transport working over `rti://` (TCP, port 14321)
- HLA simulator federates switched from `thread://` to `rti://` transport
- **Critical finding:** `-DCMAKE_CXX_STANDARD=17` must be passed to CMake. Without it, CMake + VS2022 defaults to C++14 and `createRTIambassador()` returns `std::auto_ptr` instead of `std::unique_ptr`, causing `LNK2019` when Unreal links against it.
- **Critical finding:** OpenRTI 0.10.0 uses `rti://` as the TCP scheme. `tcp://` is not recognized.

### Phase 2 — Cesium for Unreal
- CesiumForUnreal plugin enabled in the project
- `ACesiumGeoreference` origin set at LEMD threshold (40.4939°N, 3.5672°W, 600 m)
- Cesium World Terrain + Bing Maps Aerial with Labels configured for Madrid-Barajas area
- **Critical finding:** In Cesium for Unreal v2.x the ion access token is set via the **Window → Cesium panel**, not Project Settings. `Content/CesiumSettings/` is gitignored to prevent tokens from being committed.

### Phase 3 — OpenRTI ThirdParty module
- Headers tracked in `ThirdParty/OpenRTI/include/` (git)
- Compiled binaries (`.lib`, `.dll`) gitignored — must be built per machine
- `HLAVisualizer.Build.cs` wires up include paths, import libs, delay-load DLLs, and runtime staging
- **Critical finding:** The correct link targets are `rti1516e.lib` + `fedtime1516e.lib`, not `OpenRTI.lib` directly. The include path must point to `include/rti1516e/` for `#include <RTI/RTI1516.h>` to resolve.

### Phase 4 — UnrealFederate Actor
- `AUnrealFederateActor`: subscribes to `Aircraft` object class (Latitude, Longitude, Altitude); owns the `RTIambassador` and the `FHLAAmbassador`; pumps HLA callbacks from `Tick`
- Background `AsyncTask` runs the blocking connect / join / subscribe sequence with up to 30 retries × 2 s. Cancellable via a shared atomic flag so `EndPlay` aborts the loop on the next sleep boundary.
- `FHLAAmbassador`: HLA 1516e NullFederateAmbassador callbacks, caches object/attribute handles, dispatches each callback directly to the actor on the GameThread (guarded by `IsInGameThread()`)
- Position updates drive `UCesiumGlobeAnchorComponent::MoveToLongitudeLatitudeHeight()`
- Altitude conversion: JSBSim feet → Cesium metres (× 0.3048) happens once on the GameThread

> **Architecture note.** An earlier iteration of Phase 4 used a dedicated `FRunnable` thread plus two SPSC `TQueue`s to bridge HLA callbacks to the GameThread. With JSBSim publishing at 1 Hz and per-message decode work in the microsecond range, the multithreading complexity was over-engineering. The current design keeps a background thread only for what truly must be off the GameThread (the blocking connect/retry loop) and runs the HLA pump itself directly in `Tick` via `evokeCallback(0.0)`. See the [Threading model](#threading-model) section below for details.

### Phase 5 — Radar visualization
- `ARadarVisualizationActor`: procedural flat ring around LEMD, 10 km radius, 128 segments
- `AUnrealFederateActor` also subscribes to `RadarContact` (Distance, Bearing, IsInRange)
- `OnRadarRangeChanged` delegate fires on `bIsInRange` transitions; `SetOverlayMaterial()` switches `M_InRange` overlay on/off
- On-screen status messages: "Waiting for simulation…", "Simulation running", "In radar's range", "Simulation ended"
- Radar actor anchored to LEMD via `UCesiumGlobeAnchorComponent`

---

## Architecture details

### Threading model

The simulation publishes at 1 Hz and per-message decode work is trivial (three `HLAfloat64BE`
values), so almost the entire HLA flow can live on the GameThread:

```
BeginPlay (GameThread)
  │  caches RTIAddress / FederationName, constructs FHLAAmbassador
  │  ConnectionState = Connecting
  ↓
AsyncTask (background — AnyBackgroundThreadNormalTask)
  │  for i in 1..30:
  │    if ShouldStopFlag → return
  │    try: createRTIambassador → connect → joinFederationExecution → subscribe
  │    on success: break
  │    on failure: sleep(2s), retry
  │  AsyncTask(GameThread): hand RTIambassador to actor, ConnectionState = Connected

Tick (GameThread, every frame)
  │  if Connected: evokeCallback(0.0)            non-blocking poll
  │     → FHLAAmbassador::reflectAttributeValues fires synchronously
  │       → OnAircraftStateReceived(State)       direct UObject mutation, no queue
  │       → OnRadarContactReceived(Contact)
  │     → FHLAAmbassador::removeObjectInstance / connectionLost
  │       → OnFederationLost()                   idempotent state transition
  │  interpolate PositionBuffer via Lagrange and MoveToLongitudeLatitudeHeight
```

There are **no SPSC queues and no marshaling between threads during normal operation**: the
HLA callbacks fire on the GameThread because that is where `evokeCallback` is called from.
The single background thread exists only for the 30-retry connect loop, which is genuinely
unable to run on the GameThread (a single failed `connect()` can block for seconds, and 60 s
of retries would freeze the editor).

**Lifetime safety.** The connect AsyncTask captures three things:
- `TWeakObjectPtr<AUnrealFederateActor>` — never dereferenced from the background thread; only
  used inside the marshal-back-to-GameThread lambda for safe access.
- `TSharedPtr<FHLAAmbassador>` and `std::shared_ptr<rti1516e::RTIambassador>` (after success) —
  shared ownership so the resources stay alive even if the actor is destroyed mid-connect.
- `TSharedPtr<std::atomic<bool>> ShouldStopFlag` — `EndPlay` flips it to `true`; the worker
  checks it before and after each `Sleep` to abort early without dereferencing the actor.

The `FHLAAmbassador::reflectAttributeValues` dispatch is wrapped in `if (IsInGameThread())`
with an `AsyncTask(ENamedThreads::GameThread, ...)` fallback. In normal operation the guard
takes the fast path; the fallback covers the theoretical case of OpenRTI raising a callback
during `connect()` / `joinFederationExecution()` on the worker thread.

### Shutdown sequence

```
EndPlay() [GameThread]
  → ShouldStopFlag->store(true)        signals the connect AsyncTask to abort on next sleep
  → ConnectionState = Stopped          guards Tick from calling evokeCallback
  → RtiAmbassador.reset()              drops local ref; worker may still hold one
  → Ambassador.Reset()                 same
  → unbind delegates, clear on-screen messages
```

`EndPlay` does not block waiting for the worker to finish. If the worker is mid-`connect()`
when `EndPlay` runs, the captured `shared_ptr`s keep the ambassador and any partial RTI alive
on the worker thread; when `connect()` finally returns or throws, the worker checks
`ShouldStopFlag` and exits, releasing its references.

Resign and disconnect are skipped intentionally. If the WSL2 simulation ended first,
OpenRTI holds a dangling pointer to the destroyed federation and calling `resignFederationExecution()`
causes an access violation inside OpenRTI that cannot be caught via C++ exceptions. The clean
path when the simulator ends first is `FHLAAmbassador::connectionLost()` → `OnFederationLost()`
on the GameThread, which flips `ConnectionState` to `Stopped` so `Tick` stops pumping before
OpenRTI tears down.

### Coordinate conversion

```
JSBSim (HLA)                 Cesium for Unreal
─────────────────────────    ─────────────────────────────────────────────
Latitude  [degrees WGS84] →  MoveToLongitudeLatitudeHeight(Longitude, Latitude, AltM)
Longitude [degrees WGS84] →  (note argument order: Lon first, then Lat)
Altitude  [feet MSL]      →  Altitude * 0.3048  (feet → metres)
```

### Project Settings integration

`UHLASettings` extends `UDeveloperSettings` so the RTI address and federation name appear in
**Edit → Project Settings → Plugins → HLA Visualizer** and are stored in `Config/DefaultGame.ini`.
No source edits are needed when the WSL2 IP changes between sessions.

---

## Class reference

| Class | File | Role |
|---|---|---|
| `AUnrealFederateActor` | `UnrealFederate/AUnrealFederateActor.h/.cpp` | Main Actor: owns the `RTIambassador` and ambassador, runs the connect `AsyncTask`, pumps `evokeCallback` from `Tick`, drives the A320 mesh via Cesium |
| `FHLAAmbassador` | `UnrealFederate/FHLAAmbassador.h/.cpp` | `NullFederateAmbassador`: decodes attributes, dispatches to the actor on the GameThread (guarded by `IsInGameThread()`) |
| `ARadarVisualizationActor` | `Radar/ARadarVisualizationActor.h/.cpp` | Procedural ring mesh anchored to LEMD |
| `UHLASettings` | `Settings/UHLASettings.h/.cpp` | Project Settings: RTI address, federation name |
| `FAircraftState` | `Types/FAircraftState.h` | POD struct: Latitude, Longitude, Altitude, Timestamp |
| `FRadarContact` | `Types/FRadarContact.h` | POD struct: Distance, Bearing, IsInRange |

---

## Third-party dependencies

| Dependency | Version | Purpose | Notes |
|---|---|---|---|
| OpenRTI | 0.10.0 | HLA 1516e RTI | Must be compiled for Windows (MSVC). Headers in git; binaries gitignored. |
| Cesium for Unreal | Latest for UE 5.5 | WGS84 georeferencing + terrain streaming | Installed via Epic Launcher. Requires Cesium ion account. |
| JSBSim | (inside hla-simulator) | Flight dynamics model for A320 | Runs in WSL2 only. |

---

## Known technical debt

| Issue | Status | Notes |
|---|---|---|
| No HLA Time Management | Accepted for MVP | Wall-clock pacing; TAR/TAG deferred |
| A320 altitude model unvalidated | Known JSBSim BETA issue | Altitude loss visible but not a blocker |
| Radar circle is flat | Deferred post-MVP | Terrain conformance requires Cesium landscape projection |
| No reconnect logic | Deferred | Restarting the simulator requires a full Play stop/restart in Unreal |
| resign/disconnect skipped on Stop() | Accepted workaround | OpenRTI crash on dead federation; connectionLost() handles the clean path |

---

## Implementation phases status

| Phase | Status | Description |
|---|---|---|
| 1 | Complete | OpenRTI Windows build + WSL2 ↔ Windows HLA connectivity |
| 2 | Complete | Cesium for Unreal + Madrid-Barajas terrain |
| 3 | Complete | OpenRTI as Unreal ThirdParty module |
| 4 | Complete | UnrealFederate Actor — HLA subscriber + A320 mesh positioning |
| 5 | Complete | Radar visualization — 10 km range circle + `SetOverlayMaterial()` switching + on-screen status UI |
| 6 | Pending | Reconnect logic — detect federation restart, re-run the connect + subscribe sequence without requiring a Play stop/restart in the Editor |
| 7 | Pending | Clean HLA shutdown — call `resignFederationExecution()` + `disconnect()` from `EndPlay` / `OnFederationLost()` once the OpenRTI crash-on-dead-federation issue is resolved upstream or worked around |
| 9 | Pending | Terrain-conforming radar circle — project ring vertices onto Cesium terrain height instead of a flat plane at fixed altitude |
| 10 | Pending | HLA Time Management — add TAR/TAG time advance requests to synchronize the UnrealFederate with the simulation clock instead of using wall-clock pacing |