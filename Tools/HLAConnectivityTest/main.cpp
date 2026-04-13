// HLAConnectivityTest/main.cpp
//
// Phase 4.1 verification app — joins the AircraftSimulation HLA federation
// running on rtinode in WSL2, then immediately resigns.
//
// Purpose: confirm that a Windows process can reach the WSL2 rtinode via tcp://
// before wiring OpenRTI into Unreal.
//
// Usage:
//   HLAConnectivityTest.exe [rtinode_address]
//
// Default address: tcp://172.28.0.1:14321
// To find the actual WSL2 IP in WSL2: ip addr show eth0 | grep inet
//
// Prerequisites:
//   - rtinode running in WSL2:  rtinode --listen 0.0.0.0:14321
//   - aircraft_simulator running in WSL2 (creates the federation)
//   - OpenRTI.dll next to this exe (CMake post-build step handles this)

#include <RTI/RTI1516e.h>
#include <RTI/NullFederateAmbassador.h>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

int main(int argc, char* argv[])
{
    const std::wstring FederationName = L"AircraftSimulation";
    const std::wstring FederateName   = L"UnrealVerifier";
    const std::wstring FederateType   = L"Verifier";

    // Accept rtinode address as optional CLI argument
    std::string RtiAddress = "tcp://172.28.0.1:14321";
    if (argc > 1)
        RtiAddress = argv[1];

    std::wcout << L"[HLAConnectivityTest] Connecting to: "
               << std::wstring(RtiAddress.begin(), RtiAddress.end()) << L"\n";

    try
    {
        rti1516e::RTIambassadorFactory Factory;
        std::unique_ptr<rti1516e::RTIambassador> RtiAmb = Factory.createRTIambassador();

        // NullFederateAmbassador provides empty stub implementations for all callbacks.
        // We only care about the connect/join/resign roundtrip here.
        rti1516e::NullFederateAmbassador FedAmb;
        RtiAmb->connect(FedAmb, rti1516e::HLA_EVOKED, RtiAddress);
        std::wcout << L"[OK] Connected to rtinode\n";

        // Join the existing federation — no additional FOM modules needed since
        // aircraft_simulator has already created it with AircraftFOM.xml + RadarFOM.xml.
        std::vector<std::wstring> AdditionalModules;
        RtiAmb->joinFederationExecution(
            FederateName, FederateType, FederationName, AdditionalModules);
        std::wcout << L"[OK] Joined federation: " << FederationName << L"\n";

        RtiAmb->resignFederationExecution(rti1516e::NO_ACTION);
        std::wcout << L"[OK] Resigned from federation\n";

        RtiAmb->disconnect();
        std::wcout << L"[OK] Disconnected from rtinode\n";

        std::wcout << L"\nSUCCESS — Phase 4.1 connectivity verified.\n"
                   << L"Windows process successfully joined HLA federation over WSL2 tcp://\n";
        return 0;
    }
    catch (const rti1516e::ConnectionFailed& e)
    {
        std::wcerr << L"[FAIL] Cannot reach rtinode at "
                   << std::wstring(RtiAddress.begin(), RtiAddress.end()) << L"\n"
                   << L"       " << e.what() << L"\n"
                   << L"  --> Is rtinode running in WSL2?  rtinode --listen 0.0.0.0:14321\n"
                   << L"  --> Is port 14321 open in Windows Firewall?\n";
        return 1;
    }
    catch (const rti1516e::FederationExecutionDoesNotExist& e)
    {
        std::wcerr << L"[FAIL] Federation '" << FederationName << L"' does not exist.\n"
                   << L"       " << e.what() << L"\n"
                   << L"  --> Start aircraft_simulator in WSL2 first\n";
        return 1;
    }
    catch (const rti1516e::Exception& e)
    {
        std::wcerr << L"[FAIL] RTI exception: " << e.what() << L"\n";
        return 1;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[FAIL] std::exception: " << e.what() << "\n";
        return 1;
    }
}
