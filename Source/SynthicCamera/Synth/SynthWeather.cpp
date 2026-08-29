#include "Synth/SynthWeather.h"

#include "SynthicCamera.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace SynthWeather
{

FSynthWeatherPreset GetPreset(ESynthWeather Weather)
{
	FSynthWeatherPreset Preset;

	switch (Weather)
	{
	case ESynthWeather::Clear:
		// Hard sun, deep shadows, long visibility. The high-contrast end of the range.
		Preset.SunIntensity = 10.0f;
		Preset.SunColor = FLinearColor(1.00f, 0.96f, 0.90f);
		Preset.FogDensity = 0.005f;
		Preset.FogColor = FLinearColor(0.50f, 0.58f, 0.70f);
		Preset.AmbientIntensity = 2.5f;
		break;

	case ESynthWeather::Hazy:
		Preset.SunIntensity = 6.5f;
		Preset.SunColor = FLinearColor(1.00f, 0.93f, 0.80f);
		Preset.FogDensity = 0.35f;
		Preset.FogColor = FLinearColor(0.62f, 0.58f, 0.48f);
		Preset.AmbientIntensity = 4.5f;
		break;

	case ESynthWeather::Overcast:
		// Sun mostly blocked, everything lit by a bright grey dome: low contrast,
		// which is where speed-camera footage usually sits and where a model trained
		// only on clear-sky renders tends to fall over.
		Preset.SunIntensity = 2.0f;
		Preset.SunColor = FLinearColor(0.88f, 0.91f, 0.98f);
		Preset.FogDensity = 0.9f;
		Preset.FogColor = FLinearColor(0.60f, 0.62f, 0.66f);
		Preset.AmbientIntensity = 8.0f;
		break;

	case ESynthWeather::DustStorm:
		Preset.SunIntensity = 1.5f;
		Preset.SunColor = FLinearColor(1.00f, 0.72f, 0.42f);
		Preset.FogDensity = 2.0f;
		Preset.FogColor = FLinearColor(0.66f, 0.50f, 0.30f);
		Preset.AmbientIntensity = 6.0f;
		break;
	}

	return Preset;
}

FString GetName(ESynthWeather Weather)
{
	return StaticEnum<ESynthWeather>()->GetNameStringByValue(static_cast<int64>(Weather));
}

FSynthWeatherPreset Jitter(const FSynthWeatherPreset& Preset, const FRandomStream& Stream, float Amount)
{
	const float Bounded = FMath::Clamp(Amount, 0.0f, 0.9f);
	auto Scale = [&Stream, Bounded](float Value)
	{
		return Value * Stream.FRandRange(1.0f - Bounded, 1.0f + Bounded);
	};

	FSynthWeatherPreset Jittered = Preset;
	Jittered.SunIntensity = Scale(Preset.SunIntensity);
	Jittered.FogDensity = Scale(Preset.FogDensity);
	Jittered.AmbientIntensity = Scale(Preset.AmbientIntensity);
	return Jittered;
}

void ApplyToWorld(UWorld& World, const FSynthWeatherPreset& Preset, float SunPitchDeg, float SunYawDeg)
{
	int32 SunsFound = 0;
	for (TActorIterator<ADirectionalLight> It(&World); It; ++It)
	{
		UDirectionalLightComponent* Sun = Cast<UDirectionalLightComponent>(It->GetLightComponent());
		if (!Sun)
		{
			continue;
		}

		// Roll is meaningless for a directional light and the template ships a
		// non-zero one; carrying it forward would only confuse the recorded label.
		It->SetActorRotation(FRotator(SunPitchDeg, SunYawDeg, 0.0f));
		Sun->SetIntensity(Preset.SunIntensity);
		Sun->SetLightColor(Preset.SunColor);
		++SunsFound;
	}

	int32 FogFound = 0;
	for (TActorIterator<AExponentialHeightFog> It(&World); It; ++It)
	{
		if (UExponentialHeightFogComponent* Fog = It->GetComponent())
		{
			Fog->SetFogDensity(Preset.FogDensity);
			Fog->SetFogInscatteringColor(Preset.FogColor);
			++FogFound;
		}
	}

	if (SunsFound == 0)
	{
		UE_LOG(LogSynthic, Warning,
			TEXT("Weather: no directional light in the level - sun angle and intensity are not being varied."));
	}
	if (FogFound == 0)
	{
		UE_LOG(LogSynthic, Warning,
			TEXT("Weather: no ExponentialHeightFog in the level - fog is not being varied."));
	}
}

}	// namespace SynthWeather
