#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Synth/SynthTypes.h"
#include "SynthVehicle.generated.h"

class USceneComponent;
class UStaticMeshComponent;

/**
 * A vehicle that drives in a straight line at a commanded speed.
 *
 * Deliberately kinematic, not a Chaos vehicle: for synthetic training data the point
 * is that the speed is exactly known, and simulated suspension would only add drift
 * we would then have to measure. It still reports a *measured* velocity derived from
 * real displacement rather than echoing the setpoint, so the label stays honest if
 * this is ever swapped for a physics-driven vehicle.
 */
UCLASS()
class SYNTHICCAMERA_API ASynthVehicle : public AActor
{
	GENERATED_BODY()

public:
	ASynthVehicle();

	/** Rebuild visuals from Spec. Call after changing Spec, before the vehicle is seen. */
	UFUNCTION(BlueprintCallable, Category = "Synthic")
	void ApplySpec(const FSynthVehicleSpec& InSpec);

	/** Set the drive direction and speed. Direction is normalised; the actor yaws to face it. */
	UFUNCTION(BlueprintCallable, Category = "Synthic")
	void DriveAlong(const FVector& Direction, float SpeedKph);

	const FSynthVehicleSpec& GetSpec() const { return Spec; }

	/** Commanded speed in km/h - what we asked for. */
	float GetCommandedSpeedKph() const { return CommandedSpeedKph; }

	/** Velocity measured from actual displacement last tick, in cm/s. */
	FVector GetMeasuredVelocity() const { return MeasuredVelocity; }

	float GetMeasuredSpeedKph() const { return MeasuredVelocity.Size() * CmPerSecToKph; }

	/** Distance driven since DriveAlong was called, in centimetres. */
	float GetDistanceTravelledCm() const { return DistanceTravelledCm; }

	/**
	 * Local-space oriented bounds of the visible mesh, and the transform that puts them
	 * in world space. This is what the camera projects to get a 2D box.
	 */
	void GetVisualBounds(FTransform& OutBoxToWorld, FVector& OutLocalCenter, FVector& OutLocalExtent) const;

	static constexpr float CmPerSecToKph = 0.036f;		// cm/s -> km/h
	static constexpr float KphToCmPerSec = 1.0f / CmPerSecToKph;

protected:
	virtual void Tick(float DeltaSeconds) override;

private:
	/** Assemble Spec.Parts into child components. */
	void BuildAssembledGeometry();

	/** Scale a unit-cube proxy to the spec's dimensions when there are no parts. */
	void BuildProxyGeometry();

	/** Tint a component from the livery, scaled for wheels and tracks. */
	void ApplyLivery(UStaticMeshComponent& Component, float ColorScale) const;

	/** Cache local-space bounds covering every part, for the label's bounding box. */
	void CacheVisualBounds();


	/**
	 * Plain root at the ground contact point, with the mesh attached beneath it.
	 * The mesh must NOT be the root: a relative offset on a root component is the
	 * actor's world location, so seating the body on the ground would teleport the
	 * whole vehicle to the origin instead of raising its bodywork.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Synthic")
	TObjectPtr<USceneComponent> VehicleRoot;

	/** Single-box fallback. Unused once Parts are supplied. */
	UPROPERTY(VisibleAnywhere, Category = "Synthic")
	TObjectPtr<UStaticMeshComponent> VehicleMesh;

	/** Components built from Spec.Parts, rebuilt on every ApplySpec. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> PartMeshes;

	/** Union of every part in root-local space - what the 2D box is projected from. */
	FVector LocalBoundsCentre = FVector::ZeroVector;
	FVector LocalBoundsExtent = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, Category = "Synthic")
	FSynthVehicleSpec Spec;

	FVector DriveDirection = FVector::ForwardVector;
	float CommandedSpeedKph = 0.0f;

	FVector MeasuredVelocity = FVector::ZeroVector;
	float DistanceTravelledCm = 0.0f;
};
