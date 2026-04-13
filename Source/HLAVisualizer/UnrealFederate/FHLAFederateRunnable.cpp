#include "FHLAFederateRunnable.h"
#include "FHLAAmbassador.h"

THIRD_PARTY_INCLUDES_START
#include <RTI/RTIambassadorFactory.h>
#include <RTI/Enums.h>
THIRD_PARTY_INCLUDES_END

// RTI connection settings.
//
// WSL2_IP: the virtual network IP of the WSL2 instance.
// Run `ip addr show eth0 | grep "inet "` inside WSL2 to get it — it changes on reboot.
// rtinode must already be running: `rtinode -i 0.0.0.0:14321`
static const std::wstring kRtiUrl         = L"rti://172.26.53.127:14321";
static const std::wstring kFederationName = L"AircraftSimulation";
static const std::wstring kFederateType   = L"UnrealFederate";

FHLAFederateRunnable::FHLAFederateRunnable(
    TQueue<FAircraftState, EQueueMode::Spsc>* InAircraftQueue,
    TQueue<FRadarContact,  EQueueMode::Spsc>* InRadarQueue)
    : AircraftQueue(InAircraftQueue)
    , RadarQueue(InRadarQueue)
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
    TQueue<FRadarContact,  EQueueMode::Spsc>* RadarQueue)
{
    try
    {
        OutAmbassador = MakeUnique<FHLAAmbassador>(AircraftQueue, RadarQueue);

        rti1516e::RTIambassadorFactory Factory;
        OutRti = Factory.createRTIambassador();

        OutRti->connect(*OutAmbassador, rti1516e::HLA_EVOKED, kRtiUrl);
        OutRti->joinFederationExecution(kFederateType, kFederationName);

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
    // Retry loop: wait for the federation to be created by the WSL2 simulator.
    // This allows pressing Play in Unreal before launching aircraft_simulator.
    constexpr int   MaxRetries     = 30;
    constexpr float RetryIntervalS = 2.0f;

    for (int i = 0; i < MaxRetries; ++i)
    {
        if (i > 0)
        {
            UE_LOG(LogTemp, Log, TEXT("UnrealFederate: waiting for federation... (%d/%d)"), i, MaxRetries);
            FPlatformProcess::Sleep(RetryIntervalS);
        }

        if (TryConnect(Ambassador, RtiAmbassador, AircraftQueue, RadarQueue))
        {
            bRunning.store(true);
            bConnected.store(true);
            UE_LOG(LogTemp, Log, TEXT("UnrealFederate: joined '%s' at %s"),
                   *FString(kFederationName.c_str()), *FString(kRtiUrl.c_str()));
            return true;
        }
    }

    UE_LOG(LogTemp, Error, TEXT("UnrealFederate: could not join federation after %d retries. Check rtinode and aircraft_simulator are running."), MaxRetries);
    return false;
}

uint32 FHLAFederateRunnable::Run()
{
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
    // For the MVP we rely on the RTIambassador destructor and OS socket teardown to
    // clean up the connection. A proper reconnect/lifecycle policy is deferred to post-MVP.
    //
    // TODO (post-MVP): implement FederateAmbassador::connectionLost() in FHLAAmbassador
    // to detect federation teardown and allow a clean resign before the simulation ends.
}