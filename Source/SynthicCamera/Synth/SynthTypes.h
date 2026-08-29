#pragma once

#include "CoreMinimal.h"
#include "Engine/StaticMesh.h"
#include "SynthTypes.generated.h"

/** Coarse vehicle category. This is the classification label a downstream model predicts. */
UENUM(BlueprintType)
enum class ESynthVehicleClass : uint8
{
	Unknown			UMETA(DisplayName = "Unknown"),
	LightUtility	UMETA(DisplayName = "Light Utility"),	// jeep / technical
	CargoTruck		UMETA(DisplayName = "Cargo Truck"),		// 4-ton, 8-ton
	APC				UMETA(DisplayName = "APC"),				// wheeled armoured personnel carrier
	IFV				UMETA(DisplayName = "IFV"),				// tracked infantry fighting vehicle
	MBT				UMETA(DisplayName = "MBT")				// main battle tank
};

/**
 * Ranges the camera install is drawn from, in centimetres and degrees.
 *
 * Grouped rather than spread across the director because they are only meaningful
 * together: height without lateral offset is not a viewing geometry.
 */
USTRUCT(BlueprintType)
struct FSynthCameraPoseRanges
{
	GENERATED_BODY()

	/** Pole height above the road. Real roadside installs sit around 3-6m. */
	UPROPERTY(EditAnywhere, Category = "Synthic")
	FVector2D HeightCm = FVector2D(300.0, 650.0);

	/** Distance from the road centreline - an absolute offset, not a jitter, so the
	 *  camera can never be randomised onto the carriageway. Sign is picked per pass,
	 *  putting it on either verge to see the opposite flank of the vehicle. */
	UPROPERTY(EditAnywhere, Category = "Synthic")
	FVector2D LateralOffsetCm = FVector2D(450.0, 1000.0);

	/** Shift along the road from where the level placed the pole. */
	UPROPERTY(EditAnywhere, Category = "Synthic")
	FVector2D AlongRoadJitterCm = FVector2D(-800.0, 800.0);

	/** How far up-road the camera looks - the capture range. 18-40m is typical. */
	UPROPERTY(EditAnywhere, Category = "Synthic")
	FVector2D AimDistanceCm = FVector2D(1800.0, 4000.0);

	/** Height of the aim point: roughly bonnet to cab level. */
	UPROPERTY(EditAnywhere, Category = "Synthic")
	FVector2D AimHeightCm = FVector2D(80.0, 220.0);

	/** Lateral spread of the aim point about the centreline. */
	UPROPERTY(EditAnywhere, Category = "Synthic")
	FVector2D AimLateralCm = FVector2D(-150.0, 150.0);

	UPROPERTY(EditAnywhere, Category = "Synthic")
	FVector2D FieldOfViewDeg = FVector2D(35.0, 60.0);

	/** Real poles are never perfectly plumb, and a level horizon in every single
	 *  frame is a cue a model will happily learn instead of the vehicle. */
	UPROPERTY(EditAnywhere, Category = "Synthic")
	FVector2D RollDeg = FVector2D(-4.0, 4.0);
};

/**
 * One entry in the vehicle catalog: everything the dataset should know about a
 * vehicle before it ever moves. Mesh is a soft reference and may be null - when it
 * is, the vehicle builds a proxy box from Dimensions instead. That is deliberate:
 * the mesh is a data field, so swapping proxy geometry for real Fab assets is a
 * catalog edit, not a code change.
 */
USTRUCT(BlueprintType)
struct FSynthVehicleSpec
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Identity")
	FString Make = TEXT("Unknown");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Identity")
	FString Model = TEXT("Unknown");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Identity")
	ESynthVehicleClass VehicleClass = ESynthVehicleClass::Unknown;

	/** Length (X), width (Y), height (Z) in centimetres - UE's world unit. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Geometry")
	FVector DimensionsCm = FVector(500.0, 220.0, 200.0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Geometry")
	int32 AxleCount = 2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Geometry")
	bool bTracked = false;

	/** Null = build a proxy box from DimensionsCm. Set this to swap in a real asset. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance")
	TSoftObjectPtr<UStaticMesh> Mesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance")
	FLinearColor LiveryColor = FLinearColor(0.21f, 0.22f, 0.15f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance")
	FString LiveryName = TEXT("olive-drab");
};
