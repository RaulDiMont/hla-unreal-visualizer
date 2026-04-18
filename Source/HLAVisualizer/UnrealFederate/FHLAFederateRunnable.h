#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "Containers/Queue.h"

THIRD_PARTY_INCLUDES_START
#include <RTI/RTIambassador.h>
THIRD_PARTY_INCLUDES_END

#include <atomic>
#include <memory>

#include "Types/FAircraftState.h"
#include "Types/FRadarContact.h"

class FHLAAmbassador;

// Manages the HLA thread lifecycle and the RTIambassador event pump.
//
// Owns FHLAAmbassador — the ambassador's callbacks arrive on this thread
// because we use HLA_EVOKED mode and drive the pump via evokeCallback().
//
// Lifecycle:
//   AUnrealFederateActor::BeginPlay  → FRunnableThread::Create → Init() → Run()
//   AUnrealFederateActor::EndPlay    → Stop() → FRunnableThread::Kill(true)
class FHLAFederateRunnable : public FRunnable
{
public:
    FHLAFederateRunnable(
        TQueue<FAircraftState, EQueueMode::Spsc>* InAircraftQueue,
        TQueue<FRadarContact,  EQueueMode::Spsc>* InRadarQueue,
        FString                                   InRtiAddress,
        FString                                   InFederationName,
        TFunction<void()>                         InOnConnectionLost);

    virtual ~FHLAFederateRunnable() override;

    // FRunnable interface
    virtual bool   Init() override;  // Sets bRunning = true; connection work is done in Run()
    virtual uint32 Run()  override;  // Connect + join + subscribe, then evokeCallback loop
    virtual void   Stop() override;  // Signal exit (resign/disconnect deferred to post-MVP)

    // Called by FHLAAmbassador::connectionLost() from within evokeCallback.
    // Marks the pump as stopped so Run() exits cleanly before OpenRTI tears down.
    void SignalConnectionLost();

private:
    TQueue<FAircraftState, EQueueMode::Spsc>* AircraftQueue;
    TQueue<FRadarContact,  EQueueMode::Spsc>* RadarQueue;

    TUniquePtr<FHLAAmbassador>               Ambassador;
    std::unique_ptr<rti1516e::RTIambassador> RtiAmbassador;

    std::atomic<bool> bRunning   { false };
    std::atomic<bool> bConnected { false };  // false once evokeCallback signals connection loss

    // Invoked on the GameThread when the simulation ends — either by connection loss or
    // normal federate resignation. Marshaled via AsyncTask from the HLA thread.
    TFunction<void()> OnConnectionLostCallback;

    std::wstring RtiUrl;
    std::wstring FederationNameW;
};