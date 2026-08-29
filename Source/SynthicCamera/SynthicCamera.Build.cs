using UnrealBuildTool;

public class SynthicCamera : ModuleRules
{
	public SynthicCamera(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Flat Source/<Module>/ layout with subfolders: UE only auto-adds Public/Private
		// as include roots, so #include "Capture/Foo.h" would not resolve without this.
		PublicIncludePaths.Add(ModuleDirectory);

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine", "RenderCore", "RHI", "Json"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"ImageWrapper"
		});
	}
}
