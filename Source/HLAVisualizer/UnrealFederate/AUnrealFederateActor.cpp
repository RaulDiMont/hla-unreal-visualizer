#include "AUnrealFederateActor.h"
#include "FHLAFederateRunnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/PlatformTime.h"
#include "CesiumGlobeAnchorComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Settings/UHLASettings.h"

AUnrealFederateActor::AUnrealFederateActor()
{
    PrimaryActorTick.bCanEverTick = true;

    AircraftMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("AircraftMesh"));
    RootComponent = AircraftMesh;

    GlobeAnchor = CreateDefaultSubobject<UCesiumGlobeAnchorComponent>(TEXT("GlobeAnchor"));
}

void AUnrealFederateActor::BeginPlay()
{
    Super::BeginPlay();

    const UHLASettings* Settings = GetDefault<UHLASettings>();
    UE_LOG(LogTemp, Log, TEXT("UnrealFederate: RTI address = '%s', federation = '%s'"),
           *Settings->RTIAddress, *Settings->FederationName);

    // Bind the material-switch handler before the HLA thread can start firing events.
    OnRadarRangeChanged.AddDynamic(this, &AUnrealFederateActor::HandleRadarRangeChanged);

    HLARunnable = new FHLAFederateRunnable(
        &AircraftStateQueue, &RadarContactQueue,
        Settings->RTIAddress, Settings->FederationName);
    HLAThread   = FRunnableThread::Create(HLARunnable, TEXT("HLAFederateThread"),
                                          0, TPri_Normal);

    if (!HLAThread)
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealFederate: failed to create HLA thread."));
    }
}

void AUnrealFederateActor::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    // Drain new states into the interpolation buffer, keeping the last 3.
    {
        FAircraftState State;
        while (AircraftStateQueue.Dequeue(State))
        {
            PositionBuffer.Add(State);
        }
        if (PositionBuffer.Num() > 3)
        {
            PositionBuffer.RemoveAt(0, PositionBuffer.Num() - 3);
        }
    }

    // Interpolate position at RenderTime = Now - InterpolationDelaySeconds.
    if (GlobeAnchor && PositionBuffer.Num() >= 2)
    {
        const double RenderTime = FPlatformTime::Seconds() - InterpolationDelaySeconds;

        FAircraftState Out;

        if (PositionBuffer.Num() == 3)
        {
            // Quadratic Lagrange interpolation — curve passes exactly through the 3 HLA states.
            const FAircraftState& P0 = PositionBuffer[0];
            const FAircraftState& P1 = PositionBuffer[1];
            const FAircraftState& P2 = PositionBuffer[2];

            const double T  = FMath::Clamp(RenderTime, P0.Timestamp, P2.Timestamp);
            const double T0 = P0.Timestamp, T1 = P1.Timestamp, T2 = P2.Timestamp;

            const double L0 = (T - T1) * (T - T2) / ((T0 - T1) * (T0 - T2));
            const double L1 = (T - T0) * (T - T2) / ((T1 - T0) * (T1 - T2));
            const double L2 = (T - T0) * (T - T1) / ((T2 - T0) * (T2 - T1));

            Out.Latitude  = P0.Latitude  * L0 + P1.Latitude  * L1 + P2.Latitude  * L2;
            Out.Longitude = P0.Longitude * L0 + P1.Longitude * L1 + P2.Longitude * L2;
            Out.Altitude  = P0.Altitude  * L0 + P1.Altitude  * L1 + P2.Altitude  * L2;
        }
        else
        {
            // Linear lerp — fallback while the buffer fills during the first 2 updates.
            const FAircraftState& A = PositionBuffer[0];
            const FAircraftState& B = PositionBuffer[1];
            const double Range = B.Timestamp - A.Timestamp;
            const double Alpha = Range > 0.0 ? FMath::Clamp((RenderTime - A.Timestamp) / Range, 0.0, 1.0) : 0.0;

            Out.Latitude  = FMath::Lerp(A.Latitude,  B.Latitude,  Alpha);
            Out.Longitude = FMath::Lerp(A.Longitude, B.Longitude, Alpha);
            Out.Altitude  = FMath::Lerp(A.Altitude,  B.Altitude,  Alpha);
        }

        // Cesium expects (Longitude, Latitude, Height in metres).
        // JSBSim publishes altitude in feet — convert here, once, on the GameThread.
        GlobeAnchor->MoveToLongitudeLatitudeHeight(
            FVector(Out.Longitude, Out.Latitude, Out.Altitude * 0.3048));
    }

    // Drain the radar queue, keeping only the latest contact.
    FRadarContact Contact;
    bool bHadRadarUpdate = false;
    while (RadarContactQueue.Dequeue(Contact))
    {
        bHadRadarUpdate = true;
    }

    // Only broadcast when the state actually transitions — avoids SetMaterial every frame.
    if (bHadRadarUpdate && Contact.IsInRange != bIsInRange)
    {
        bIsInRange = Contact.IsInRange;
        OnRadarRangeChanged.Broadcast(bIsInRange);
    }
}

void AUnrealFederateActor::HandleRadarRangeChanged(bool bInRange)
{
    if (!AircraftMesh) return;

    // Overlay sits on top of all 50 livery material slots without replacing them.
    // Passing nullptr clears the overlay and restores the original mesh appearance.
    AircraftMesh->SetOverlayMaterial(bInRange ? M_InRange : nullptr);
}

void AUnrealFederateActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (HLAThread)
    {
        HLARunnable->Stop();        // signal the pump loop to exit
        HLAThread->Kill(true);      // wait for the thread to finish (blocks up to ~0.1s)
        delete HLAThread;
        HLAThread = nullptr;
    }

    delete HLARunnable;
    HLARunnable = nullptr;

    // Remove after the HLA thread is fully stopped — no Broadcast can fire past this point.
    OnRadarRangeChanged.RemoveDynamic(this, &AUnrealFederateActor::HandleRadarRangeChanged);

    Super::EndPlay(EndPlayReason);
}