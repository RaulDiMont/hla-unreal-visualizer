#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "ARadarVisualizationActor.generated.h"

class UProceduralMeshComponent;
class UCesiumGlobeAnchorComponent;

// Renders a flat horizontal ring around LEMD (Madrid-Barajas) representing the
// RadarFederate's 60 km detection range.
//
// The ring is generated once in BeginPlay and never updated — the radar position
// is fixed. Terrain conformance is deferred to post-MVP (see TECHNICAL_DEBT.md).
UCLASS()
class HLAVISUALIZER_API ARadarVisualizationActor : public AActor
{
    GENERATED_BODY()

public:
    ARadarVisualizationActor();

protected:
    virtual void BeginPlay() override;

    // Generates a flat ring mesh centered on the actor's local origin.
    // RadiusKm:  outer radius in kilometres (should match RadarFederate config — 60 km)
    // Segments:  number of subdivisions around the circle (128 gives a smooth ring)
    void GenerateRangeCircle(float RadiusKm, int32 Segments);

public:
    // Procedural ring mesh — created at runtime, no asset needed.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Radar")
    TObjectPtr<UProceduralMeshComponent> RangeCircle;

    // Anchors the actor to LEMD WGS84 coordinates via Cesium coordinate conversion.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Radar")
    TObjectPtr<UCesiumGlobeAnchorComponent> RadarAnchor;

    // Outer radius of the ring in kilometres. Must match the RadarFederate detection range.
    UPROPERTY(EditAnywhere, Category = "Radar", meta = (ClampMin = "1.0"))
    float RadiusKm = 10.0f;

    // Ring thickness in kilometres (visual only — does not affect radar logic).
    UPROPERTY(EditAnywhere, Category = "Radar", meta = (ClampMin = "0.1"))
    float ThicknessKm = 0.5f;

    // Number of segments around the circle. Higher = smoother ring.
    UPROPERTY(EditAnywhere, Category = "Radar", meta = (ClampMin = "8"))
    int32 Segments = 128;

    // Height above the LEMD WGS84 elevation (620 m) in metres.
    // Increase this if the ring clips through Cesium terrain.
    UPROPERTY(EditAnywhere, Category = "Radar")
    float HeightOffsetM = 200.0f;

    // Material applied to the ring. Assign in the Editor Details panel before Play.
    UPROPERTY(EditAnywhere, Category = "Radar")
    TObjectPtr<UMaterialInterface> RingMaterial;
};
