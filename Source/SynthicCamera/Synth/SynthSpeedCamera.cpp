#include "Synth/SynthSpeedCamera.h"

#include "SynthicCamera.h"
#include "Synth/SynthDataset.h"
#include "Synth/SynthProjection.h"
#include "Synth/SynthVehicle.h"

#include "Camera/CameraTypes.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Dom/JsonObject.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/StaticMesh.h"
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

	/** Road deck top, from build_desert.py: 6cm centre + 10cm half-thickness. */
	constexpr double RoadDeckZ = 16.0;
	constexpr float ProxyCubeSizeCm = 100.0f;

	/** Dress a prop component: engine cube, no collision, invisible to captures. */
	void MakeProp(UStaticMeshComponent& Component)
	{
		if (UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")))
		{
			Component.SetStaticMesh(Cube);
		}
		if (UMaterialInterface* Grey = LoadObject<UMaterialInterface>(nullptr,
			TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
		{
			Component.SetMaterial(0, Grey);
		}

		Component.SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component.SetMobility(EComponentMobility::Movable);

		// The housing occupies the same point as the lens. Without this it would be
		// the only thing in every single frame of the dataset.
		Component.bHiddenInSceneCapture = true;
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

	MastMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MastMesh"));
	MastMesh->SetupAttachment(Root);

	// Housing hangs off the capture component so it aims wherever the lens does.
	HousingMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("HousingMesh"));
	HousingMesh->SetupAttachment(CaptureComponent);
	HousingMesh->SetRelativeLocation(FVector(-35.0, 0.0, 0.0));
	HousingMesh->SetRelativeScale3D(FVector(0.70, 0.30, 0.30));

	// Rides on the capture point, so it tracks the trip plane wherever the aim moves.
	TripPlaneMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("TripPlaneMesh"));
	TripPlaneMesh->SetupAttachment(CapturePoint);
	TripPlaneMesh->SetRelativeScale3D(FVector(0.04, 9.0, 3.2));
}

void ASynthSpeedCamera::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	// Runs in the editor too, so the mast looks right the moment the level opens and
	// re-stretches whenever someone drags the camera up or down its pole.
	MakeProp(*MastMesh);
	MakeProp(*HousingMesh);
	MakeProp(*TripPlaneMesh);
	TripPlaneMesh->SetVisibility(bShowTripPlane);
	UpdateMastToGround();
}

void ASynthSpeedCamera::UpdateMastToGround()
{
	const double HeightAboveRoad = FMath::Max(GetActorLocation().Z - RoadDeckZ, 50.0);

	MastMesh->SetRelativeScale3D(FVector(0.22, 0.22, HeightAboveRoad / ProxyCubeSizeCm));
	MastMesh->SetRelativeLocation(FVector(0.0, 0.0, -HeightAboveRoad * 0.5));
}

void ASynthSpeedCamera::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	// Recorded here, not in BeginPlay: the director re-sites cameras from its own
	// BeginPlay, and BeginPlay ordering between actors is not guaranteed. By this
	// point every actor is constructed and none has been moved.
	InstallOrigin = GetActorLocation();
}

void ASynthSpeedCamera::BeginPlay()
{
	Super::BeginPlay();

	ResolveRunDirectory();
	EnsureRenderTarget();

	CaptureComponent->FOVAngle = FieldOfViewDeg;
	TripPlaneMesh->SetVisibility(bShowTripPlane);
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

void ASynthSpeedCamera::PlaceAt(const FVector& Position, const FVector& AimPoint,
	float RollDeg, float FieldOfView)
{
	const FVector ToAim = AimPoint - Position;
	if (ToAim.IsNearlyZero())
	{
		UE_LOG(LogSynthic, Error,
			TEXT("PlaceAt: aim point coincides with the camera; leaving the install untouched."));
		return;
	}

	SetActorLocation(Position);

	// The root stays unrotated on purpose. The trip plane's normal is the capture
	// point's forward vector, and keeping the actor square to the world keeps that
	// normal aligned with the traffic direction no matter where the lens points.
	SetActorRotation(FRotator::ZeroRotator);

	FRotator Aim = ToAim.Rotation();
	Aim.Roll = RollDeg;
	CaptureComponent->SetRelativeRotation(Aim);

	// Unrotated root means a relative offset is a plain world offset.
	CapturePoint->SetRelativeLocation(ToAim);

	FieldOfViewDeg = FieldOfView;
	CaptureComponent->FOVAngle = FieldOfView;

	// Pose randomisation changes the pole height every pass, so the mast has to be
	// re-cut to still reach the road.
	UpdateMastToGround();
}

void ASynthSpeedCamera::SetSceneConditions(float InAmbientIntensity, const FString& InWeatherName)
{
	AmbientIntensity = InAmbientIntensity;
	WeatherName = InWeatherName;
	ApplyAmbientLighting();
}

void ASynthSpeedCamera::DrawTrackedBounds() const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	for (TActorIterator<ASynthVehicle> It(World); It; ++It)
	{
		const ASynthVehicle* Vehicle = *It;
		if (!Vehicle)
		{
			continue;
		}

		FTransform BoxToWorld;
		FVector LocalCentre;
		FVector LocalExtent;
		Vehicle->GetVisualBounds(BoxToWorld, LocalCentre, LocalExtent);

		const float Visible = MeasureVisibleFraction(*Vehicle, LocalCentre, LocalExtent);

		// Same colours the preview overlay uses, so what is on screen during a run and
		// what comes out of the tooling afterwards agree.
		FColor Colour = FColor(87, 217, 138);				// clear
		if (Visible <= 0.0f)
		{
			Colour = FColor(224, 96, 96);					// fully blocked
		}
		else if (Visible < 1.0f)
		{
			Colour = FColor(224, 166, 72);					// partly hidden
		}

		DrawDebugBox(World, BoxToWorld.TransformPosition(LocalCentre), LocalExtent,
			BoxToWorld.GetRotation(), Colour, /*bPersistent*/ false, /*LifeTime*/ -1.0f,
			/*DepthPriority*/ 0, /*Thickness*/ 4.0f);
	}
}

float ASynthSpeedCamera::MeasureVisibleFraction(const ASynthVehicle& Vehicle,
	const FVector& LocalCentre, const FVector& LocalExtent) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return 1.0f;
	}

	FTransform BoxToWorld;
	FVector UnusedCentre;
	FVector UnusedExtent;
	Vehicle.GetVisualBounds(BoxToWorld, UnusedCentre, UnusedExtent);

	// Eight corners plus the centre. Corners alone would call a vehicle fully hidden
	// the moment a pole crosses one edge, and the centre alone would miss anything
	// that clips a flank.
	//
	// Sampled just inside the bounds, not on them. The box's lower corners sit exactly
	// at the ground contact point, so traces aimed at them graze the road deck and come
	// back blocked - which reported every vehicle as occluded with nothing occluding it.
	constexpr double SampleInset = 0.88;

	TArray<FVector, TInlineAllocator<8>> Samples;
	SynthProjection::BoxCornersToWorld(BoxToWorld, LocalCentre, LocalExtent * SampleInset, Samples);
	Samples.Add(BoxToWorld.TransformPosition(LocalCentre));

	FCollisionQueryParams Params(SCENE_QUERY_STAT(SynthVisibility), /*bTraceComplex*/ false);
	Params.AddIgnoredActor(this);
	Params.AddIgnoredActor(&Vehicle);

	const FVector Lens = CaptureComponent->GetComponentLocation();
	int32 Clear = 0;
	for (const FVector& Sample : Samples)
	{
		if (!World->LineTraceTestByChannel(Lens, Sample, ECC_Visibility, Params))
		{
			++Clear;
		}
	}

	return static_cast<float>(Clear) / static_cast<float>(Samples.Num());
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

	if (bDrawDebugBounds)
	{
		DrawTrackedBounds();
	}
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

	const FString ImageFileName = FString::Printf(TEXT("%s_%06d.png"), *RunName, CaptureCount);

	if (!SynthDataset::SavePng(FPaths::Combine(RunDirectory, ImageFileName), Pixels, ImageWidth, ImageHeight))
	{
		return false;	// no image means the label would dangle - drop the whole sample
	}

	const TSharedRef<FJsonObject> Frame = BuildFrameLabel(*Vehicle, Pixels, ImageFileName);
	if (!SynthDataset::AppendJsonLine(LabelFilePath, Frame))
	{
		return false;
	}

	++CaptureCount;

	const TArray<TSharedPtr<FJsonValue>>* Objects = nullptr;
	Frame->TryGetArrayField(TEXT("objects"), Objects);

	UE_LOG(LogSynthic, Log, TEXT("Captured %s %s at %.1f km/h -> %s (%d object%s in frame)"),
		*Vehicle->GetSpec().Make, *Vehicle->GetSpec().Model, Vehicle->GetMeasuredSpeedKph(),
		*ImageFileName, Objects ? Objects->Num() : 0,
		(Objects && Objects->Num() == 1) ? TEXT("") : TEXT("s"));

	return true;
}

float ASynthSpeedCamera::MeasureContrast(const TArray<FColor>& Pixels, int32 Width,
	int32 Height, const FSynthScreenBox& Box)
{
	const int64 Expected = static_cast<int64>(Width) * static_cast<int64>(Height);
	if (Pixels.Num() != Expected || Box.Width() < 2.0f || Box.Height() < 2.0f)
	{
		return 0.0f;
	}

	auto MeanOver = [&Pixels, Width, Height](int32 X0, int32 Y0, int32 X1, int32 Y1, double* Out)
	{
		X0 = FMath::Clamp(X0, 0, Width - 1);
		X1 = FMath::Clamp(X1, 0, Width - 1);
		Y0 = FMath::Clamp(Y0, 0, Height - 1);
		Y1 = FMath::Clamp(Y1, 0, Height - 1);

		double Sums[3] = { 0.0, 0.0, 0.0 };
		int64 Count = 0;
		for (int32 Y = Y0; Y <= Y1; ++Y)
		{
			for (int32 X = X0; X <= X1; ++X)
			{
				const FColor& C = Pixels[Y * Width + X];
				Sums[0] += C.R; Sums[1] += C.G; Sums[2] += C.B;
				++Count;
			}
		}

		if (Count == 0)
		{
			return false;
		}
		for (int32 Channel = 0; Channel < 3; ++Channel)
		{
			Out[Channel] = Sums[Channel] / static_cast<double>(Count);
		}
		return true;
	};

	// Inner is the box; outer is the box grown by 40%, so the difference is the object
	// against what immediately surrounds it rather than against the frame average.
	const int32 Margin = FMath::Max(4, FMath::RoundToInt(FMath::Max(Box.Width(), Box.Height()) * 0.4f));

	double Inner[3] = { 0.0, 0.0, 0.0 };
	double Outer[3] = { 0.0, 0.0, 0.0 };
	if (!MeanOver(Box.MinX, Box.MinY, Box.MaxX, Box.MaxY, Inner) ||
		!MeanOver(Box.MinX - Margin, Box.MinY - Margin, Box.MaxX + Margin, Box.MaxY + Margin, Outer))
	{
		return 0.0f;
	}

	double Largest = 0.0;
	for (int32 Channel = 0; Channel < 3; ++Channel)
	{
		Largest = FMath::Max(Largest, FMath::Abs(Inner[Channel] - Outer[Channel]));
	}
	return static_cast<float>(Largest);
}

TSharedPtr<FJsonObject> ASynthSpeedCamera::BuildObjectEntry(const ASynthVehicle& Vehicle,
	const TArray<FColor>& Pixels, bool bTriggered) const
{
	FTransform BoxToWorld;
	FVector LocalCentre;
	FVector LocalExtent;
	Vehicle.GetVisualBounds(BoxToWorld, LocalCentre, LocalExtent);

	const FSynthProjectedBox Projected = SynthProjection::ProjectOrientedBox(
		BuildViewProjectionMatrix(), BoxToWorld, LocalCentre, LocalExtent, ImageWidth, ImageHeight);
	const FSynthScreenBox& Box = Projected.Bounds;

	// Off-screen vehicles are simply not in this frame. Emitting an entry with no box
	// would put a labelled object where there are no pixels.
	if (!Box.bValid)
	{
		return nullptr;
	}

	// Too small to be a detection. The vehicle that tripped the shutter is always kept
	// - it is the reason the frame exists, and dropping it would leave a capture with
	// nothing to explain it.
	if (!bTriggered && Box.Width() * Box.Height() < MinObjectAreaPx)
	{
		return nullptr;
	}

	const float VisibleFraction = MeasureVisibleFraction(Vehicle, LocalCentre, LocalExtent);
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

	// Which vehicle tripped the shutter. The others were simply in shot, and a model
	// evaluated on "did it find the speeding vehicle" needs to tell them apart.
	Root->SetBoolField(TEXT("triggered_capture"), bTriggered);

	TSharedPtr<FJsonObject> VehicleJson = MakeShared<FJsonObject>();
	VehicleJson->SetStringField(TEXT("make"), Spec.Make);
	VehicleJson->SetStringField(TEXT("model"), Spec.Model);
	VehicleJson->SetStringField(TEXT("class"),
		StaticEnum<ESynthVehicleClass>()->GetNameStringByValue(static_cast<int64>(Spec.VehicleClass)));
	VehicleJson->SetNumberField(TEXT("axle_count"), Spec.AxleCount);
	VehicleJson->SetBoolField(TEXT("tracked"), Spec.bTracked);
	VehicleJson->SetBoolField(TEXT("military"), Spec.bMilitary);
	VehicleJson->SetStringField(TEXT("livery"), Spec.LiveryName);

	// Only meaningful when the livery was actually painted on. A real asset keeps its
	// authored materials, so publishing a colour here would describe pixels that do
	// not exist.
	VehicleJson->SetBoolField(TEXT("livery_applied"), Vehicle.WasLiveryApplied());
	if (Vehicle.WasLiveryApplied())
	{
		VehicleJson->SetObjectField(TEXT("livery_rgb"), VectorToJson(
			FVector(Spec.LiveryColor.R, Spec.LiveryColor.G, Spec.LiveryColor.B)));
	}
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

		// The four rectangle corners, clockwise from top-left.
		TArray<TSharedPtr<FJsonValue>> Rect;
		const float RectPoints[4][2] = {
			{ Box.MinX, Box.MinY }, { Box.MaxX, Box.MinY },
			{ Box.MaxX, Box.MaxY }, { Box.MinX, Box.MaxY } };
		for (const float(&Point)[2] : RectPoints)
		{
			TSharedPtr<FJsonObject> Vertex = MakeShared<FJsonObject>();
			Vertex->SetNumberField(TEXT("x"), Point[0]);
			Vertex->SetNumberField(TEXT("y"), Point[1]);
			Rect.Add(MakeShared<FJsonValueObject>(Vertex));
		}
		Geometry->SetArrayField(TEXT("bbox_verts_2d"), Rect);
	}

	// The eight projected cuboid corners. These keep the orientation the axis-aligned
	// rectangle discards - which face is toward the lens, how the vehicle is yawed.
	if (Projected.Corners.Num() == 8)
	{
		TArray<TSharedPtr<FJsonValue>> Cuboid;
		for (const FVector2D& Corner : Projected.Corners)
		{
			TSharedPtr<FJsonObject> Vertex = MakeShared<FJsonObject>();
			Vertex->SetNumberField(TEXT("x"), Corner.X);
			Vertex->SetNumberField(TEXT("y"), Corner.Y);
			Cuboid.Add(MakeShared<FJsonValueObject>(Vertex));
		}
		Geometry->SetArrayField(TEXT("bbox_verts_3d_projected"), Cuboid);
	}
	Root->SetObjectField(TEXT("geometry"), Geometry);

	// Visibility is separate from geometry on purpose: a box can be perfectly correct
	// while the thing it describes is behind a pole or off the edge of the frame.
	TSharedPtr<FJsonObject> Vis = MakeShared<FJsonObject>();
	Vis->SetNumberField(TEXT("visible_fraction"), VisibleFraction);
	Vis->SetBoolField(TEXT("occluded"), VisibleFraction < 1.0f);
	Vis->SetBoolField(TEXT("fully_occluded"), VisibleFraction <= 0.0f);
	Vis->SetBoolField(TEXT("in_frame"), Box.bValid);
	Vis->SetBoolField(TEXT("truncated"), Box.bClipped);
	Vis->SetBoolField(TEXT("camera_can_see"), Box.bValid && VisibleFraction > 0.0f);

	// Measured on the rendered frame, not inferred. Geometry says where the vehicle is;
	// this says whether it can be told apart from what is behind it.
	Vis->SetNumberField(TEXT("contrast"),
		MeasureContrast(Pixels, ImageWidth, ImageHeight, Box));
	Root->SetObjectField(TEXT("visibility"), Vis);

	return Root;
}

TSharedRef<FJsonObject> ASynthSpeedCamera::BuildFrameLabel(const ASynthVehicle& Trigger,
	const TArray<FColor>& Pixels, const FString& ImageFileName) const
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("frame_id"), CaptureCount);
	Root->SetStringField(TEXT("image_file"), ImageFileName);
	Root->SetNumberField(TEXT("image_width"), ImageWidth);
	Root->SetNumberField(TEXT("image_height"), ImageHeight);
	Root->SetNumberField(TEXT("sim_time_s"), GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0);

	// Every vehicle on screen, not only the one that crossed the plane. Anything left
	// out is an unlabelled object sitting in the image, which trains a detector to
	// treat that vehicle's pixels as background.
	TArray<TSharedPtr<FJsonValue>> Objects;
	if (const UWorld* World = GetWorld())
	{
		for (TActorIterator<ASynthVehicle> It(const_cast<UWorld*>(World)); It; ++It)
		{
			const ASynthVehicle* Vehicle = *It;
			if (!Vehicle)
			{
				continue;
			}

			if (TSharedPtr<FJsonObject> Entry = BuildObjectEntry(*Vehicle, Pixels, Vehicle == &Trigger))
			{
				Objects.Add(MakeShared<FJsonValueObject>(Entry));
			}
		}
	}
	Root->SetArrayField(TEXT("objects"), Objects);

	TSharedPtr<FJsonObject> CameraJson = MakeShared<FJsonObject>();
	CameraJson->SetObjectField(TEXT("position"), VectorToJson(CaptureComponent->GetComponentLocation()));
	CameraJson->SetObjectField(TEXT("rotation_pyr"), VectorToJson(FVector(
		CaptureComponent->GetComponentRotation().Pitch,
		CaptureComponent->GetComponentRotation().Yaw,
		CaptureComponent->GetComponentRotation().Roll)));
	CameraJson->SetNumberField(TEXT("fov_deg"), CaptureComponent->FOVAngle);
	Root->SetObjectField(TEXT("camera"), CameraJson);

	// Read back out of the world at capture time rather than echoing what was asked
	// for, so a setter that silently stops working shows as a flat column.
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
