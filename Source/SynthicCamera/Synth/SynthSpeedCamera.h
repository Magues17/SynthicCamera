#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SynthSpeedCamera.generated.h"

class ADirectionalLight;
class AExponentialHeightFog;
class ASynthVehicle;
class UStaticMeshComponent;
class UTextureCube;
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;
struct FSynthScreenBox;

/**
 * Roadside speed camera that emits a labelled training sample per vehicle pass:
 * one PNG plus one JSONL row of ground truth.
 *
 * It does not detect anything. In a synthetic pipeline the engine already knows the
 * vehicle's exact class, dimensions, pose and velocity, so the "measurement" is a
 * read, not an inference. That read is what a downstream model is later trained to
 * reproduce from the image alone.
 */
UCLASS()
class SYNTHICCAMERA_API ASynthSpeedCamera : public AActor
{
	GENERATED_BODY()

public:
	ASynthSpeedCamera();

	/** Directory this run's images and labels are written to. Valid after BeginPlay. */
	const FString& GetRunDirectory() const { return RunDirectory; }

	int32 GetCaptureCount() const { return CaptureCount; }

	/**
	 * Where the level placed this camera, captured before anything moves it.
	 *
	 * Randomisation perturbs the authored install rather than inventing a position
	 * from scratch, so the ranges stay meaningful if the road is moved or rotated.
	 */
	const FVector& GetInstallOrigin() const { return InstallOrigin; }

protected:
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostInitializeComponents() override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

public:
	/**
	 * Move the install: pole position, what it looks at, camera roll and lens.
	 *
	 * Position, aim direction and the trip plane are three coupled things - the plane
	 * has to sit where the lens is pointed or the vehicle is photographed off-frame,
	 * and a bounding box computed from a stale aim is confidently wrong rather than
	 * obviously broken. They are set together here so they cannot drift apart.
	 */
	UFUNCTION(BlueprintCallable, Category = "Synthic")
	void PlaceAt(const FVector& Position, const FVector& AimPoint, float RollDeg, float FieldOfView);

	/**
	 * Push the current atmospheric condition in. Called by the director once per pass;
	 * the ambient term is a per-capture post-process value, so it cannot live on a
	 * world actor the way sun and fog do.
	 */
	UFUNCTION(BlueprintCallable, Category = "Synthic")
	void SetSceneConditions(float InAmbientIntensity, const FString& InWeatherName);

	/** Capture and label a single pass. Public so a test or the director can force one. */
	UFUNCTION(BlueprintCallable, Category = "Synthic")
	bool CaptureVehicle(ASynthVehicle* Vehicle);

	/** Image resolution written to disk. */
	UPROPERTY(EditAnywhere, Category = "Synthic|Capture", meta = (ClampMin = "64"))
	int32 ImageWidth = 1280;

	UPROPERTY(EditAnywhere, Category = "Synthic|Capture", meta = (ClampMin = "64"))
	int32 ImageHeight = 720;

	UPROPERTY(EditAnywhere, Category = "Synthic|Capture", meta = (ClampMin = "5.0", ClampMax = "170.0"))
	float FieldOfViewDeg = 50.0f;

	/**
	 * Ambient fill applied through the capture's own post-process chain.
	 *
	 * A SkyLight is the physically correct source and contributes nothing to
	 * SceneCapture2D renders - measured, with real-time capture, Lumen, classic GI
	 * and a forced RecaptureSky all leaving shadowed faces at RGB (0,0,0). An
	 * ambient cubemap is a per-view deferred feature, so it does reach the capture.
	 * Zero disables it and every surface facing away from the sun goes black.
	 */
	UPROPERTY(EditAnywhere, Category = "Synthic|Capture", meta = (ClampMin = "0.0"))
	float AmbientIntensity = 1.0f;

	/**
	 * Draw the trip line across the carriageway. Debug scaffolding: it shows exactly
	 * where the shutter fires. Safe to leave on - the marker is hidden from scene
	 * captures, so it cannot reach the dataset either way.
	 */
	UPROPERTY(EditAnywhere, Category = "Synthic|Capture")
	bool bShowTripPlane = true;

	/**
	 * Smallest box, in pixels of area, that earns a label.
	 *
	 * Labelling every vehicle in shot means distant ones arrive as a handful of
	 * pixels. Those are not detections, they are noise with a confident annotation
	 * attached, and a detector penalised for missing them learns to chase specks.
	 * Set to 0 to label everything on screen.
	 */
	UPROPERTY(EditAnywhere, Category = "Synthic|Capture", meta = (ClampMin = "0.0"))
	float MinObjectAreaPx = 260.0f;

	/** Leave empty to name the run from the wall clock at BeginPlay. */
	UPROPERTY(EditAnywhere, Category = "Synthic|Capture")
	FString RunName;

private:
	/** Stretch the mast from the housing down to the road, whatever height it sits at. */
	void UpdateMastToGround();

	/**
	 * Fraction of the vehicle's silhouette the camera has an unobstructed line to.
	 *
	 * Traces from the lens to sample points on the vehicle's bounds and counts how
	 * many arrive. 1.0 is a clear view, 0.0 is fully hidden behind something. Both
	 * the vehicle and the camera are ignored by the trace, so anything hit is by
	 * definition an occluder.
	 */
	float MeasureVisibleFraction(const ASynthVehicle& Vehicle,
		const FVector& LocalCentre, const FVector& LocalExtent) const;

	void EnsureRenderTarget();
	void ApplyAmbientLighting();
	void ResolveRunDirectory();
	FMatrix BuildViewProjectionMatrix() const;
	/**
	 * One row describing the whole frame, with an entry for every vehicle visible in
	 * it - not just the one that tripped the shutter.
	 *
	 * With several vehicles on the road a frame routinely contains more than one. A
	 * row naming only the trigger would leave the others as unlabelled objects, which
	 * teaches a detector that those pixels are background.
	 */
	TSharedRef<class FJsonObject> BuildFrameLabel(const ASynthVehicle& Trigger,
		const TArray<FColor>& Pixels, const FString& ImageFileName) const;

	/**
	 * How far the object's mean colour sits from the background just outside it, in
	 * 0-255 units, measured on the frame that was actually rendered.
	 *
	 * A box can be geometrically perfect while the vehicle has washed out into dust.
	 * Recording separability alongside the box is what lets a consumer drop those
	 * without re-reading every image - the geometry alone cannot tell them apart.
	 */
	static float MeasureContrast(const TArray<FColor>& Pixels, int32 Width, int32 Height,
		const struct FSynthScreenBox& Box);

	/** One object entry, or null when the vehicle is not on screen. */
	TSharedPtr<class FJsonObject> BuildObjectEntry(const ASynthVehicle& Vehicle,
		const TArray<FColor>& Pixels, bool bTriggered) const;

	UPROPERTY(VisibleAnywhere, Category = "Synthic")
	TObjectPtr<USceneCaptureComponent2D> CaptureComponent;

	/**
	 * Defines the trip line: its location is a point on the plane and its forward vector
	 * is the plane normal. Place it across the carriageway pointing along the traffic.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Synthic")
	TObjectPtr<USceneComponent> CapturePoint;

	/**
	 * Mast and housing, so the install is actually visible in the level - a bare
	 * capture component shows as nothing at all in the viewport.
	 *
	 * Both are children of the camera rather than scenery placed beside it, so they
	 * follow it when the pose is randomised. Both are hidden from scene captures: the
	 * housing sits exactly where the lens is and would otherwise fill every frame.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Synthic")
	TObjectPtr<UStaticMeshComponent> MastMesh;

	UPROPERTY(VisibleAnywhere, Category = "Synthic")
	TObjectPtr<UStaticMeshComponent> HousingMesh;

	/** Visual marker for the trip plane. Never seen by the capture. */
	UPROPERTY(VisibleAnywhere, Category = "Synthic")
	TObjectPtr<UStaticMeshComponent> TripPlaneMesh;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> RenderTarget;

	UPROPERTY(Transient)
	TWeakObjectPtr<ADirectionalLight> SunLight;

	UPROPERTY(Transient)
	TWeakObjectPtr<AExponentialHeightFog> HeightFog;

	UPROPERTY(Transient)
	TObjectPtr<UTextureCube> AmbientCubemap;

	/** Set by the director each pass; "unspecified" when nothing is randomising. */
	FString WeatherName = TEXT("unspecified");

	/**
	 * Signed distance of each live vehicle to the trip plane, as of last tick. Rebuilt
	 * every tick from the world, so destroyed vehicles drop out without bookkeeping.
	 */
	TMap<TWeakObjectPtr<ASynthVehicle>, float> PreviousSignedDistance;

	FVector InstallOrigin = FVector::ZeroVector;
	FString RunDirectory;
	FString LabelFilePath;
	int32 CaptureCount = 0;
};
