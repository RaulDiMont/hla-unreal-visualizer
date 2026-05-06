#include "AUnrealFederateActor.h"
#include "FHLAAmbassador.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"
#include "Async/Async.h"
#include "CesiumGlobeAnchorComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Settings/UHLASettings.h"

THIRD_PARTY_INCLUDES_START
#include <RTI/RTIambassador.h>
#include <RTI/RTIambassadorFactory.h>
#include <RTI/Enums.h>
THIRD_PARTY_INCLUDES_END

namespace
{
    const std::wstring kFederateType = L"UnrealFederate";
    constexpr int   kMaxConnectRetries     = 30;
    constexpr float kConnectRetryIntervalS = 2.0f;
}

AUnrealFederateActor::AUnrealFederateActor()
{
    PrimaryActorTick.bCanEverTick = true;

    AircraftMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("AircraftMesh"));
    RootComponent = AircraftMesh;

    GlobeAnchor = CreateDefaultSubobject<UCesiumGlobeAnchorComponent>(TEXT("GlobeAnchor"));
}

// Defaulted in the .cpp where rti1516e::RTIambassador is a complete type — required for
// std::shared_ptr<rti1516e::RTIambassador> to instantiate its destructor correctly.
AUnrealFederateActor::~AUnrealFederateActor() = default;

void AUnrealFederateActor::BeginPlay()
{
    Super::BeginPlay();

    const UHLASettings* Settings = GetDefault<UHLASettings>();
    CachedRtiAddress     = Settings->RTIAddress;
    CachedFederationName = Settings->FederationName;
    UE_LOG(LogTemp, Log, TEXT("UnrealFederate: RTI address = '%s', federation = '%s'"),
           *CachedRtiAddress, *CachedFederationName);

    // Bind handlers before any HLA callback can fire.
    OnRadarRangeChanged.AddDynamic(this, &AUnrealFederateActor::HandleRadarRangeChanged);
    OnSimulationStarted.AddDynamic(this, &AUnrealFederateActor::HandleSimulationStarted);
    OnSimulationEnded.AddDynamic(this,   &AUnrealFederateActor::HandleSimulationEnded);

    // Show initial status — same key (100) is reused by all status messages.
    if (GEngine)
    {
        GEngine->AddOnScreenDebugMessage(100, 99999.f, FColor::Yellow, TEXT("Waiting for simulation..."));
    }

    ShouldStopFlag = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
    Ambassador     = MakeShared<FHLAAmbassador, ESPMode::ThreadSafe>(TWeakObjectPtr<AUnrealFederateActor>(this));

    ConnectionState.store(EHLAConnectionState::Connecting);
    RunConnectionAsync();
}

void AUnrealFederateActor::RunConnectionAsync()
{
    // Capture-by-value everything the worker will need. WeakThis lets the inner GameThread
    // marshal-back fail safely if the actor is destroyed in the meantime. AmbassadorPtr and
    // StopFlag are shared so the worker keeps them alive even past actor destruction.
    TWeakObjectPtr<AUnrealFederateActor> WeakThis(this);
    auto AmbassadorPtr = Ambassador;
    auto StopFlag      = ShouldStopFlag;
    std::wstring RtiUrlW(*CachedRtiAddress);
    std::wstring FedNameW(*CachedFederationName);

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
        [WeakThis, AmbassadorPtr, StopFlag,
         RtiUrlW = std::move(RtiUrlW), FedNameW = std::move(FedNameW)]()
        {
            std::shared_ptr<rti1516e::RTIambassador> ResultRti;

            for (int i = 0; i < kMaxConnectRetries; ++i)
            {
                if (StopFlag->load()) return;

                if (i > 0)
                {
                    UE_LOG(LogTemp, Log, TEXT("UnrealFederate: waiting for federation... (%d/%d)"), i, kMaxConnectRetries);
                    FPlatformProcess::Sleep(kConnectRetryIntervalS);
                }

                if (StopFlag->load()) return;

                std::unique_ptr<rti1516e::RTIambassador> UniqueRti;
                try
                {
                    rti1516e::RTIambassadorFactory Factory;
                    UniqueRti = Factory.createRTIambassador();

                    UniqueRti->connect(*AmbassadorPtr, rti1516e::HLA_EVOKED, RtiUrlW);
                    UniqueRti->joinFederationExecution(kFederateType, FedNameW);

                    AmbassadorPtr->CacheHandles(*UniqueRti);

                    {
                        rti1516e::ObjectClassHandle AircraftClass = UniqueRti->getObjectClassHandle(L"Aircraft");
                        rti1516e::AttributeHandleSet Attribs;
                        Attribs.insert(UniqueRti->getAttributeHandle(AircraftClass, L"Latitude"));
                        Attribs.insert(UniqueRti->getAttributeHandle(AircraftClass, L"Longitude"));
                        Attribs.insert(UniqueRti->getAttributeHandle(AircraftClass, L"Altitude"));
                        UniqueRti->subscribeObjectClassAttributes(AircraftClass, Attribs);
                    }

                    {
                        rti1516e::ObjectClassHandle RadarClass = UniqueRti->getObjectClassHandle(L"RadarContact");
                        rti1516e::AttributeHandleSet Attribs;
                        Attribs.insert(UniqueRti->getAttributeHandle(RadarClass, L"Distance"));
                        Attribs.insert(UniqueRti->getAttributeHandle(RadarClass, L"Bearing"));
                        Attribs.insert(UniqueRti->getAttributeHandle(RadarClass, L"IsInRange"));
                        UniqueRti->subscribeObjectClassAttributes(RadarClass, Attribs);
                    }

                    UE_LOG(LogTemp, Log, TEXT("UnrealFederate: joined '%s' at %s"),
                           *FString(FedNameW.c_str()), *FString(RtiUrlW.c_str()));

                    // Promote unique → shared so we can hand ownership to the actor on the GameThread.
                    ResultRti = std::shared_ptr<rti1516e::RTIambassador>(std::move(UniqueRti));
                    break;
                }
                catch (...)
                {
                    // Match the prior FHLAFederateRunnable cleanup behaviour.
                    if (UniqueRti)
                    {
                        try { UniqueRti->disconnect(); } catch (...) {}
                        UniqueRti.reset();
                    }
                }
            }

            // Marshal the outcome back to the GameThread for state assignment + delegate broadcasts.
            AsyncTask(ENamedThreads::GameThread, [WeakThis, ResultRti]()
            {
                AUnrealFederateActor* Actor = WeakThis.Get();
                if (!Actor) return;

                // EndPlay may have already flipped Stopped — don't resurrect a dead actor's state.
                if (Actor->ConnectionState.load() == EHLAConnectionState::Stopped) return;

                if (ResultRti)
                {
                    Actor->RtiAmbassador = ResultRti;
                    Actor->ConnectionState.store(EHLAConnectionState::Connected);
                }
                else
                {
                    UE_LOG(LogTemp, Error, TEXT("UnrealFederate: could not join federation after %d retries. Check rtinode and aircraft_simulator are running."), kMaxConnectRetries);
                    Actor->ConnectionState.store(EHLAConnectionState::Failed);
                    Actor->OnSimulationEnded.Broadcast();
                }
            });
        });
}

void AUnrealFederateActor::PumpHLACallbacks()
{
    try
    {
        // Non-blocking: returns immediately if no HLA events are pending.
        // Any callbacks fire synchronously on this thread (GameThread).
        RtiAmbassador->evokeCallback(0.0);
    }
    catch (...)
    {
        // Treat any exception out of evokeCallback as connection loss.
        UE_LOG(LogTemp, Warning, TEXT("UnrealFederate: evokeCallback threw — federation ended"));
        OnFederationLost();
    }
}

void AUnrealFederateActor::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    if (ConnectionState.load() == EHLAConnectionState::Connected && RtiAmbassador)
    {
        PumpHLACallbacks();
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
}

void AUnrealFederateActor::OnAircraftStateReceived(const FAircraftState& State)
{
    PositionBuffer.Add(State);
    if (PositionBuffer.Num() > 3)
    {
        PositionBuffer.RemoveAt(0, PositionBuffer.Num() - 3);
    }

    if (!bHasReceivedData)
    {
        bHasReceivedData = true;
        OnSimulationStarted.Broadcast();
    }
}

void AUnrealFederateActor::OnRadarContactReceived(const FRadarContact& Contact)
{
    // Only broadcast when the state actually transitions — avoids redundant SetMaterial calls.
    if (Contact.IsInRange != bIsInRange)
    {
        bIsInRange = Contact.IsInRange;
        OnRadarRangeChanged.Broadcast(bIsInRange);
    }
}

void AUnrealFederateActor::OnFederationLost()
{
    // Idempotent: removeObjectInstance + connectionLost can both fire during teardown.
    if (ConnectionState.load() == EHLAConnectionState::Stopped)
    {
        return;
    }
    ConnectionState.store(EHLAConnectionState::Stopped);
    OnSimulationEnded.Broadcast();
    // Do NOT call resign/disconnect on RtiAmbassador — same crash workaround as the prior
    // FHLAFederateRunnable::Stop(): if WSL2 already died, OpenRTI holds a dangling federation
    // pointer and any cleanup call causes an access violation inside OpenRTI itself.
    // The RtiAmbassador shared_ptr is released in EndPlay; OpenRTI's destructor handles teardown.
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
    // Signal the connect AsyncTask to abort its retry loop on the next sleep boundary.
    if (ShouldStopFlag.IsValid())
    {
        ShouldStopFlag->store(true);
    }
    ConnectionState.store(EHLAConnectionState::Stopped);

    // Drop our local references. If the connect worker is still mid-connect(), it holds
    // its own shared_ptr copies and the resources stay alive until the lambda exits.
    // No resign/disconnect call — see OnFederationLost for the rationale.
    RtiAmbassador.reset();
    Ambassador.Reset();

    OnRadarRangeChanged.RemoveDynamic(this, &AUnrealFederateActor::HandleRadarRangeChanged);
    OnSimulationStarted.RemoveDynamic(this, &AUnrealFederateActor::HandleSimulationStarted);
    OnSimulationEnded.RemoveDynamic(this,   &AUnrealFederateActor::HandleSimulationEnded);

    if (GEngine)
    {
        GEngine->RemoveOnScreenDebugMessage(100);
    }

    Super::EndPlay(EndPlayReason);
}
