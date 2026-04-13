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
// rtinode must already be running: `rtinode --listen 0.0.0.0:14321 &`
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

bool FHLAFederateRunnable::Init()
{
    try
    {
        Ambassador = MakeUnique<FHLAAmbassador>(AircraftQueue, RadarQueue);

        rti1516e::RTIambassadorFactory Factory;
        RtiAmbassador = Factory.createRTIambassador();

        // HLA_EVOKED: callbacks only fire when we call evokeCallback() — no RTI-managed thread.
        RtiAmbassador->connect(*Ambassador, rti1516e::HLA_EVOKED, kRtiUrl);

        // The federation is already created by AircraftFederate in WSL2; just join.
        RtiAmbassador->joinFederationExecution(kFederateType, kFederationName);

        // Cache object class and attribute handles in the ambassador for callback dispatch.
        Ambassador->CacheHandles(*RtiAmbassador);

        // Subscribe to Aircraft attributes (published by AircraftFederate).
        {
            rti1516e::ObjectClassHandle AircraftClass = RtiAmbassador->getObjectClassHandle(L"Aircraft");
            rti1516e::AttributeHandleSet Attribs;
            Attribs.insert(RtiAmbassador->getAttributeHandle(AircraftClass, L"Latitude"));
            Attribs.insert(RtiAmbassador->getAttributeHandle(AircraftClass, L"Longitude"));
            Attribs.insert(RtiAmbassador->getAttributeHandle(AircraftClass, L"Altitude"));
            RtiAmbassador->subscribeObjectClassAttributes(AircraftClass, Attribs);
        }

        // Subscribe to RadarContact attributes (published by RadarFederate).
        {
            rti1516e::ObjectClassHandle RadarClass = RtiAmbassador->getObjectClassHandle(L"RadarContact");
            rti1516e::AttributeHandleSet Attribs;
            Attribs.insert(RtiAmbassador->getAttributeHandle(RadarClass, L"Distance"));
            Attribs.insert(RtiAmbassador->getAttributeHandle(RadarClass, L"Bearing"));
            Attribs.insert(RtiAmbassador->getAttributeHandle(RadarClass, L"IsInRange"));
            RtiAmbassador->subscribeObjectClassAttributes(RadarClass, Attribs);
        }

        bRunning.store(true);

        UE_LOG(LogTemp, Log, TEXT("UnrealFederate: joined '%s' at %s"),
               *FString(kFederationName.c_str()), *FString(kRtiUrl.c_str()));

        return true;
    }
    catch (const rti1516e::Exception& Ex)
    {
        UE_LOG(LogTemp, Error, TEXT("UnrealFederate Init failed: %s"),
               *FString(Ex.what().c_str()));
        return false;
    }
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
        catch (const rti1516e::Exception& Ex)
        {
            UE_LOG(LogTemp, Warning, TEXT("UnrealFederate evokeCallback: %s"),
                   *FString(Ex.what().c_str()));
        }
    }
    return 0;
}

void FHLAFederateRunnable::Stop()
{
    bRunning.store(false);

    if (RtiAmbassador)
    {
        try
        {
            RtiAmbassador->resignFederationExecution(rti1516e::NO_ACTION);
            RtiAmbassador->disconnect();
        }
        catch (const rti1516e::Exception& Ex)
        {
            UE_LOG(LogTemp, Warning, TEXT("UnrealFederate Stop: %s"),
                   *FString(Ex.what().c_str()));
        }
    }
}