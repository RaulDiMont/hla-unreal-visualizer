#include "AUnrealFederateActor.h"
#include "FHLAFederateRunnable.h"
#include "HAL/RunnableThread.h"
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

    // Drain the aircraft queue, keeping only the latest state to avoid position lag.
    FAircraftState State;
    bool bHadAircraftUpdate = false;
    while (AircraftStateQueue.Dequeue(State))
    {
        bHadAircraftUpdate = true;
    }

    if (bHadAircraftUpdate && GlobeAnchor)
    {
        // Cesium expects (Longitude, Latitude, Height in metres).
        // JSBSim publishes altitude in feet — convert here, once, on the GameThread.
        GlobeAnchor->MoveToLongitudeLatitudeHeight(
            FVector(State.Longitude, State.Latitude, State.Altitude * 0.3048));
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