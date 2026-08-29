#include "Synth/SynthSpeedCamera.h"

#include "SynthicCamera.h"
#include "Synth/SynthDataset.h"
#include "Synth/SynthProjection.h"
#include "Synth/SynthVehicle.h"

#include "Camera/CameraTypes.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Dom/JsonObject.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/TextureCube.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"

namespace
{
	/** Pull the render target down to the CPU as opaque BGRA. */
	bool ReadRenderTargetPixels(UTextureRenderTarget2D& Target, TArray<FColor>& OutPixels)
	{
		FTextureRenderTargetResource* Resource = Target.GameThread_GetRenderTargetResource();
		if (!Resource)
		{
			UE_LOG(LogSynthic, Error, TEXT("Capture: render target has no resource; nothing to read."));
			return false;
		}

		// SCS_FinalColorLDR is already gamma-encoded, so do not convert again.
		FReadSurfaceDataFlags Flags(RCM_UNorm, CubeFace_MAX);
		Flags.SetLinearToGamma(false);

		if (!Resource->ReadPixels(OutPixels, Flags))
		{
			UE_LOG(LogSynthic, Error, TEXT("Capture: ReadPixels failed."));
			return false;
		}

		// The scene capture leaves alpha undefined; a PNG with it would be part-transparent.
		for (FColor& Pixel : OutPixels)
		{
			Pixel.A = 255;
		}
		return true;
	}

	TSharedPtr<FJsonObject> VectorToJson(const FVector& V)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("x"), V.X);
		Object->SetNumberField(TEXT("y"), V.Y);
		Object->SetNumberField(TEXT("z"), V.Z);
		return Object;
	}
}

ASynthSpeedCamera::ASynthSpeedCamera()
{
	PrimaryActorTick.bCanEverTick = true;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("CameraRoot"));
	SetRootComponent(Root);

	CaptureComponent = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("SceneCapture"));
	CaptureComponent->SetupAttachment(Root);
	CaptureComponent->bCaptureEveryFrame = false;
	CaptureComponent->bCaptureOnMovement = false;
	CaptureComponent->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;

	CapturePoint = CreateDefaultSubobject<USceneComponent>(TEXT("CapturePoint"));
	CapturePoint->SetupAttachment(Root);
}

void ASynthSpeedCamera::BeginPlay()
{
	Super::BeginPlay();

	ResolveRunDirectory();
	EnsureRenderTarget();

	CaptureComponent->FOVAngle = FieldOfViewDeg;
	ApplyAmbientLighting();

	for (TActorIterator<ADirectionalLight> It(GetWorld()); It; ++It)
	{
		SunLight = *It;	// first directional light is the sun; scene has exactly one
		break;
	}

	for (TActorIterator<AExponentialHeightFog> It(GetWorld()); It; ++It)
	{
		HeightFog = *It;
		break;
	}

	UE_LOG(LogSynthic, Log, TEXT("Speed camera armed. Writing %dx%d samples to '%s'."),
		ImageWidth, ImageHeight, *RunDirectory);
}

void ASynthSpeedCamera::ResolveRunDirectory()
{
	if (RunName.IsEmpty())
	{
		RunName = FDateTime::Now().ToString(TEXT("run_%Y%m%d_%H%M%S"));
	}

	RunDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SynthData"), RunName);
	LabelFilePath = FPaths::Combine(RunDirectory, TEXT("labels.jsonl"));
}

void ASynthSpeedCamera::ApplyAmbientLighting()
{
	if (AmbientIntensity <= 0.0f)
	{
		return;
	}

	static const TCHAR* const AmbientCubemapPath =
		TEXT("/Engine/MapTemplates/Sky/DaylightAmbientCubemap.DaylightAmbientCubemap");

	if (!AmbientCubemap)
	{
		AmbientCubemap = LoadObject<UTextureCube>(nullptr, AmbientCubemapPath);
	}

	if (!AmbientCubemap)
	{
		UE_LOG(LogSynthic, Warning,
			TEXT("Ambient cubemap '%s' failed to load; shadowed surfaces will render black."),
			AmbientCubemapPath);
		return;
	}

	// The cubemap itself carries no override flag - only tint and intensity do.
	FPostProcessSettings& Settings = CaptureComponent->PostProcessSettings;
	Settings.AmbientCubemap = AmbientCubemap;
	Settings.bOverride_AmbientCubemapIntensity = true;
	Settings.AmbientCubemapIntensity = AmbientIntensity;
	Settings.bOverride_AmbientCubemapTint = true;
	Settings.AmbientCubemapTint = FLinearColor::White;

}

void ASynthSpeedCamera::SetSceneConditions(float InAmbientIntensity, const FString& InWeatherName)
{
	AmbientIntensity = InAmbientIntensity;
	WeatherName = InWeatherName;
	ApplyAmbientLighting();
}

void ASynthSpeedCamera::EnsureRenderTarget()
{
	if (RenderTarget)
	{
		return;
	}

	RenderTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("SynthCaptureTarget"));
	RenderTarget->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA8;
	RenderTarget->ClearColor = FLinearColor::Black;
	RenderTarget->bAutoGenerateMips = false;
	RenderTarget->InitAutoFormat(ImageWidth, ImageHeight);
	RenderTarget->UpdateResourceImmediate(true);

	CaptureComponent->TextureTarget = RenderTarget;
}

/**
 * Fire on the frame a vehicle crosses the trip plane.
 *
 * An overlap volume would be the obvious way to do this and is the wrong one: at
 * 110 km/h a vehicle advances ~51cm per frame, and faster traffic simply teleports
 * across a box of any sane thickness and is never photographed. A signed-distance
 * sign change cannot be tunnelled through at any speed.
 *
 * The shutter still fires up to one frame late, so the vehicle sits slightly past
 * the plane - that is framing jitter, not label error, since the bounding box is
 * projected from where the vehicle actually is. Real roadside cameras jitter too.
 */
void ASynthSpeedCamera::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	const FVector PlanePoint = CapturePoint->GetComponentLocation();
	const FVector PlaneNormal = CapturePoint->GetForwardVector();

	TMap<TWeakObjectPtr<ASynthVehicle>, float> CurrentSignedDistance;

	for (TActorIterator<ASynthVehicle> It(GetWorld()); It; ++It)
	{
		ASynthVehicle* Vehicle = *It;
		const float Distance = static_cast<float>(
			FVector::DotProduct(Vehicle->GetActorLocation() - PlanePoint, PlaneNormal));

		const float* Previous = PreviousSignedDistance.Find(Vehicle);
		if (Previous && *Previous < 0.0f && Distance >= 0.0f)
		{
			// ponytail: one sample per crossing; burst capture is a loop around this call.
			CaptureVehicle(Vehicle);
		}

		CurrentSignedDistance.Add(Vehicle, Distance);
	}

	// Rebuilt wholesale, so vehicles the director destroyed simply stop being tracked.
	PreviousSignedDistance = MoveTemp(CurrentSignedDistance);
}

FMatrix ASynthSpeedCamera::BuildViewProjectionMatrix() const
{
	FMinimalViewInfo View;
	View.Location = CaptureComponent->GetComponentLocation();
	View.Rotation = CaptureComponent->GetComponentRotation();
	View.FOV = CaptureComponent->FOVAngle;
	View.AspectRatio = static_cast<float>(ImageWidth) / static_cast<float>(ImageHeight);
	View.bConstrainAspectRatio = true;
	View.ProjectionMode = ECameraProjectionMode::Perspective;

	FMatrix ViewMatrix;
	FMatrix ProjectionMatrix;
	FMatrix ViewProjectionMatrix;
	UGameplayStatics::GetViewProjectionMatrix(View, ViewMatrix, ProjectionMatrix, ViewProjectionMatrix);
	return ViewProjectionMatrix;
}

bool ASynthSpeedCamera::CaptureVehicle(ASynthVehicle* Vehicle)
{
	if (!Vehicle || !RenderTarget)
	{
		UE_LOG(LogSynthic, Error, TEXT("CaptureVehicle: no vehicle or no render target; skipping sample."));
		return false;
	}

	CaptureComponent->CaptureScene();

	TArray<FColor> Pixels;
	if (!ReadRenderTargetPixels(*RenderTarget, Pixels))
	{
		return false;
	}

	FTransform BoxToWorld;
	FVector LocalCentre;
	FVector LocalExtent;
	Vehicle->GetVisualBounds(BoxToWorld, LocalCentre, LocalExtent);

	const FSynthScreenBox Box = SynthProjection::OrientedBoxToScreenBounds(
		BuildViewProjectionMatrix(), BoxToWorld, LocalCentre, LocalExtent, ImageWidth, ImageHeight);

	const FString ImageFileName = FString::Printf(TEXT("%s_%06d.png"), *RunName, CaptureCount);

	if (!SynthDataset::SavePng(FPaths::Combine(RunDirectory, ImageFileName), Pixels, ImageWidth, ImageHeight))
	{
		return false;	// no image means the label would dangle - drop the whole sample
	}

	if (!SynthDataset::AppendJsonLine(LabelFilePath, BuildLabel(*Vehicle, Box, ImageFileName)))
	{
		return false;
	}

	++CaptureCount;

	UE_LOG(LogSynthic, Log, TEXT("Captured %s %s at %.1f km/h -> %s (bbox %s)"),
		*Vehicle->GetSpec().Make, *Vehicle->GetSpec().Model, Vehicle->GetMeasuredSpeedKph(),
		*ImageFileName, Box.bValid ? TEXT("ok") : TEXT("INVALID"));

	return true;
}

TSharedRef<FJsonObject> ASynthSpeedCamera::BuildLabel(const ASynthVehicle& Vehicle,
	const FSynthScreenBox& Box, const FString& ImageFileName) const
{
	const FSynthVehicleSpec& Spec = Vehicle.GetSpec();
	const FVector CameraLocation = CaptureComponent->GetComponentLocation();
	const FVector VehicleLocation = Vehicle.GetActorLocation();
	const FVector Velocity = Vehicle.GetMeasuredVelocity();

	const FVector ToVehicle = VehicleLocation - CameraLocation;
	const double RangeCm = ToVehicle.Size();
	const FVector LineOfSight = ToVehicle.GetSafeNormal();

	// A real roadside radar reads only the velocity component along its line of sight,
	// so an angled install under-reads by cos(theta). Recording both the true speed and
	// the cosine-affected reading is what lets this data line up with real camera logs.
	const double RadialSpeedCmS = FMath::Abs(FVector::DotProduct(Velocity, LineOfSight));
	const FVector VelocityDirection = Velocity.GetSafeNormal();
	const double CosTheta = VelocityDirection.IsNearlyZero()
		? 1.0
		: FMath::Clamp(FMath::Abs(FVector::DotProduct(VelocityDirection, LineOfSight)), 0.0, 1.0);

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("frame_id"), CaptureCount);
	Root->SetStringField(TEXT("image_file"), ImageFileName);
	Root->SetNumberField(TEXT("image_width"), ImageWidth);
	Root->SetNumberField(TEXT("image_height"), ImageHeight);
	Root->SetNumberField(TEXT("sim_time_s"), GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0);

	TSharedPtr<FJsonObject> VehicleJson = MakeShared<FJsonObject>();
	VehicleJson->SetStringField(TEXT("make"), Spec.Make);
	VehicleJson->SetStringField(TEXT("model"), Spec.Model);
	VehicleJson->SetStringField(TEXT("class"),
		StaticEnum<ESynthVehicleClass>()->GetNameStringByValue(static_cast<int64>(Spec.VehicleClass)));
	VehicleJson->SetNumberField(TEXT("axle_count"), Spec.AxleCount);
	VehicleJson->SetBoolField(TEXT("tracked"), Spec.bTracked);
	VehicleJson->SetStringField(TEXT("livery"), Spec.LiveryName);
	VehicleJson->SetObjectField(TEXT("livery_rgb"), VectorToJson(
		FVector(Spec.LiveryColor.R, Spec.LiveryColor.G, Spec.LiveryColor.B)));
	VehicleJson->SetObjectField(TEXT("dimensions_cm"), VectorToJson(Spec.DimensionsCm));
	Root->SetObjectField(TEXT("vehicle"), VehicleJson);

	TSharedPtr<FJsonObject> Kinematics = MakeShared<FJsonObject>();
	Kinematics->SetNumberField(TEXT("speed_kph_true"), Vehicle.GetMeasuredSpeedKph());
	Kinematics->SetNumberField(TEXT("speed_kph_commanded"), Vehicle.GetCommandedSpeedKph());
	Kinematics->SetNumberField(TEXT("speed_kph_radar"), RadialSpeedCmS * ASynthVehicle::CmPerSecToKph);
	Kinematics->SetNumberField(TEXT("cosine_angle_deg"), FMath::RadiansToDegrees(FMath::Acos(CosTheta)));
	Kinematics->SetObjectField(TEXT("velocity_cms"), VectorToJson(Velocity));
	Root->SetObjectField(TEXT("kinematics"), Kinematics);

	TSharedPtr<FJsonObject> Geometry = MakeShared<FJsonObject>();
	Geometry->SetObjectField(TEXT("world_position"), VectorToJson(VehicleLocation));
	Geometry->SetObjectField(TEXT("world_rotation_pyr"), VectorToJson(
		FVector(Vehicle.GetActorRotation().Pitch, Vehicle.GetActorRotation().Yaw, Vehicle.GetActorRotation().Roll)));
	Geometry->SetNumberField(TEXT("range_cm"), RangeCm);
	Geometry->SetBoolField(TEXT("bbox_valid"), Box.bValid);
	Geometry->SetBoolField(TEXT("bbox_clipped"), Box.bClipped);

	if (Box.bValid)
	{
		TSharedPtr<FJsonObject> BBox = MakeShared<FJsonObject>();
		BBox->SetNumberField(TEXT("x"), Box.MinX);
		BBox->SetNumberField(TEXT("y"), Box.MinY);
		BBox->SetNumberField(TEXT("w"), Box.Width());
		BBox->SetNumberField(TEXT("h"), Box.Height());
		Geometry->SetObjectField(TEXT("bbox_xywh"), BBox);
	}
	Root->SetObjectField(TEXT("geometry"), Geometry);

	TSharedPtr<FJsonObject> CameraJson = MakeShared<FJsonObject>();
	CameraJson->SetObjectField(TEXT("position"), VectorToJson(CameraLocation));
	CameraJson->SetObjectField(TEXT("rotation_pyr"), VectorToJson(FVector(
		CaptureComponent->GetComponentRotation().Pitch,
		CaptureComponent->GetComponentRotation().Yaw,
		CaptureComponent->GetComponentRotation().Roll)));
	CameraJson->SetNumberField(TEXT("fov_deg"), CaptureComponent->FOVAngle);
	Root->SetObjectField(TEXT("camera"), CameraJson);

	// Every field here is read back out of the world at capture time rather than
	// echoing what the director asked for. A setter that silently fails then shows up
	// as a flat column in the dataset instead of a plausible lie in every row.
	TSharedPtr<FJsonObject> Scene = MakeShared<FJsonObject>();
	Scene->SetStringField(TEXT("weather"), WeatherName);
	Scene->SetNumberField(TEXT("ambient_intensity"), AmbientIntensity);

	if (const ADirectionalLight* Sun = SunLight.Get())
	{
		Scene->SetNumberField(TEXT("sun_pitch_deg"), Sun->GetActorRotation().Pitch);
		Scene->SetNumberField(TEXT("sun_yaw_deg"), Sun->GetActorRotation().Yaw);

		if (const UDirectionalLightComponent* SunComponent =
			Cast<UDirectionalLightComponent>(Sun->GetLightComponent()))
		{
			Scene->SetNumberField(TEXT("sun_intensity"), SunComponent->Intensity);
		}
	}

	if (const AExponentialHeightFog* Fog = HeightFog.Get())
	{
		if (const UExponentialHeightFogComponent* FogComponent = Fog->GetComponent())
		{
			Scene->SetNumberField(TEXT("fog_density"), FogComponent->FogDensity);
		}
	}
	Root->SetObjectField(TEXT("scene"), Scene);

	return Root;
}
