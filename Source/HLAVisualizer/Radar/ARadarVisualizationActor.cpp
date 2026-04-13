#include "ARadarVisualizationActor.h"
#include "ProceduralMeshComponent.h"
#include "CesiumGlobeAnchorComponent.h"
#include "Materials/MaterialInterface.h"

ARadarVisualizationActor::ARadarVisualizationActor()
{
    PrimaryActorTick.bCanEverTick = false;

    RangeCircle = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("RangeCircle"));
    RootComponent = RangeCircle;

    RadarAnchor = CreateDefaultSubobject<UCesiumGlobeAnchorComponent>(TEXT("RadarAnchor"));
}

void ARadarVisualizationActor::BeginPlay()
{
    Super::BeginPlay();

    // Fix the actor at the LEMD threshold position on the WGS84 globe.
    // Longitude, Latitude, Height (metres) — Cesium convention.
    // HeightOffsetM lifts the ring above the terrain surface to avoid clipping.
    RadarAnchor->MoveToLongitudeLatitudeHeight(FVector(-3.5672, 40.4939, 620.0f + HeightOffsetM));

    GenerateRangeCircle(RadiusKm, Segments);
}

void ARadarVisualizationActor::GenerateRangeCircle(float InRadiusKm, int32 InSegments)
{
    // Unreal world units are centimetres. Convert km → cm.
    const float OuterRadius = InRadiusKm * 100'000.0f;
    const float InnerRadius = (InRadiusKm - ThicknessKm) * 100'000.0f;

    TArray<FVector>  Vertices;
    TArray<int32>    Triangles;
    TArray<FVector>  Normals;
    TArray<FVector2D> UV0;

    Vertices.Reserve(InSegments * 2);
    Triangles.Reserve(InSegments * 6);
    Normals.Reserve(InSegments * 2);
    UV0.Reserve(InSegments * 2);

    for (int32 i = 0; i < InSegments; ++i)
    {
        const float Angle = (2.0f * PI * i) / InSegments;
        const float CosA  = FMath::Cos(Angle);
        const float SinA  = FMath::Sin(Angle);

        // Outer vertex
        Vertices.Add(FVector(CosA * OuterRadius, SinA * OuterRadius, 0.0f));
        // Inner vertex
        Vertices.Add(FVector(CosA * InnerRadius, SinA * InnerRadius, 0.0f));

        Normals.Add(FVector(0.0f, 0.0f, 1.0f));
        Normals.Add(FVector(0.0f, 0.0f, 1.0f));

        UV0.Add(FVector2D(static_cast<float>(i) / InSegments, 0.0f));
        UV0.Add(FVector2D(static_cast<float>(i) / InSegments, 1.0f));

        // Two triangles per segment, wrapping around at the last segment.
        const int32 Outer0 = i * 2;
        const int32 Inner0 = i * 2 + 1;
        const int32 Outer1 = ((i + 1) % InSegments) * 2;
        const int32 Inner1 = ((i + 1) % InSegments) * 2 + 1;

        // Triangle A — counter-clockwise winding viewed from above (+Z)
        Triangles.Add(Outer0);
        Triangles.Add(Inner0);
        Triangles.Add(Outer1);

        // Triangle B
        Triangles.Add(Inner0);
        Triangles.Add(Inner1);
        Triangles.Add(Outer1);
    }

    RangeCircle->CreateMeshSection(
        0,           // section index
        Vertices,
        Triangles,
        Normals,
        UV0,
        TArray<FColor>(),          // no vertex colours
        TArray<FProcMeshTangent>(), // tangents computed by Unreal from normals
        false);                    // no collision — purely visual

    if (RingMaterial)
    {
        RangeCircle->SetMaterial(0, RingMaterial);
    }
}
