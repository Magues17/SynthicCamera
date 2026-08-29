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
