#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"

THIRD_PARTY_INCLUDES_START
#include <RTI/NullFederateAmbassador.h>
#include <RTI/RTIambassador.h>
THIRD_PARTY_INCLUDES_END

#include <map>

#include "Types/FAircraftState.h"
#include "Types/FRadarContact.h"

// Handles HLA protocol callbacks for the UnrealFederate.
//
// Inherits NullFederateAmbassador so only the two callbacks we care about need
// to be implemented — the ~20 other virtual methods stay as no-ops.
//
// Threading: callbacks arrive on the FHLAFederateRunnable thread (the same thread
// that calls evokeCallback). This class itself has no thread state.
class FHLAAmbassador : public rti1516e::NullFederateAmbassador
{
public:
    FHLAAmbassador(
        TQueue<FAircraftState, EQueueMode::Spsc>* InAircraftQueue,
        TQueue<FRadarContact,  EQueueMode::Spsc>* InRadarQueue);

    virtual ~FHLAAmbassador() RTI_NOEXCEPT override;

    // Called by FHLAFederateRunnable after joining the federation.
    // Fetches and caches all object class and attribute handles needed for decoding.
    void CacheHandles(rti1516e::RTIambassador& Rta);

    // 6.9 — records newly discovered object instances so reflectAttributeValues can
    // dispatch to the correct struct (Aircraft vs RadarContact).
    virtual void discoverObjectInstance(
        rti1516e::ObjectInstanceHandle theObject,
        rti1516e::ObjectClassHandle    theObjectClass,
        std::wstring const&            theObjectInstanceName)
        RTI_THROW((rti1516e::FederateInternalError)) override;

    // 6.11 — timestamp-free variant; used because we run without HLA Time Management.
    virtual void reflectAttributeValues(
        rti1516e::ObjectInstanceHandle           theObject,
        rti1516e::AttributeHandleValueMap const& theAttributeValues,
        rti1516e::VariableLengthData const&      theUserSuppliedTag,
        rti1516e::OrderType                      sentOrder,
        rti1516e::TransportationType             theType,
        rti1516e::SupplementalReflectInfo        theReflectInfo)
        RTI_THROW((rti1516e::FederateInternalError)) override;

private:
    TQueue<FAircraftState, EQueueMode::Spsc>* AircraftQueue;
    TQueue<FRadarContact,  EQueueMode::Spsc>* RadarQueue;

    // Cached object class handles
    rti1516e::ObjectClassHandle AircraftClass;
    rti1516e::ObjectClassHandle RadarContactClass;

    // Cached attribute handles — Aircraft object
    rti1516e::AttributeHandle LatitudeHandle;
    rti1516e::AttributeHandle LongitudeHandle;
    rti1516e::AttributeHandle AltitudeHandle;

    // Cached attribute handles — RadarContact object
    rti1516e::AttributeHandle DistanceHandle;
    rti1516e::AttributeHandle BearingHandle;
    rti1516e::AttributeHandle IsInRangeHandle;

    // Maps discovered instance handles to their object class.
    // std::map is used because rti1516e handles do not provide a TMap-compatible hash.
    std::map<rti1516e::ObjectInstanceHandle, rti1516e::ObjectClassHandle> InstanceClassMap;
};