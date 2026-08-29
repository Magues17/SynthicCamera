using UnrealBuildTool;

public class SynthicCameraEditorTarget : TargetRules
{
	public SynthicCameraEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("SynthicCamera");
	}
}
