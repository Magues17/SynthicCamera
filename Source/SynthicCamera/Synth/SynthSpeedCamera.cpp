#include "Synth/SynthSpeedCamera.h"

#include "SynthicCamera.h"
#include "Synth/SynthDataset.h"
#include "Synth/SynthProjection.h"
#include "Synth/SynthVehicle.h"

#include "Camera/CameraTypes.h"
#include "Components/BoxComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Dom/JsonObject.h"
#include "Engine/DirectionalLight.h"
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
	PrimaryActorTick.bCanEverTick = false;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("CameraRoot"));
	SetRootComponent(Root);

	CaptureComponent = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("SceneCapture"));
	CaptureComponent->SetupAttachment(Root);
	CaptureComponent->bCaptureEveryFrame = false;
	CaptureComponent->bCaptureOnMovement = false;
	CaptureComponent->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;

	TriggerVolume = CreateDefaultSubobject<UBoxComponent>(TEXT("TriggerVolume"));
	TriggerVolume->SetupAttachment(Root);
	TriggerVolume->SetBoxExtent(FVector(100.0, 800.0, 400.0));
	TriggerVolume->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	TriggerVolume->SetCollisionProfileName(TEXT("OverlapAllDynamic"));
	TriggerVolume->SetGenerateOverlapEvents(true);
}

void ASynthSpeedCamera::BeginPlay()
{
	Super::BeginPlay();

	ResolveRunDirectory();
	EnsureRenderTarget();

	CaptureComponent->FOVAngle = FieldOfViewDeg;
	TriggerVolume->OnComponentBeginOverlap.AddDynamic(this, &ASynthSpeedCamera::OnVehicleEnteredTrigger);

	for (TActorIterator<ADirectionalLight> It(GetWorld()); It; ++It)
	{
		SunLight = *It;	// first directional light is the sun; scene has exactly one
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

void ASynthSpeedCamera::OnVehicleEnteredTrigger(UPrimitiveComponent* /*OverlappedComponent*/, AActor* OtherActor,
	UPrimitiveComponent* /*OtherComponent*/, int32 /*OtherBodyIndex*/, bool /*bFromSweep*/, const FHitResult& /*Sweep*/)
{
	ASynthVehicle* Vehicle = Cast<ASynthVehicle>(OtherActor);
	if (!Vehicle)
	{
		return;		// scenery drifting through the trigger is not a subject
	}

	bool bWasAlreadyCaptured = false;
	AlreadyCaptured.Add(Vehicle, &bWasAlreadyCaptured);
	if (bWasAlreadyCaptured)
	{
		return;		// ponytail: one sample per pass; burst capture is a loop here when needed
	}

	CaptureVehicle(Vehicle);
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

	TSharedPtr<FJsonObject> Scene = MakeShared<FJsonObject>();
	if (const ADirectionalLight* Sun = SunLight.Get())
	{
		Scene->SetNumberField(TEXT("sun_pitch_deg"), Sun->GetActorRotation().Pitch);
		Scene->SetNumberField(TEXT("sun_yaw_deg"), Sun->GetActorRotation().Yaw);
	}
	Root->SetObjectField(TEXT("scene"), Scene);

	return Root;
}
