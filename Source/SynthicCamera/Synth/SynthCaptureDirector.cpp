#include "Synth/SynthCaptureDirector.h"

#include "SynthicCamera.h"
#include "Synth/SynthSpeedCamera.h"
#include "Synth/SynthVehicle.h"
#include "Synth/SynthWeather.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

ASynthCaptureDirector::ASynthCaptureDirector()
{
	PrimaryActorTick.bCanEverTick = true;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("DirectorRoot")));
}

TArray<FSynthVehicleSpec> ASynthCaptureDirector::MakeDefaultCatalog()
{
	// Proxy stand-ins with real-world dimensions. Make is deliberately "PROXY" so no
	// one mistakes a box for a real vehicle signature in the output. Point Spec.Mesh at
	// an imported asset to promote an entry to the real thing - no code change needed.
	auto Entry = [](const TCHAR* Model, ESynthVehicleClass Class, FVector Dims, int32 Axles, bool bTracked,
		const TCHAR* Livery, FLinearColor Colour)
	{
		FSynthVehicleSpec Spec;
		Spec.Make = TEXT("PROXY");
		Spec.Model = Model;
		Spec.VehicleClass = Class;
		Spec.DimensionsCm = Dims;
		Spec.AxleCount = Axles;
		Spec.bTracked = bTracked;
		Spec.LiveryName = Livery;
		Spec.LiveryColor = Colour;
		return Spec;
	};

	// Distinct liveries so the colour field in the label is actually present in the
	// pixels. Identical colours would make it a field a model can only learn to ignore.
	return {
		Entry(TEXT("LightUtility4x4"), ESynthVehicleClass::LightUtility, FVector(480, 210, 195), 2, false,
			TEXT("desert-tan"), FLinearColor(0.42f, 0.34f, 0.20f)),
		Entry(TEXT("CargoTruck6x6"), ESynthVehicleClass::CargoTruck, FVector(780, 250, 290), 3, false,
			TEXT("olive-drab"), FLinearColor(0.16f, 0.18f, 0.11f)),
		Entry(TEXT("WheeledAPC8x8"), ESynthVehicleClass::APC, FVector(780, 290, 270), 4, false,
			TEXT("nato-green"), FLinearColor(0.13f, 0.20f, 0.14f)),
		Entry(TEXT("TrackedIFV"), ESynthVehicleClass::IFV, FVector(660, 320, 260), 0, true,
			TEXT("sand-grey"), FLinearColor(0.38f, 0.35f, 0.28f)),
		Entry(TEXT("MainBattleTank"), ESynthVehicleClass::MBT, FVector(990, 370, 245), 0, true,
			TEXT("olive-drab"), FLinearColor(0.16f, 0.18f, 0.11f))
	};
}

void ASynthCaptureDirector::BeginPlay()
{
	Super::BeginPlay();

	if (Catalog.IsEmpty())
	{
		Catalog = MakeDefaultCatalog();
		UE_LOG(LogSynthic, Warning, TEXT("Director: no catalog authored, using %d proxy archetypes."), Catalog.Num());
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
