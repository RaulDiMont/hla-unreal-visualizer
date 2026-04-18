#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Containers/Queue.h"
#include "Materials/MaterialInterface.h"
#include "Types/FAircraftState.h"
#include "Types/FRadarContact.h"
#include "AUnrealFederateActor.generated.h"

class UCesiumGlobeAnchorComponent;
class UStaticMeshComponent;
class FHLAFederateRunnable;

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
//   FHLAFederateRunnable (dedicated thread) → pushes to TQueues (SPSC, lock-free)
//   Tick() (GameThread)                     → drains TQueues, updates GlobeAnchor
//
// All Unreal Actor/Component calls stay on the GameThread — never inside HLA callbacks.
UCLASS()
class HLAVISUALIZER_API AUnrealFederateActor : public AActor
{
    GENERATED_BODY()

public:
    AUnrealFederateActor();

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
    // Circular buffer of the last 3 received states — used for Lagrange quadratic interpolation.
    TArray<FAircraftState> PositionBuffer;

    // SPSC queues: HLA thread produces, GameThread (Tick) consumes.
    TQueue<FAircraftState, EQueueMode::Spsc> AircraftStateQueue;
    TQueue<FRadarContact,  EQueueMode::Spsc> RadarContactQueue;

    FHLAFederateRunnable* HLARunnable = nullptr;
    FRunnableThread*      HLAThread   = nullptr;

    // Tracks the latest radar state between frames to detect transitions.
    bool bIsInRange = false;

    // True after the first FAircraftState is received — guards OnSimulationStarted broadcast.
    bool bHasReceivedData = false;

    // Bound to OnRadarRangeChanged in BeginPlay — swaps AircraftMesh overlay material.
    UFUNCTION()
    void HandleRadarRangeChanged(bool bInRange);

    UFUNCTION()
    void HandleSimulationStarted();

    UFUNCTION()
    void HandleSimulationEnded();
};