#include "AUnrealFederateActor.h"
#include "FHLAFederateRunnable.h"
#include "HAL/RunnableThread.h"
#include "CesiumGlobeAnchorComponent.h"
#include "Components/StaticMeshComponent.h"
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
    while (RadarContactQueue.Dequeue(Contact))
    {
        bIsInRange = Contact.IsInRange;
    }
    // Phase 5: material switching on bIsInRange goes here.
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

    Super::EndPlay(EndPlayReason);
}