#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Math/RandomStream.h"
#include "Synth/SynthTypes.h"
#include "Synth/SynthWeather.h"
#include "SynthCaptureDirector.generated.h"

class ASynthSpeedCamera;
class ASynthVehicle;

/**
 * Runs the dataset: sends one vehicle at a time down the road with a randomised spec,
 * speed and lane offset, until NumPasses samples have been generated.
 *
 * The director's own transform is the start line - its forward vector is the direction
 * of travel and its right vector is the lane axis. Place it at the far end of the road
 * pointing at the camera.
 *
 * Seeded on purpose: a dataset you cannot regenerate byte-for-byte is a dataset you
 * cannot debug when a model trained on it behaves oddly.
 */
UCLASS()
class SYNTHICCAMERA_API ASynthCaptureDirector : public AActor
{
	GENERATED_BODY()

public:
	ASynthCaptureDirector();

	/** Vehicle archetypes to draw from. Populated with proxy specs if left empty. */
	UPROPERTY(EditAnywhere, Category = "Synthic|Dataset")
	TArray<FSynthVehicleSpec> Catalog;

	UPROPERTY(EditAnywhere, Category = "Synthic|Dataset", meta = (ClampMin = "1"))
	int32 NumPasses = 20;

	UPROPERTY(EditAnywhere, Category = "Synthic|Dataset")
	int32 RandomSeed = 1337;

	UPROPERTY(EditAnywhere, Category = "Synthic|Traffic", meta = (ClampMin = "1.0"))
	float MinSpeedKph = 40.0f;

	UPROPERTY(EditAnywhere, Category = "Synthic|Traffic", meta = (ClampMin = "1.0"))
	float MaxSpeedKph = 110.0f;

	/**
	 * How far a vehicle drives before it is recycled, in centimetres. Just past the
	 * camera plus margin - every extra metre is wall-clock spent driving into empty
	 * desert after the photo has already been taken.
	 */
	UPROPERTY(EditAnywhere, Category = "Synthic|Traffic", meta = (ClampMin = "100.0"))
	float TravelDistanceCm = 15000.0f;

	/** Lateral spread about the start line, in centimetres - lane position variation. */
	UPROPERTY(EditAnywhere, Category = "Synthic|Traffic")
	float LaneJitterCm = 150.0f;

	/** Re-roll sun angle and weather before every pass. Off gives one fixed condition. */
	UPROPERTY(EditAnywhere, Category = "Synthic|Randomisation")
	bool bRandomiseScene = true;

	/**
	 * Sun elevation range in degrees. Negative points downward, so -80 is near
	 * overhead and -12 is a low raking sun. The low end is the hard case worth
	 * covering: long shadows, heavy glare, and the vehicle lit from one side only.
	 */
	UPROPERTY(EditAnywhere, Category = "Synthic|Randomisation")
	FVector2D SunPitchRangeDeg = FVector2D(-80.0, -12.0);

	/** Sun compass range. The full circle - it decides which faces are lit at all. */
	UPROPERTY(EditAnywhere, Category = "Synthic|Randomisation")
	FVector2D SunYawRangeDeg = FVector2D(0.0, 360.0);

	/**
	 * Conditions to draw from, sampled uniformly. Repeat an entry to weight it - a
	 * mix of three Clear to one DustStorm is realistic, but an even spread gives a
	 * model more of the rare cases to learn from, which is usually what you want.
	 */
	UPROPERTY(EditAnywhere, Category = "Synthic|Randomisation")
	TArray<ESynthWeather> WeatherMix;

	/** Per-parameter spread within a condition, as a fraction. 0.25 = +/-25%. */
	UPROPERTY(EditAnywhere, Category = "Synthic|Randomisation", meta = (ClampMin = "0.0", ClampMax = "0.9"))
	float WeatherJitter = 0.25f;

	/**
	 * Re-site the camera before every pass. Off keeps whatever the level was built
	 * with, which is the right choice when modelling one specific real install.
	 */
	UPROPERTY(EditAnywhere, Category = "Synthic|Randomisation")
	bool bRandomiseCameraPose = true;

	UPROPERTY(EditAnywhere, Category = "Synthic|Randomisation")
	FSynthCameraPoseRanges CameraPose;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	/** Realistic proxy archetypes, used when no catalog is authored. */
	static TArray<FSynthVehicleSpec> MakeDefaultCatalog();

	/** Re-roll sun angle and weather, and push ambient and pose to every camera. */
	void RandomiseScene();

	/** Draw one camera install from CameraPose and apply it. */
	void RandomiseCameraPose(ASynthSpeedCamera& Camera);

	/** Spawn the next randomised vehicle at the start line. Returns false when done. */
	bool DispatchNextVehicle();

	UPROPERTY(Transient)
	TObjectPtr<ASynthVehicle> ActiveVehicle;

	FRandomStream Stream;
	int32 PassesDispatched = 0;
};
