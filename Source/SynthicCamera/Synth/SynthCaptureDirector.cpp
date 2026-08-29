#include "Synth/SynthCaptureDirector.h"

#include "SynthicCamera.h"
#include "Synth/SynthSpeedCamera.h"
#include "Synth/SynthVehicle.h"
#include "Synth/SynthWeather.h"
#include "Algo/Count.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

ASynthCaptureDirector::ASynthCaptureDirector()
{
	PrimaryActorTick.bCanEverTick = true;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("DirectorRoot")));
}

namespace
{
	/** A box part. Offsets are from the ground contact point at the actor origin. */
	FSynthVehiclePart Box(FVector Offset, FVector Size, float Tint = 1.0f)
	{
		FSynthVehiclePart Part;
		Part.Shape = ESynthPartShape::Box;
		Part.OffsetCm = Offset;
		Part.SizeCm = Size;
		Part.ColorScale = Tint;
		return Part;
	}

	/**
	 * A wheel: a cylinder rolled onto its side so its axis runs across the vehicle.
	 * Scale is applied before rotation, so the size is given as
	 * (diameter, diameter, width) and the roll then lays it down.
	 */
	FSynthVehiclePart Wheel(FVector Centre, float Diameter, float Width)
	{
		FSynthVehiclePart Part;
		Part.Shape = ESynthPartShape::Cylinder;
		Part.OffsetCm = Centre;
		Part.SizeCm = FVector(Diameter, Diameter, Width);
		Part.Rotation = FRotator(0.0, 0.0, 90.0);
		Part.ColorScale = 0.32f;		// rubber reads far darker than bodywork
		return Part;
	}

	/** A gun barrel: a cylinder pitched to lie along the vehicle's length. */
	FSynthVehiclePart Barrel(FVector Centre, float Diameter, float Length)
	{
		FSynthVehiclePart Part;
		Part.Shape = ESynthPartShape::Cylinder;
		Part.OffsetCm = Centre;
		Part.SizeCm = FVector(Diameter, Diameter, Length);
		Part.Rotation = FRotator(90.0, 0.0, 0.0);
		Part.ColorScale = 0.55f;
		return Part;
	}

	void AddWheelPair(TArray<FSynthVehiclePart>& Parts, float X, float HalfTrack,
		float Diameter, float Width)
	{
		Parts.Add(Wheel(FVector(X, -HalfTrack, Diameter * 0.5), Diameter, Width));
		Parts.Add(Wheel(FVector(X,  HalfTrack, Diameter * 0.5), Diameter, Width));
	}
}

TArray<FSynthVehicleSpec> ASynthCaptureDirector::MakeDefaultCatalog()
{
	// Assembled silhouettes rather than single boxes. Class is mostly carried by
	// shape - turret, barrel, tracks, cab position - and two boxes of different
	// proportions give a model nothing it could carry across to real footage.
	//
	// Still proxies: no suspension detail, no stowage, no surface texture. Make stays
	// PROXY so nothing mistakes one for a real vehicle signature.
	auto Entry = [](const TCHAR* Model, ESynthVehicleClass Class, FVector Dims, int32 Axles,
		bool bTracked, const TCHAR* Livery, FLinearColor Colour, TArray<FSynthVehiclePart> Parts)
	{
		FSynthVehicleSpec Spec;
		Spec.bMilitary = true;
		Spec.Make = TEXT("PROXY");
		Spec.Model = Model;
		Spec.VehicleClass = Class;
		Spec.DimensionsCm = Dims;
		Spec.AxleCount = Axles;
		Spec.bTracked = bTracked;
		Spec.LiveryName = Livery;
		Spec.LiveryColor = Colour;
		Spec.Parts = MoveTemp(Parts);
		return Spec;
	};

	TArray<FSynthVehiclePart> Utility;
	AddWheelPair(Utility, 155.0f, 88.0f, 80.0f, 30.0f);
	AddWheelPair(Utility, -155.0f, 88.0f, 80.0f, 30.0f);
	Utility.Add(Box(FVector(0, 0, 88), FVector(430, 180, 66)));			// chassis
	Utility.Add(Box(FVector(145, 0, 138), FVector(150, 172, 46)));		// bonnet
	Utility.Add(Box(FVector(-40, 0, 158), FVector(170, 168, 74)));		// cab

	TArray<FSynthVehiclePart> Truck;
	AddWheelPair(Truck, 250.0f, 105.0f, 110.0f, 40.0f);
	AddWheelPair(Truck, -140.0f, 105.0f, 110.0f, 40.0f);
	AddWheelPair(Truck, -280.0f, 105.0f, 110.0f, 40.0f);
	Truck.Add(Box(FVector(0, 0, 118), FVector(700, 215, 56)));			// chassis rail
	Truck.Add(Box(FVector(270, 0, 208), FVector(200, 228, 128)));		// cab
	Truck.Add(Box(FVector(-120, 0, 216), FVector(420, 236, 145), 0.88f));	// cargo bed

	TArray<FSynthVehiclePart> Apc;
	AddWheelPair(Apc, 280.0f, 122.0f, 120.0f, 45.0f);
	AddWheelPair(Apc, 90.0f, 122.0f, 120.0f, 45.0f);
	AddWheelPair(Apc, -100.0f, 122.0f, 120.0f, 45.0f);
	AddWheelPair(Apc, -290.0f, 122.0f, 120.0f, 45.0f);
	Apc.Add(Box(FVector(0, 0, 126), FVector(720, 262, 104)));			// hull
	Apc.Add(Box(FVector(-30, 0, 202), FVector(600, 232, 68)));			// upper hull
	Apc.Add(Box(FVector(-80, 0, 246), FVector(150, 148, 44)));			// cupola

	// Tracks sit at the vehicle's full width with the hull narrower and resting on
	// top, so the running gear reads as track runs rather than as skids poking out
	// from under a wider body. Tracked versus wheeled is a primary class cue.
	TArray<FSynthVehiclePart> Ifv;
	Ifv.Add(Box(FVector(0, -108, 52), FVector(640, 104, 104), 0.40f));	// track run
	Ifv.Add(Box(FVector(0,  108, 52), FVector(640, 104, 104), 0.40f));
	Ifv.Add(Box(FVector(0, 0, 146), FVector(590, 214, 88)));			// hull
	Ifv.Add(Box(FVector(-20, 0, 206), FVector(460, 196, 44)));			// upper hull
	Ifv.Add(Box(FVector(-55, 0, 236), FVector(210, 168, 52)));			// turret
	Ifv.Add(Barrel(FVector(135, 0, 240), 14.0f, 330.0f));

	TArray<FSynthVehiclePart> Mbt;
	Mbt.Add(Box(FVector(0, -140, 58), FVector(920, 90, 116), 0.40f));	// track run
	Mbt.Add(Box(FVector(0,  140, 58), FVector(920, 90, 116), 0.40f));
	Mbt.Add(Box(FVector(0, 0, 152), FVector(860, 262, 72)));			// hull
	Mbt.Add(Box(FVector(-60, 0, 208), FVector(340, 232, 56)));			// turret
	Mbt.Add(Barrel(FVector(200, 0, 212), 24.0f, 450.0f));				// main gun

	// Real assets from the imported pack. Measured pivots sit at ground contact, which
	// is what the actor origin assumes, so no vertical offset is needed. Dimensions
	// come from the scan rather than being typed by hand, so the label matches the
	// geometry a model actually sees.
	auto Asset = [](const TCHAR* Model, ESynthVehicleClass Class, FVector Dims,
		int32 Axles, const TCHAR* MeshPath)
	{
		FSynthVehicleSpec Spec;
		Spec.bMilitary = false;
		Spec.Make = TEXT("CIVILIAN");
		Spec.Model = Model;
		Spec.VehicleClass = Class;
		Spec.DimensionsCm = Dims;
		Spec.AxleCount = Axles;
		Spec.bTracked = false;
		Spec.Mesh = TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(MeshPath));
		// Left as-authored: the asset brings its own textured materials.
		Spec.LiveryName = TEXT("as-authored");
		return Spec;
	};

	return {
		Asset(TEXT("Hatchback"), ESynthVehicleClass::Car, FVector(394, 180, 124), 2,
			TEXT("/Game/VehicleVarietyPack/Meshes/SM_Hatchback.SM_Hatchback")),
		Asset(TEXT("SportsCar"), ESynthVehicleClass::Car, FVector(436, 224, 113), 2,
			TEXT("/Game/VehicleVarietyPack/Meshes/SM_SportsCar.SM_SportsCar")),
		Asset(TEXT("SUV"), ESynthVehicleClass::SUV, FVector(421, 196, 167), 2,
			TEXT("/Game/VehicleVarietyPack/Meshes/SM_SUV.SM_SUV")),
		Asset(TEXT("Pickup"), ESynthVehicleClass::Pickup, FVector(429, 191, 143), 2,
			TEXT("/Game/VehicleVarietyPack/Meshes/SM_Pickup.SM_Pickup")),
		Asset(TEXT("BoxTruck"), ESynthVehicleClass::BoxTruck, FVector(732, 306, 319), 2,
			TEXT("/Game/VehicleVarietyPack/Meshes/SM_Truck_Box.SM_Truck_Box")),

		Entry(TEXT("LightUtility4x4"), ESynthVehicleClass::LightUtility, FVector(480, 210, 195), 2, false,
			TEXT("desert-tan"), FLinearColor(0.42f, 0.34f, 0.20f), MoveTemp(Utility)),
		Entry(TEXT("CargoTruck6x6"), ESynthVehicleClass::CargoTruck, FVector(780, 250, 290), 3, false,
			TEXT("olive-drab"), FLinearColor(0.16f, 0.18f, 0.11f), MoveTemp(Truck)),
		Entry(TEXT("WheeledAPC8x8"), ESynthVehicleClass::APC, FVector(780, 290, 270), 4, false,
			TEXT("nato-green"), FLinearColor(0.13f, 0.20f, 0.14f), MoveTemp(Apc)),
		Entry(TEXT("TrackedIFV"), ESynthVehicleClass::IFV, FVector(660, 320, 260), 0, true,
			TEXT("sand-grey"), FLinearColor(0.38f, 0.35f, 0.28f), MoveTemp(Ifv)),
		Entry(TEXT("MainBattleTank"), ESynthVehicleClass::MBT, FVector(990, 370, 245), 0, true,
			TEXT("olive-drab"), FLinearColor(0.16f, 0.18f, 0.11f), MoveTemp(Mbt))
	};
}

void ASynthCaptureDirector::BeginPlay()
{
	Super::BeginPlay();

	if (Catalog.IsEmpty())
	{
		Catalog = MakeDefaultCatalog();
		const int32 RealAssets = Algo::CountIf(Catalog,
			[](const FSynthVehicleSpec& Spec) { return !Spec.Mesh.IsNull(); });

		UE_LOG(LogSynthic, Log,
			TEXT("Director: default catalog - %d archetypes, %d from real assets, %d proxy."),
			Catalog.Num(), RealAssets, Catalog.Num() - RealAssets);
	}

	if (MaxSpeedKph < MinSpeedKph)
	{
		UE_LOG(LogSynthic, Error, TEXT("Director: MaxSpeedKph (%.1f) is below MinSpeedKph (%.1f); no traffic will run."),
			MaxSpeedKph, MinSpeedKph);
		return;
	}

	if (WeatherMix.IsEmpty())
	{
		WeatherMix = { ESynthWeather::Clear, ESynthWeather::Hazy,
			ESynthWeather::Overcast, ESynthWeather::DustStorm };
	}

	Stream.Initialize(RandomSeed);
	UE_LOG(LogSynthic, Log,
		TEXT("Director: %d passes, seed %d, speeds %.0f-%.0f km/h, scene randomisation %s (%d conditions)."),
		NumPasses, RandomSeed, MinSpeedKph, MaxSpeedKph,
		bRandomiseScene ? TEXT("on") : TEXT("OFF"), WeatherMix.Num());

	DispatchNextVehicle();
}

void ASynthCaptureDirector::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!ActiveVehicle)
	{
		return;		// run finished, or never started
	}

	if (ActiveVehicle->GetDistanceTravelledCm() < TravelDistanceCm)
	{
		return;		// still on the road
	}

	ActiveVehicle->Destroy();
	ActiveVehicle = nullptr;
	DispatchNextVehicle();
}

void ASynthCaptureDirector::RandomiseScene()
{
	if (!bRandomiseScene || WeatherMix.IsEmpty())
	{
		return;
	}

	const ESynthWeather Weather = WeatherMix[Stream.RandRange(0, WeatherMix.Num() - 1)];
	const FSynthWeatherPreset Preset =
		SynthWeather::Jitter(SynthWeather::GetPreset(Weather), Stream, WeatherJitter);

	const float SunPitch = Stream.FRandRange(SunPitchRangeDeg.X, SunPitchRangeDeg.Y);
	const float SunYaw = Stream.FRandRange(SunYawRangeDeg.X, SunYawRangeDeg.Y);

	SynthWeather::ApplyToWorld(*GetWorld(), Preset, SunPitch, SunYaw);

	// Ambient is a post-process value on each capture, not a world actor, so it has
	// to be pushed rather than picked up from the level like sun and fog are.
	for (TActorIterator<ASynthSpeedCamera> It(GetWorld()); It; ++It)
	{
		It->SetSceneConditions(Preset.AmbientIntensity, SynthWeather::GetName(Weather));

		if (bRandomiseCameraPose)
		{
			RandomiseCameraPose(**It);
		}
	}

	UE_LOG(LogSynthic, Log, TEXT("Director: %s, sun %.0f deg elevation / %.0f deg bearing, fog %.2f."),
		*SynthWeather::GetName(Weather), SunPitch, SunYaw, Preset.FogDensity);
}

void ASynthCaptureDirector::RandomiseCameraPose(ASynthSpeedCamera& Camera)
{
	// The director's transform defines the road: its location is the start line and
	// its forward vector is the direction of travel. Directions come from that frame
	// so the geometry survives the road being moved or rotated.
	const FVector Along = GetActorForwardVector();
	const FVector Across = GetActorRightVector();
	const FVector StartLine = GetActorLocation();

	// How far down the road the level actually installed this camera. Positions are
	// perturbations of that, NOT offsets from the start line - measuring from the
	// start line put the camera (and with it the trip plane) behind the spawn point,
	// where an approaching vehicle is already past the plane and never crosses it.
	const float InstalledAlong =
		static_cast<float>(FVector::DotProduct(Camera.GetInstallOrigin() - StartLine, Along));

	const float AlongRoad = InstalledAlong +
		Stream.FRandRange(CameraPose.AlongRoadJitterCm.X, CameraPose.AlongRoadJitterCm.Y);

	const float Side = Stream.FRand() < 0.5f ? -1.0f : 1.0f;
	const float Lateral = Side * Stream.FRandRange(
		CameraPose.LateralOffsetCm.X, CameraPose.LateralOffsetCm.Y);
	const float Height = Stream.FRandRange(CameraPose.HeightCm.X, CameraPose.HeightCm.Y);

	const FVector Position = StartLine + (Along * AlongRoad) + (Across * Lateral)
		+ FVector(0.0, 0.0, Height);

	// Sampling a distance up-road rather than an absolute position is what guarantees
	// the camera always faces oncoming traffic, instead of hoping two independent
	// draws happen to land the right way round.
	const float AimDistance = Stream.FRandRange(CameraPose.AimDistanceCm.X, CameraPose.AimDistanceCm.Y);
	const float AimLateral = Stream.FRandRange(CameraPose.AimLateralCm.X, CameraPose.AimLateralCm.Y);
	const float AimHeight = Stream.FRandRange(CameraPose.AimHeightCm.X, CameraPose.AimHeightCm.Y);

	const FVector AimPoint = StartLine + (Along * (AlongRoad - AimDistance))
		+ (Across * AimLateral) + FVector(0.0, 0.0, AimHeight);

	// The trip plane sits at the aim point, so a vehicle must still have road left to
	// cover before reaching it. Warn rather than emit a run of empty passes.
	if (AlongRoad - AimDistance <= 0.0f)
	{
		UE_LOG(LogSynthic, Warning,
			TEXT("Camera pose: trip plane lands %.0fcm behind the start line; this pass will not "
				 "capture. Reduce AimDistanceCm or move the camera further down the road."),
			AimDistance - AlongRoad);
	}

	const float Roll = Stream.FRandRange(CameraPose.RollDeg.X, CameraPose.RollDeg.Y);
	const float Fov = Stream.FRandRange(CameraPose.FieldOfViewDeg.X, CameraPose.FieldOfViewDeg.Y);

	Camera.PlaceAt(Position, AimPoint, Roll, Fov);

	UE_LOG(LogSynthic, Log,
		TEXT("Director: camera %.0fcm high, %.0fcm off-side, shooting %.0fm at %.0f deg FOV."),
		Height, Lateral, AimDistance / 100.0f, Fov);
}

bool ASynthCaptureDirector::DispatchNextVehicle()
{
	if (PassesDispatched >= NumPasses)
	{
		UE_LOG(LogSynthic, Log, TEXT("Director: run complete, %d passes dispatched."), PassesDispatched);

		// Lets a whole dataset be generated by one headless command that actually ends.
		if (FParse::Param(FCommandLine::Get(), TEXT("SynthAutoQuit")))
		{
			UE_LOG(LogSynthic, Log, TEXT("Director: -SynthAutoQuit set, exiting."));
			FPlatformMisc::RequestExit(false);
		}
		return false;
	}

	RandomiseScene();

	const FSynthVehicleSpec& Spec = Catalog[Stream.RandRange(0, Catalog.Num() - 1)];
	const float SpeedKph = Stream.FRandRange(MinSpeedKph, MaxSpeedKph);
	const float LaneOffset = Stream.FRandRange(-LaneJitterCm, LaneJitterCm);

	const FVector SpawnLocation = GetActorLocation() + (GetActorRightVector() * LaneOffset);

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	ActiveVehicle = GetWorld()->SpawnActor<ASynthVehicle>(
		ASynthVehicle::StaticClass(), SpawnLocation, GetActorRotation(), SpawnParameters);

	if (!ActiveVehicle)
	{
		UE_LOG(LogSynthic, Error, TEXT("Director: SpawnActor failed on pass %d; aborting run."), PassesDispatched);
		return false;
	}

	ActiveVehicle->ApplySpec(Spec);
	ActiveVehicle->DriveAlong(GetActorForwardVector(), SpeedKph);
	++PassesDispatched;

	UE_LOG(LogSynthic, Log, TEXT("Director: pass %d/%d - %s at %.1f km/h from %s, recycling after %.0fcm."),
		PassesDispatched, NumPasses, *Spec.Model, SpeedKph,
		*ActiveVehicle->GetActorLocation().ToCompactString(), TravelDistanceCm);

	return true;
}
