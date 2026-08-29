#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SynthSpeedCamera.generated.h"

class ADirectionalLight;
class ASynthVehicle;
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

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

public:
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

	/** Leave empty to name the run from the wall clock at BeginPlay. */
	UPROPERTY(EditAnywhere, Category = "Synthic|Capture")
	FString RunName;

private:
	void EnsureRenderTarget();
	void ResolveRunDirectory();
	FMatrix BuildViewProjectionMatrix() const;
	TSharedRef<class FJsonObject> BuildLabel(const ASynthVehicle& Vehicle, const FSynthScreenBox& Box,
		const FString& ImageFileName) const;

	UPROPERTY(VisibleAnywhere, Category = "Synthic")
	TObjectPtr<USceneCaptureComponent2D> CaptureComponent;

	/**
	 * Defines the trip line: its location is a point on the plane and its forward vector
	 * is the plane normal. Place it across the carriageway pointing along the traffic.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Synthic")
	TObjectPtr<USceneComponent> CapturePoint;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> RenderTarget;

	UPROPERTY(Transient)
	TWeakObjectPtr<ADirectionalLight> SunLight;

	/**
	 * Signed distance of each live vehicle to the trip plane, as of last tick. Rebuilt
	 * every tick from the world, so destroyed vehicles drop out without bookkeeping.
	 */
	TMap<TWeakObjectPtr<ASynthVehicle>, float> PreviousSignedDistance;

	FString RunDirectory;
	FString LabelFilePath;
	int32 CaptureCount = 0;
};
