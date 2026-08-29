#include "Synth/SynthCaptureDirector.h"

#include "SynthicCamera.h"
#include "Synth/SynthVehicle.h"
#include "Engine/World.h"

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
	auto Entry = [](const TCHAR* Model, ESynthVehicleClass Class, FVector Dims, int32 Axles, bool bTracked)
	{
		FSynthVehicleSpec Spec;
		Spec.Make = TEXT("PROXY");
		Spec.Model = Model;
		Spec.VehicleClass = Class;
		Spec.DimensionsCm = Dims;
		Spec.AxleCount = Axles;
		Spec.bTracked = bTracked;
		return Spec;
	};

	return {
		Entry(TEXT("LightUtility4x4"), ESynthVehicleClass::LightUtility, FVector(480, 210, 195), 2, false),
		Entry(TEXT("CargoTruck6x6"),   ESynthVehicleClass::CargoTruck,   FVector(780, 250, 290), 3, false),
		Entry(TEXT("WheeledAPC8x8"),   ESynthVehicleClass::APC,          FVector(780, 290, 270), 4, false),
		Entry(TEXT("TrackedIFV"),      ESynthVehicleClass::IFV,          FVector(660, 320, 260), 0, true),
		Entry(TEXT("MainBattleTank"),  ESynthVehicleClass::MBT,          FVector(990, 370, 245), 0, true)
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

	Stream.Initialize(RandomSeed);
	UE_LOG(LogSynthic, Log, TEXT("Director: %d passes, seed %d, speeds %.0f-%.0f km/h."),
		NumPasses, RandomSeed, MinSpeedKph, MaxSpeedKph);

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

bool ASynthCaptureDirector::DispatchNextVehicle()
{
	if (PassesDispatched >= NumPasses)
	{
		UE_LOG(LogSynthic, Log, TEXT("Director: run complete, %d passes dispatched."), PassesDispatched);
		return false;
	}

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

	UE_LOG(LogSynthic, Verbose, TEXT("Director: pass %d/%d - %s at %.1f km/h, lane offset %.0fcm."),
		PassesDispatched, NumPasses, *Spec.Model, SpeedKph, LaneOffset);

	return true;
}
