#include "FHLAAmbassador.h"

#include <cstring>  // std::memcpy

FHLAAmbassador::FHLAAmbassador(
    TQueue<FAircraftState, EQueueMode::Spsc>* InAircraftQueue,
    TQueue<FRadarContact,  EQueueMode::Spsc>* InRadarQueue)
    : AircraftQueue(InAircraftQueue)
    , RadarQueue(InRadarQueue)
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

void FHLAAmbassador::discoverObjectInstance(
    rti1516e::ObjectInstanceHandle theObject,
    rti1516e::ObjectClassHandle    theObjectClass,
    std::wstring const&            theObjectInstanceName)
    RTI_THROW((rti1516e::FederateInternalError))
{
    InstanceClassMap[theObject] = theObjectClass;
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
    auto It = InstanceClassMap.find(theObject);
    if (It == InstanceClassMap.end())
    {
        return;
    }

    const rti1516e::ObjectClassHandle& ObjectClass = It->second;

    // Helper: copy raw bytes from a VariableLengthData entry into a typed value.
    // Both sides are x64 little-endian (WSL2 Linux + Windows), so no byte-swap is needed.
    auto ReadBytes = [&](rti1516e::AttributeHandle Handle, void* OutValue, size_t ExpectedSize) -> bool
    {
        auto AttrIt = theAttributeValues.find(Handle);
        if (AttrIt != theAttributeValues.end() && AttrIt->second.size() == ExpectedSize)
        {
            std::memcpy(OutValue, AttrIt->second.data(), ExpectedSize);
            return true;
        }
        return false;
    };

    if (ObjectClass == AircraftClass)
    {
        FAircraftState State;
        ReadBytes(LatitudeHandle,  &State.Latitude,  sizeof(double));
        ReadBytes(LongitudeHandle, &State.Longitude, sizeof(double));
        ReadBytes(AltitudeHandle,  &State.Altitude,  sizeof(double));
        AircraftQueue->Enqueue(State);
    }
    else if (ObjectClass == RadarContactClass)
    {
        FRadarContact Contact;
        ReadBytes(DistanceHandle,  &Contact.Distance,  sizeof(float));
        ReadBytes(BearingHandle,   &Contact.Bearing,   sizeof(float));
        ReadBytes(IsInRangeHandle, &Contact.IsInRange, sizeof(bool));
        RadarQueue->Enqueue(Contact);
    }
}