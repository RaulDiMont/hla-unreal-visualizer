#include "UHLASettings.h"

UHLASettings::UHLASettings()
{
    // Register this settings object under the "Plugins" category in Project Settings.
    CategoryName = TEXT("Plugins");
    SectionName  = TEXT("HLA Visualizer");
}