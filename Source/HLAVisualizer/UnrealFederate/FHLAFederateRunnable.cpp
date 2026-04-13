#include "FHLAFederateRunnable.h"
#include "FHLAAmbassador.h"

THIRD_PARTY_INCLUDES_START
#include <RTI/RTIambassadorFactory.h>
#include <RTI/Enums.h>
THIRD_PARTY_INCLUDES_END

static const std::wstring kFederateType = L"UnrealFederate";

FHLAFederateRunnable::FHLAFederateRunnable(
    TQueue<FAircraftState, EQueueMode::Spsc>* InAircraftQueue,
    TQueue<FRadarContact,  EQueueMode::Spsc>* InRadarQueue,
    FString                                   InRtiAddress,
    FString                                   InFederationName)
    : AircraftQueue(InAircraftQueue)
    , RadarQueue(InRadarQueue)
    , RtiUrl(*InRtiAddress)
    , FederationNameW(*InFederationName)
{
}

FHLAFederateRunnable::~FHLAFederateRunnable()
{
}

// Attempt a single connection + join cycle. Returns true on success.
// On failure, cleans up and leaves RtiAmbassador/Ambassador in a reset state.
static bool TryConnect(
    TUniquePtr<FHLAAmbassador>&               OutAmbassador,
    std::unique_ptr<rti1516e::RTIambassador>& OutRti,
    TQueue<FAircraftState, EQueueMode::Spsc>* AircraftQueue,
    TQueue<FRadarContact,  EQueueMode::Spsc>* RadarQueue,
    FHLAFederateRunnable*                     Owner,
    const std::wstring&                       InRtiUrl,
    const std::wstring&                       InFederationName)
{
    try
    {
        OutAmbassador = MakeUnique<FHLAAmbassador>(AircraftQueue, RadarQueue, Owner);

        rti1516e::RTIambassadorFactory Factory;
        OutRti = Factory.createRTIambassador();

        OutRti->connect(*OutAmbassador, rti1516e::HLA_EVOKED, InRtiUrl);
        OutRti->joinFederationExecution(kFederateType, InFederationName);

        OutAmbassador->CacheHandles(*OutRti);

        {
            rti1516e::ObjectClassHandle AircraftClass = OutRti->getObjectClassHandle(L"Aircraft");
            rti1516e::AttributeHandleSet Attribs;
            Attribs.insert(OutRti->getAttributeHandle(AircraftClass, L"Latitude"));
            Attribs.insert(OutRti->getAttributeHandle(AircraftClass, L"Longitude"));
            Attribs.insert(OutRti->getAttributeHandle(AircraftClass, L"Altitude"));
            OutRti->subscribeObjectClassAttributes(AircraftClass, Attribs);
        }

        {
            rti1516e::ObjectClassHandle RadarClass = OutRti->getObjectClassHandle(L"RadarContact");
            rti1516e::AttributeHandleSet Attribs;
            Attribs.insert(OutRti->getAttributeHandle(RadarClass, L"Distance"));
            Attribs.insert(OutRti->getAttributeHandle(RadarClass, L"Bearing"));
            Attribs.insert(OutRti->getAttributeHandle(RadarClass, L"IsInRange"));
            OutRti->subscribeObjectClassAttributes(RadarClass, Attribs);
        }

        return true;
    }
    catch (...)
    {
        // Clean up so the next retry starts fresh.
        if (OutRti)
        {
            try { OutRti->disconnect(); } catch (...) {}
            OutRti.reset();
        }
        OutAmbassador.Reset();
        return false;
    }
}

bool FHLAFederateRunnable::Init()
{
    // Init() is called by FRunnableThread::Create() while the GameThread is blocked
    // waiting for this semaphore. Keep it instant — all connection work goes in Run().
    bRunning.store(true);
    return true;
}

uint32 FHLAFederateRunnable::Run()
{
    // Connect + join before entering the event pump.
    // Running on the HLA thread — the GameThread is never blocked here.
    constexpr int   MaxRetries     = 30;
    constexpr float RetryIntervalS = 2.0f;

    for (int i = 0; i < MaxRetries && bRunning.load(); ++i)
    {
        if (i > 0)
        {
            UE_LOG(LogTemp, Log, TEXT("UnrealFederate: waiting for federation... (%d/%d)"), i, MaxRetries);
            FPlatformProcess::Sleep(RetryIntervalS);
        }

        if (TryConnect(Ambassador, RtiAmbassador, AircraftQueue, RadarQueue, this, RtiUrl, FederationNameW))
        {
            bConnected.store(true);
            UE_LOG(LogTemp, Log, TEXT("UnrealFederate: joined '%s' at %s"),
                   *FString(FederationNameW.c_str()), *FString(RtiUrl.c_str()));
            break;
        }
    }

    if (!bConnected.load())
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealFederate: could not join federation after %d retries. Check rtinode and aircraft_simulator are running."), MaxRetries);
        bRunning.store(false);
        return 1;
    }

    // Event pump: drain HLA callbacks until Stop() is called or the federation ends.
    while (bRunning.load())
    {
        try
        {
            // Block up to 0.1s waiting for an HLA event; returns immediately if one arrives.
            RtiAmbassador->evokeCallback(0.1);
        }
        catch (...)
        {
            // Any exception from evokeCallback means the federation has ended or the
            // connection was lost. Stop the pump and mark as disconnected so Stop()
            // does not attempt resign/disconnect on a dead ambassador.
            UE_LOG(LogTemp, Warning, TEXT("UnrealFederate: evokeCallback threw — federation ended, stopping pump"));
            bConnected.store(false);
            bRunning.store(false);
        }
    }
    return 0;
}

void FHLAFederateRunnable::SignalConnectionLost()
{
    // Called from FHLAAmbassador::connectionLost(), which fires inside evokeCallback.
    // Setting bRunning to false here causes the while(bRunning) loop in Run() to exit
    // after evokeCallback returns — OpenRTI still owns the ambassador at this point,
    // so we must not touch RtiAmbassador after this call.
    bConnected.store(false);
    bRunning.store(false);
}

void FHLAFederateRunnable::Stop()
{
    bRunning.store(false);
    bConnected.store(false);

    // NOTE: resign/disconnect are intentionally skipped here.
    //
    // If the WSL2 simulation has already ended, OpenRTI holds a dangling pointer to
    // the destroyed federation. Calling resignFederationExecution() on that state
    // causes an access violation inside OpenRTI itself (not catchable via C++ try/catch).
    //
    // The clean shutdown path when the simulation ends first is handled by
    // FHLAAmbassador::connectionLost() → SignalConnectionLost(), which exits the Run()
    // loop before OpenRTI tears down its internal state.
}