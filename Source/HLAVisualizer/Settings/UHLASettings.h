#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "UHLASettings.generated.h"

// Project-wide HLA connection settings.
// Edit in: Project Settings → Plugins → HLA Visualizer
//
// Values are stored in Config/DefaultGame.ini under [/Script/HLAVisualizer.HLASettings].
// Commit that file to share settings across the team — only the RTI address needs
// updating when the WSL2 IP changes on reboot.
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "HLA Visualizer"))
class HLAVISUALIZER_API UHLASettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UHLASettings();

    // RTI node address: rti://<WSL2_IP>:<port>
    // Run `ip addr show eth0 | grep "inet "` inside WSL2 to get the current IP.
    // rtinode must be running: `rtinode -i 0.0.0.0:14321`
    UPROPERTY(config, EditAnywhere, Category = "Connection",
        meta = (DisplayName = "RTI Address", ToolTip = "Full RTI URL, e.g. rti://172.26.53.127:14321"))
    FString RTIAddress = TEXT("rti://172.26.53.127:14321");

    // HLA federation name — must match the value used by aircraft_simulator.
    UPROPERTY(config, EditAnywhere, Category = "Connection",
        meta = (DisplayName = "Federation Name"))
    FString FederationName = TEXT("AircraftSimulation");
};