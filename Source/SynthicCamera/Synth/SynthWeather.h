#pragma once

#include "CoreMinimal.h"
#include "SynthWeather.generated.h"

class UWorld;

/**
 * Atmospheric condition. This is a stratification label - it lets you ask "how does
 * the model do on dust storms" rather than only seeing an aggregate score.
 */
UENUM(BlueprintType)
enum class ESynthWeather : uint8
{
	Clear		UMETA(DisplayName = "Clear"),
	Hazy		UMETA(DisplayName = "Hazy"),
	Overcast	UMETA(DisplayName = "Overcast"),
	DustStorm	UMETA(DisplayName = "Dust Storm")
};

/**
 * The photometric settings that make up one condition.
 *
 * Deliberately limited to things that actually reach a SceneCapture2D: direct sun,
 * height fog, and the capture's own ambient term. Sky-light-driven effects were
 * measured to contribute nothing to captures, so a "weather" model built on them
 * would vary the labels without varying a single pixel.
 */
USTRUCT(BlueprintType)
struct FSynthWeatherPreset
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Synthic")
	float SunIntensity = 6.0f;

	UPROPERTY(EditAnywhere, Category = "Synthic")
	FLinearColor SunColor = FLinearColor(1.0f, 0.95f, 0.88f);

	UPROPERTY(EditAnywhere, Category = "Synthic")
	float FogDensity = 0.02f;

	UPROPERTY(EditAnywhere, Category = "Synthic")
	FLinearColor FogColor = FLinearColor(0.45f, 0.50f, 0.60f);

	/** Drives the capture's ambient cubemap - how much fill shadowed surfaces get. */
	UPROPERTY(EditAnywhere, Category = "Synthic")
	float AmbientIntensity = 4.0f;
};

namespace SynthWeather
{
	/** Base settings for a condition. Pure - no world access, no side effects. */
	FSynthWeatherPreset GetPreset(ESynthWeather Weather);

	FString GetName(ESynthWeather Weather);

	/**
	 * Scale every value by an independent factor in [1-Amount, 1+Amount].
	 *
	 * Four discrete presets would produce four tight clusters for a model to overfit
	 * to. Jittering each parameter separately fills the space between them, which is
	 * the entire point of randomising rather than enumerating.
	 */
	FSynthWeatherPreset Jitter(const FSynthWeatherPreset& Preset, const FRandomStream& Stream, float Amount);

	/** The one function here that touches the world: sets sun angle, sun and fog. */
	void ApplyToWorld(UWorld& World, const FSynthWeatherPreset& Preset, float SunPitchDeg, float SunYawDeg);
}
