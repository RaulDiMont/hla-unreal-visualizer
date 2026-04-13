#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Containers/Queue.h"
#include "Types/FAircraftState.h"
#include "Types/FRadarContact.h"
#include "AUnrealFederateActor.generated.h"

class UCesiumGlobeAnchorComponent;
class UStaticMeshComponent;
class FHLAFederateRunnable;

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

private:
    // SPSC queues: HLA thread produces, GameThread (Tick) consumes.
    TQueue<FAircraftState, EQueueMode::Spsc> AircraftStateQueue;
    TQueue<FRadarContact,  EQueueMode::Spsc> RadarContactQueue;

    FHLAFederateRunnable* HLARunnable = nullptr;
    FRunnableThread*      HLAThread   = nullptr;

    // Tracks the latest radar state between frames.
    // Used in Phase 4.5 for A320 material switching (IsInRange highlight).
    bool bIsInRange = false;
};