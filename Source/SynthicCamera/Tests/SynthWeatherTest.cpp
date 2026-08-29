#include "Synth/SynthWeather.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSynthWeatherTest, "Synthic.Weather.PresetsAndJitter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSynthWeatherTest::RunTest(const FString& /*Parameters*/)
{
	const FSynthWeatherPreset Clear = SynthWeather::GetPreset(ESynthWeather::Clear);
	const FSynthWeatherPreset Overcast = SynthWeather::GetPreset(ESynthWeather::Overcast);
	const FSynthWeatherPreset Dust = SynthWeather::GetPreset(ESynthWeather::DustStorm);

	// The point of the presets is that they span a range. If these ever collapse the
	// dataset still carries four distinct labels while every image looks the same.
	TestTrue(TEXT("clear sun is stronger than overcast"), Clear.SunIntensity > Overcast.SunIntensity);
	TestTrue(TEXT("overcast fills shadows more than clear"), Overcast.AmbientIntensity > Clear.AmbientIntensity);
	TestTrue(TEXT("dust is foggier than clear"), Dust.FogDensity > Clear.FogDensity);
	TestTrue(TEXT("dust sun is warmer than clear"), Dust.SunColor.B < Clear.SunColor.B);

	for (const ESynthWeather Weather : { ESynthWeather::Clear, ESynthWeather::Hazy,
		ESynthWeather::Overcast, ESynthWeather::DustStorm })
	{
		const FSynthWeatherPreset Preset = SynthWeather::GetPreset(Weather);
		TestTrue(TEXT("sun intensity is positive"), Preset.SunIntensity > 0.0f);
		TestTrue(TEXT("fog density is non-negative"), Preset.FogDensity >= 0.0f);
		TestFalse(TEXT("condition has a name"), SynthWeather::GetName(Weather).IsEmpty());
	}

	// Zero jitter must be the identity, or "randomisation off" would still drift.
	FRandomStream Stream(4242);
	const FSynthWeatherPreset Unjittered = SynthWeather::Jitter(Clear, Stream, 0.0f);
	TestEqual(TEXT("zero jitter leaves sun untouched"), Unjittered.SunIntensity, Clear.SunIntensity);

	// Every sample must stay inside the stated band - a preset that can jitter to
	// zero sun would emit unlabelled night frames in the middle of a daytime run.
	for (int32 Sample = 0; Sample < 200; ++Sample)
	{
		const FSynthWeatherPreset Jittered = SynthWeather::Jitter(Clear, Stream, 0.25f);
		TestTrue(TEXT("sun stays within +/-25%"),
			Jittered.SunIntensity >= Clear.SunIntensity * 0.75f - UE_KINDA_SMALL_NUMBER &&
			Jittered.SunIntensity <= Clear.SunIntensity * 1.25f + UE_KINDA_SMALL_NUMBER);
		TestTrue(TEXT("ambient stays positive"), Jittered.AmbientIntensity > 0.0f);
	}

	// Out-of-range jitter is clamped rather than trusted.
	const FSynthWeatherPreset Extreme = SynthWeather::Jitter(Clear, Stream, 5.0f);
	TestTrue(TEXT("clamped jitter cannot invert the sun"), Extreme.SunIntensity > 0.0f);

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
