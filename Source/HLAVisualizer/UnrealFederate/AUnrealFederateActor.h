#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "Templates/SharedPointer.h"
#include "Types/FAircraftState.h"
#include "Types/FRadarContact.h"

#include <atomic>
#include <memory>

#include "AUnrealFederateActor.generated.h"

class UCesiumGlobeAnchorComponent;
class UStaticMeshComponent;
class FHLAAmbassador;

namespace rti1516e
{
    class RTIambassador;
}

// Fired on the GameThread whenever the A320's radar contact state transitions.
// bInRange = true  → aircraft has entered the 60 km radar range.
// bInRange = false → aircraft has left the range (or simulation reset).
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRadarRangeChanged, bool, bInRange);

// Fired once when the first HLA data arrives after pressing Play.
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnSimulationStarted);

// Fired when the RTI connection is lost (simulator stopped or crashed).
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnSimulationEnded);

// Subscribes to AircraftFederate and RadarFederate via HLA and drives the A320 mesh
// over Cesium-georeferenced Madrid terrain.
//
// Threading model:
//   AsyncTask (background) — runs the blocking connect / join / subscribe ONLY.
//                            Cancellable via the shared atomic ShouldStopFlag.
//   Tick (GameThread)      — once Connected, polls evokeCallback(0.0). All HLA
//                            callbacks fire synchronously on the GameThread and
//                            mutate state directly through the OnXxxReceived methods.
//
// All Unreal Actor/Component calls stay on the GameThread.
UCLASS()
class HLAVISUALIZER_API AUnrealFederateActor : public AActor
{
    GENERATED_BODY()

public:
    AUnrealFederateActor();
    virtual ~AUnrealFederateActor();

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:
    virtual void Tick(float DeltaTime) override;

    // Positions this Actor on the WGS84 globe via Cesium coordinate conversion.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HLA|Cesium")
    TObjectPtr<UCesiumGlobeAnchorComponent> GlobeAnchor;

    // A320 placeholder mesh — assign a Static Mesh asset in the Editor before pressing Play.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HLA|Aircraft")
    TObjectPtr<UStaticMeshComponent> AircraftMesh;

    // Fired whenever IsInRange transitions. BlueprintAssignable so Blueprints can also react.
    UPROPERTY(BlueprintAssignable, Category = "HLA|Radar")
    FOnRadarRangeChanged OnRadarRangeChanged;

    UPROPERTY(BlueprintAssignable, Category = "HLA|Simulation")
    FOnSimulationStarted OnSimulationStarted;

    UPROPERTY(BlueprintAssignable, Category = "HLA|Simulation")
    FOnSimulationEnded OnSimulationEnded;

    // Overlay material applied on top of all mesh slots when the A320 is inside radar range.
    // When outside range the overlay is cleared (nullptr) — original livery materials are restored.
    // Assign in the Details panel. Must use Blend Mode: Translucent.
    UPROPERTY(EditAnywhere, Category = "HLA|Materials")
    TObjectPtr<UMaterialInterface> M_InRange;

    // How far behind real time the visualizer renders (seconds).
    // Must be >= 2x the HLA update interval (1 Hz → min 2.0 s) to guarantee 3 buffered states.
    UPROPERTY(EditAnywhere, Category = "HLA|Interpolation", meta = (ClampMin = "0.5"))
    float InterpolationDelaySeconds = 2.0f;

private:
    // FHLAAmbassador dispatches HLA callbacks directly into the actor's private receivers.
    friend class FHLAAmbassador;

    enum class EHLAConnectionState : uint8
    {
        Idle,        // before BeginPlay completes
        Connecting,  // connect AsyncTask in flight
        Connected,   // pump is live in Tick
        Failed,      // gave up after kMaxConnectRetries
        Stopped,     // EndPlay or federation lost
    };

    // Launches the connect AsyncTask. Captures only weak-self + shared resources so
    // the worker can outlive the actor briefly without dangling references.
    void RunConnectionAsync();

    // Drains any pending HLA callbacks. Called every Tick when Connected.
    void PumpHLACallbacks();

    // Receivers — invoked from FHLAAmbassador on the GameThread (guarded by IsInGameThread).
    void OnAircraftStateReceived(const FAircraftState& State);
    void OnRadarContactReceived(const FRadarContact& Contact);
    void OnFederationLost();

    // Bound to OnRadarRangeChanged in BeginPlay — swaps AircraftMesh overlay material.
    UFUNCTION()
    void HandleRadarRangeChanged(bool bInRange);

    UFUNCTION()
    void HandleSimulationStarted();

    UFUNCTION()
    void HandleSimulationEnded();

    // Circular buffer of the last 3 received states — used for Lagrange quadratic interpolation.
    TArray<FAircraftState> PositionBuffer;

    // Tracks the latest radar state between frames to detect transitions.
    bool bIsInRange = false;

    // True after the first FAircraftState is received — guards OnSimulationStarted broadcast.
    bool bHasReceivedData = false;

    // Connection lifecycle. Read on GameThread; written by both GameThread (Tick/EndPlay)
    // and the inner GameThread AsyncTask that the connect worker marshals back to.
    std::atomic<EHLAConnectionState> ConnectionState { EHLAConnectionState::Idle };

    // Shared atomic so the connect AsyncTask can detect cancellation without dereferencing
    // this UObject from a background thread. EndPlay flips it to true; the worker checks it
    // between sleeps and aborts the retry loop early.
    TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> ShouldStopFlag;

    // Owned RTI ambassador. shared_ptr so the connect worker can keep it alive even if
    // the actor is destroyed mid-connect (the worker holds its own copy of the pointer).
    std::shared_ptr<rti1516e::RTIambassador> RtiAmbassador;

    // Shared with the connect worker for the same lifetime reason as RtiAmbassador.
    TSharedPtr<FHLAAmbassador, ESPMode::ThreadSafe> Ambassador;

    // Cached at BeginPlay so the connect worker never reads UObject state from a background thread.
    FString CachedRtiAddress;
    FString CachedFederationName;
};
