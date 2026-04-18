#pragma once

// Position snapshot received from AircraftFederate via HLA.
// Altitude is kept in feet (JSBSim native unit); conversion to metres happens in AUnrealFederateActor::Tick().
struct FAircraftState
{
    double Latitude  = 0.0;  // degrees WGS84
    double Longitude = 0.0;  // degrees WGS84
    double Altitude  = 0.0;  // feet above MSL
    double Timestamp = 0.0;  // FPlatformTime::Seconds() at receive time — used for interpolation
};