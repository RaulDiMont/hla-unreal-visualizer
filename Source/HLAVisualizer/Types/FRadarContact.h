#pragma once

// Radar snapshot received from RadarFederate via HLA.
struct FRadarContact
{
    float Distance  = 0.f;    // kilometres from LEMD (Madrid-Barajas)
    float Bearing   = 0.f;    // degrees magnetic from LEMD
    bool  IsInRange = false;  // true when aircraft is within the 60km radar radius
};