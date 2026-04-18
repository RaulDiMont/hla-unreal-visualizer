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

    // Bind handlers before the HLA thread can start firing events.
    OnRadarRangeChanged.AddDynamic(this, &AUnrealFederateActor::HandleRadarRangeChanged);
    OnSimulationStarted.AddDynamic(this, &AUnrealFederateActor::HandleSimulationStarted);
    OnSimulationEnded.AddDynamic(this,   &AUnrealFederateActor::HandleSimulationEnded);

    // Show initial status — same key (100) is reused by all status messages.
    if (GEngine)
    {
        GEngine->AddOnScreenDebugMessage(100, 99999.f, FColor::Yellow, TEXT("Waiting for simulation..."));
    }

    // Pass a weak-captured lambda so the HLA thread can signal GameThread on connection loss.
    TWeakObjectPtr<AUnrealFederateActor> WeakThis(this);
    HLARunnable = new FHLAFederateRunnable(
        &AircraftStateQueue, &RadarContactQueue,
        Settings->RTIAddress, Settings->FederationName,
        [WeakThis]()
        {
            if (WeakThis.IsValid())
            {
                WeakThis->OnSimulationEnded.Broadcast();
            }
        });
    HLAThread = FRunnableThread::Create(HLARunnable, TEXT("HLAFederateThread"), 0, TPri_Normal);

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

    // Fire OnSimulationStarted once on the first received state.
    if (!bHasReceivedData && PositionBuffer.Num() > 0)
    {
        bHasReceivedData = true;
        OnSimulationStarted.Broadcast();
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
    if (AircraftMesh)
    {
        AircraftMesh->SetOverlayMaterial(bInRange ? M_InRange : nullptr);
    }

    if (GEngine)
    {
        if (bInRange)
            GEngine->AddOnScreenDebugMessage(101, 99999.f, FColor::Green, TEXT("In radar's range"));
        else
            GEngine->AddOnScreenDebugMessage(101, 99999.f, FColor::White,  TEXT("Out of radar's range"));
    }
}

void AUnrealFederateActor::HandleSimulationStarted()
{
    if (GEngine)
    {
        GEngine->AddOnScreenDebugMessage(100, 99999.f, FColor::Green, TEXT("Simulation running"));
        GEngine->AddOnScreenDebugMessage(101, 99999.f, FColor::White, TEXT("Out of radar's range"));
    }
}

void AUnrealFederateActor::HandleSimulationEnded()
{
    if (AircraftMesh)
    {
        AircraftMesh->SetOverlayMaterial(nullptr);
    }

    if (GEngine)
    {
        GEngine->AddOnScreenDebugMessage(100, 99999.f, FColor::Red,  TEXT("Simulation ended"));
        GEngine->RemoveOnScreenDebugMessage(101);
    }
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
    OnSimulationStarted.RemoveDynamic(this, &AUnrealFederateActor::HandleSimulationStarted);
    OnSimulationEnded.RemoveDynamic(this,   &AUnrealFederateActor::HandleSimulationEnded);

    if (GEngine)
    {
        GEngine->RemoveOnScreenDebugMessage(100);
    }

    Super::EndPlay(EndPlayReason);
}