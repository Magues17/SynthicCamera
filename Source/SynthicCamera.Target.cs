using UnrealBuildTool;

public class SynthicCameraTarget : TargetRules
{
	public SynthicCameraTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("SynthicCamera");
	}
}
