// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class HLAVisualizer : ModuleRules
{
	public HLAVisualizer(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "CesiumRuntime", "DeveloperSettings" });

		PrivateDependencyModuleNames.AddRange(new string[] {  });

		// Allow subdirectory includes (e.g. "Types/FAircraftState.h", "UnrealFederate/FHLAAmbassador.h")
		// to resolve relative to the module root. UBT does not add this automatically.
		PrivateIncludePaths.Add(ModuleDirectory);

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true

		// OpenRTI 0.10.0 — HLA 1516e bindings (Windows only)
		// Headers tracked in git; compiled binaries are gitignored and must be built locally.
		// Note: OpenRTI 0.10.0 uses rti:// as the TCP scheme, not tcp://.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			string OpenRTIPath = Path.Combine(ModuleDirectory, "../../ThirdParty/OpenRTI");

			// Headers: include/rti1516e/ contains RTI/RTI1516.h etc.
			PublicIncludePaths.Add(Path.Combine(OpenRTIPath, "include", "rti1516e"));

			// Import libraries
			PublicAdditionalLibraries.Add(Path.Combine(OpenRTIPath, "lib", "Win64", "rti1516e.lib"));
			PublicAdditionalLibraries.Add(Path.Combine(OpenRTIPath, "lib", "Win64", "fedtime1516e.lib"));

			// Delay-load the DLLs so Unreal's module loader controls when they are brought in
			PublicDelayLoadDLLs.Add("librti1516e.dll");
			PublicDelayLoadDLLs.Add("libfedtime1516e.dll");
			PublicDelayLoadDLLs.Add("OpenRTI.dll");

			// Stage DLLs next to the game binary at cook/package time
			RuntimeDependencies.Add("$(BinaryOutputDir)/librti1516e.dll",
				Path.Combine(OpenRTIPath, "bin", "Win64", "librti1516e.dll"));
			RuntimeDependencies.Add("$(BinaryOutputDir)/libfedtime1516e.dll",
				Path.Combine(OpenRTIPath, "bin", "Win64", "libfedtime1516e.dll"));
			RuntimeDependencies.Add("$(BinaryOutputDir)/OpenRTI.dll",
				Path.Combine(OpenRTIPath, "bin", "Win64", "OpenRTI.dll"));
		}
	}
}
