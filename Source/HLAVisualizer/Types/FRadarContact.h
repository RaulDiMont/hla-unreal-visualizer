#pragma once

// Radar snapshot received from RadarFederate via HLA.
struct FRadarContact
{
    double Distance  = 0.0;    // kilometres from LEMD (Madrid-Barajas)
    double Bearing   = 0.0;    // degrees magnetic from LEMD
    bool  IsInRange = false;  // true when aircraft is within the 60km radar radius
};