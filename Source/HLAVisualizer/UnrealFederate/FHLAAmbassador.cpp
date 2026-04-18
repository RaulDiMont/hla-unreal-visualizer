#include "FHLAAmbassador.h"
#include "FHLAFederateRunnable.h"
#include "HAL/PlatformTime.h"

THIRD_PARTY_INCLUDES_START
#include <RTI/encoding/BasicDataElements.h>
THIRD_PARTY_INCLUDES_END

FHLAAmbassador::FHLAAmbassador(
    TQueue<FAircraftState, EQueueMode::Spsc>* InAircraftQueue,
    TQueue<FRadarContact,  EQueueMode::Spsc>* InRadarQueue,
    FHLAFederateRunnable*                     InOwner)
    : AircraftQueue(InAircraftQueue)
    , RadarQueue(InRadarQueue)
    , Owner(InOwner)
{
}

FHLAAmbassador::~FHLAAmbassador() RTI_NOEXCEPT
{
}

void FHLAAmbassador::CacheHandles(rti1516e::RTIambassador& Rta)
{
    AircraftClass     = Rta.getObjectClassHandle(L"Aircraft");
    RadarContactClass = Rta.getObjectClassHandle(L"RadarContact");

    LatitudeHandle  = Rta.getAttributeHandle(AircraftClass,     L"Latitude");
    LongitudeHandle = Rta.getAttributeHandle(AircraftClass,     L"Longitude");
    AltitudeHandle  = Rta.getAttributeHandle(AircraftClass,     L"Altitude");

    DistanceHandle  = Rta.getAttributeHandle(RadarContactClass, L"Distance");
    BearingHandle   = Rta.getAttributeHandle(RadarContactClass, L"Bearing");
    IsInRangeHandle = Rta.getAttributeHandle(RadarContactClass, L"IsInRange");
}

void FHLAAmbassador::connectionLost(std::wstring const& faultDescription)
    RTI_THROW((rti1516e::FederateInternalError))
{
    // Called by OpenRTI from within evokeCallback when the rtinode connection drops.
    // We are already on the HLA thread — safe to write to the atomic flags directly.
    // Do NOT call resignFederationExecution here: the RTI is already tearing down and
    // a resign call would cause an access violation inside OpenRTI.
    UE_LOG(LogTemp, Warning, TEXT("UnrealFederate: connectionLost — %s"),
           *FString(faultDescription.c_str()));
    Owner->SignalConnectionLost();
}

void FHLAAmbassador::discoverObjectInstance(
    rti1516e::ObjectInstanceHandle theObject,
    rti1516e::ObjectClassHandle    theObjectClass,
    std::wstring const&            theObjectInstanceName)
    RTI_THROW((rti1516e::FederateInternalError))
{
    InstanceClassMap[theObject] = theObjectClass;
}

void FHLAAmbassador::removeObjectInstance(
    rti1516e::ObjectInstanceHandle      theObject,
    rti1516e::VariableLengthData const& theUserSuppliedTag,
    rti1516e::OrderType                 sentOrder,
    rti1516e::SupplementalRemoveInfo    theRemoveInfo)
    RTI_THROW((rti1516e::FederateInternalError))
{
    std::map<rti1516e::ObjectInstanceHandle, rti1516e::ObjectClassHandle>::iterator It = InstanceClassMap.find(theObject);
    if (It != InstanceClassMap.end() && It->second == AircraftClass)
    {
        UE_LOG(LogTemp, Log, TEXT("UnrealFederate: Aircraft object removed — simulation ended."));
        Owner->SignalConnectionLost();
    }
    InstanceClassMap.erase(theObject);
}

void FHLAAmbassador::reflectAttributeValues(
    rti1516e::ObjectInstanceHandle           theObject,
    rti1516e::AttributeHandleValueMap const& theAttributeValues,
    rti1516e::VariableLengthData const&      theUserSuppliedTag,
    rti1516e::OrderType                      sentOrder,
    rti1516e::TransportationType             theType,
    rti1516e::SupplementalReflectInfo        theReflectInfo)
    RTI_THROW((rti1516e::FederateInternalError))
{
    std::map<rti1516e::ObjectInstanceHandle, rti1516e::ObjectClassHandle>::iterator It = InstanceClassMap.find(theObject);
    if (It == InstanceClassMap.end())
    {
        return;
    }

    const rti1516e::ObjectClassHandle& ObjectClass = It->second;

    // Decode a HLAfloat64BE attribute into a double.
    // The simulator (WSL2) encodes all floating-point attributes as big-endian (HLAfloat64BE),
    // which is the IEEE 1516-2010 standard network encoding.
    auto DecodeFloat64 = [&](rti1516e::AttributeHandle Handle, double& Out) -> bool
    {
        rti1516e::AttributeHandleValueMap::const_iterator It = theAttributeValues.find(Handle);
        if (It == theAttributeValues.end()) return false;
        rti1516e::HLAfloat64BE decoder;
        decoder.decode(It->second);
        Out = decoder.get();
        return true;
    };

    if (ObjectClass == AircraftClass)
    {
        FAircraftState State;
        DecodeFloat64(LatitudeHandle,  State.Latitude);
        DecodeFloat64(LongitudeHandle, State.Longitude);
        DecodeFloat64(AltitudeHandle,  State.Altitude);
        State.Timestamp = FPlatformTime::Seconds();
        UE_LOG(LogTemp, Log, TEXT("[Aircraft] Lat=%.6f  Lon=%.6f  Alt=%.1f ft"),
            State.Latitude, State.Longitude, State.Altitude);
        AircraftQueue->Enqueue(State);
    }
    else if (ObjectClass == RadarContactClass)
    {
        FRadarContact Contact;
        DecodeFloat64(DistanceHandle, Contact.Distance);
        DecodeFloat64(BearingHandle,  Contact.Bearing);
        // IsInRange — HLAboolean (4 bytes). OpenRTI wraps it as bool via get().
        rti1516e::AttributeHandleValueMap::const_iterator InRangeIt = theAttributeValues.find(IsInRangeHandle);
        if (InRangeIt != theAttributeValues.end())
        {
            rti1516e::HLAboolean Decoder;
            Decoder.decode(InRangeIt->second);
            Contact.IsInRange = Decoder.get();
        }
        UE_LOG(LogTemp, Log, TEXT("[Radar] Distance=%.2f km  Bearing=%.1f°  InRange=%s"),
            Contact.Distance, Contact.Bearing, Contact.IsInRange ? TEXT("true") : TEXT("false"));
        RadarQueue->Enqueue(Contact);
    }
}