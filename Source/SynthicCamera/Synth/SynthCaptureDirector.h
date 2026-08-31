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
 * Seeded on purpose: a dataset you cannot regenerate is one you cannot debug when a
 * model trained on it behaves oddly.
 *
 * The seed fixes every decision - vehicle, speed, lane, weather, sun angle, camera
 * pose - so two runs are parameter-identical. They are NOT pixel-identical: the
 * shutter fires up to one frame after the trip plane is crossed, and frame duration
 * varies between runs, so a vehicle can sit a few centimetres further on. Labels
 * still match their own image exactly, because the box is projected from where the
 * vehicle actually is. Use the seed to reproduce conditions, not to diff renders.
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

	/**
	 * Give every vehicle class the same number of passes instead of drawing one at
	 * random each time.
	 *
	 * Uniform draws leave the tail classes starved - thirty passes over ten archetypes
	 * gave one class a single sample and another six, and a class with two archetypes
	 * drew twice as often as one with a single archetype. Off models realistic traffic
	 * where cars vastly outnumber tanks; on gives a model enough of every class to
	 * learn it.
	 */
	UPROPERTY(EditAnywhere, Category = "Synthic|Dataset")
	bool bBalanceClasses = true;

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

	/**
	 * How many vehicles may be on the road at once.
	 *
	 * One at a time meant nothing could ever occlude anything, so the visibility
	 * fields were constant and carried no information. It also wasted the road: a
	 * vehicle drove 150m to produce a single frame.
	 */
	UPROPERTY(EditAnywhere, Category = "Synthic|Traffic", meta = (ClampMin = "1"))
	int32 MaxConcurrentVehicles = 4;

	/** Minimum gap before the next vehicle is released, in centimetres. */
	UPROPERTY(EditAnywhere, Category = "Synthic|Traffic", meta = (ClampMin = "100.0"))
	float MinHeadwayCm = 2600.0f;

	/** Lane centres relative to the road centreline. Empty means a single lane. */
	UPROPERTY(EditAnywhere, Category = "Synthic|Traffic")
	TArray<float> LaneOffsetsCm;

	/** Lateral wander within a lane, in centimetres. */
	UPROPERTY(EditAnywhere, Category = "Synthic|Traffic")
	float LaneJitterCm = 55.0f;

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

	/**
	 * Catalog index to use for each pass, balanced across classes.
	 *
	 * Classes are filled round-robin so each gets NumPasses/classes samples (the
	 * remainder going to the earliest classes), archetypes within a class are cycled
	 * so a class with several does not favour one, and the result is shuffled so the
	 * order vehicles arrive in does not correlate with the weather and speed drawn
	 * alongside them. Static and pure so the balance can be checked without a world.
	 */
	static TArray<int32> BuildBalancedSchedule(const TArray<ESynthVehicleClass>& EntryClasses,
		int32 NumPasses, FRandomStream& Stream);

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	/** Realistic proxy archetypes, used when no catalog is authored. */
	static TArray<FSynthVehicleSpec> MakeDefaultCatalog();

	/** Re-roll sun angle and weather, and push ambient and pose to every camera. */
	void RandomiseScene(bool bAllowCameraMove);

	/**
	 * Push the same ambient the captures use into the level's post-process volumes.
	 *
	 * Without this the viewport keeps whatever fixed value the level was built with
	 * while the captures vary per pass, so what a person sees on screen is lit
	 * differently from what the dataset receives. That is not cosmetic: it made a
	 * correctly-placed rock 24m off the road read as a wall the traffic drove through.
	 */
	void MatchViewportAmbient(float AmbientIntensity);

	/** Draw one camera install from CameraPose and apply it. */
	void RandomiseCameraPose(ASynthSpeedCamera& Camera);

	/** Retire anything that has driven its distance. */
	void RetireFinishedVehicles();

	/** True once the last release has opened enough gap for the next. */
	bool HasHeadway() const;

	/** Spawn the next scheduled vehicle at the start line. Returns false when done. */
	bool DispatchNextVehicle();

	/** Everything currently on the road, oldest first. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<ASynthVehicle>> ActiveVehicles;

	/** Most recent release, used to hold the headway gap. */
	UPROPERTY(Transient)
	TWeakObjectPtr<ASynthVehicle> LastReleased;

	bool bRunFinished = false;

	FRandomStream Stream;

	/** One catalog index per pass, decided up front at BeginPlay. */
	TArray<int32> PassSchedule;
	int32 PassesDispatched = 0;
};
