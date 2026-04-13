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
  JSBSim A320       │   HLA 1516e            ├── FHLAFederateRunnable (thread)
  publishes         │   over TCP             │     └── FHLAAmbassador
  Lat/Lon/Alt       ├─► rtinode ──────────► │           reflectAttributeValues()
                    │   port 14321          │     → TQueue<FAircraftState>
RadarFederate     ─┘                        │
  publishes                                 ├── Tick() [GameThread]
  Distance/Bearing/IsInRange                │     GlobeAnchor.MoveToLongLatHeight()
                                            └── ARadarVisualizationActor
MonitorFederate                                   procedural 60 km ring at LEMD
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
- `AUnrealFederateActor`: subscribes to `Aircraft` object class (Latitude, Longitude, Altitude)
- `FHLAFederateRunnable`: dedicated HLA thread with async connect + retry loop (30 retries, 2s interval)
- `FHLAAmbassador`: HLA 1516e NullFederateAmbassador callbacks, caches object/attribute handles
- Position updates drive `UCesiumGlobeAnchorComponent::MoveToLongitudeLatitudeHeight()`
- Altitude conversion: JSBSim feet → Cesium metres (× 0.3048) happens once on the GameThread

### Phase 5 — Radar visualization
- `ARadarVisualizationActor`: procedural flat ring around LEMD, 60 km radius, 128 segments
- `AUnrealFederateActor` also subscribes to `RadarContact` (Distance, Bearing, IsInRange)
- `bIsInRange` state tracked per frame; material switching hook ready in `Tick()` 
- Radar actor anchored to LEMD via `UCesiumGlobeAnchorComponent`

---

## Architecture details

### Threading model

HLA callbacks are not safe to call from the Unreal GameThread, and Unreal Actor/Component
updates are not safe to call from any other thread. The bridge uses lock-free SPSC queues:

```
FHLAFederateRunnable (HLA thread)
  │  FHLAAmbassador::reflectAttributeValues()
  │  → TQueue<FAircraftState>.Enqueue()   [SPSC, lock-free]
  │  → TQueue<FRadarContact>.Enqueue()    [SPSC, lock-free]
  │
AUnrealFederateActor::Tick() (GameThread)
  │  → Dequeue all pending states, keep only latest (avoids position lag accumulation)
  │  → UCesiumGlobeAnchorComponent::MoveToLongitudeLatitudeHeight()
  │  → bIsInRange update for material switching
```

`TQueue<T, EQueueMode::Spsc>` is Unreal's built-in lock-free single-producer single-consumer queue.
It allows exactly one producer thread and one consumer thread — which matches the HLA thread + GameThread pattern perfectly.

### Async connect with retry

The HLA thread (`FHLAFederateRunnable::Run()`) attempts to connect before entering the event pump.
It retries up to 30 times with a 2-second interval, logging progress each attempt.
This means you can press Play in the Unreal Editor before `rtinode` or `aircraft_simulator` are fully started.

### Shutdown sequence

```
EndPlay() [GameThread]
  → HLARunnable->Stop()           sets bRunning = false (atomic)
  → HLAThread->Kill(true)         waits for Run() to exit (evokeCallback blocks max 0.1s)
  → delete HLAThread, HLARunnable
```

`Stop()` intentionally skips `resignFederationExecution()` and `disconnect()`. If the WSL2 simulation
has already ended, OpenRTI holds a dangling pointer to the destroyed federation and calling resign
causes an access violation inside OpenRTI that cannot be caught via C++ exceptions.
The clean shutdown path when the simulation ends first is via `FHLAAmbassador::connectionLost()`,
which calls `SignalConnectionLost()` and exits the `Run()` loop before OpenRTI tears down.

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
| `AUnrealFederateActor` | `UnrealFederate/AUnrealFederateActor.h/.cpp` | Main Actor: owns HLA thread, drives A320 mesh via Cesium |
| `FHLAFederateRunnable` | `UnrealFederate/FHLAFederateRunnable.h/.cpp` | FRunnable: connect loop, evokeCallback pump, Stop() |
| `FHLAAmbassador` | `UnrealFederate/FHLAAmbassador.h/.cpp` | NullFederateAmbassador: reflectAttributeValues callbacks |
| `ARadarVisualizationActor` | `Radar/ARadarVisualizationActor.h/.cpp` | Procedural ring mesh anchored to LEMD |
| `UHLASettings` | `Settings/UHLASettings.h/.cpp` | Project Settings: RTI address, federation name |
| `FAircraftState` | `Types/FAircraftState.h` | POD struct: Latitude, Longitude, Altitude |
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
| 5 | Complete | Radar visualization — range circle + IsInRange state tracking |
| 6 | Pending | A320 material highlight — create `M_A320_Normal` and `M_A320_InRange` material assets, wire `bIsInRange` to `AircraftMesh->SetMaterial()` in `Tick()` (hook already in place) |
| 7 | Pending | Reconnect logic — detect federation restart, re-run the connect + subscribe sequence without requiring a Play stop/restart in the Editor |
| 8 | Pending | Clean HLA shutdown — implement `resignFederationExecution()` + `disconnect()` in `FHLAFederateRunnable::Stop()` once the OpenRTI crash-on-dead-federation issue is resolved upstream or worked around |
| 9 | Pending | Terrain-conforming radar circle — project ring vertices onto Cesium terrain height instead of a flat plane at fixed altitude |
| 10 | Pending | HLA Time Management — add TAR/TAG time advance requests to synchronize the UnrealFederate with the simulation clock instead of using wall-clock pacing |