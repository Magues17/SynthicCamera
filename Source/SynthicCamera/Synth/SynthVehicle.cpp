#include "Synth/SynthVehicle.h"

#include "SynthicCamera.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

namespace
{
	/** Engine primitive used for proxy geometry: a 100uu cube centred on its origin. */
	const TCHAR* const ProxyCubePath = TEXT("/Engine/BasicShapes/Cube.Cube");
	/** Built by build_desert.py; exposes a "Color" parameter the livery can drive. */
	const TCHAR* const ProxyMaterialPath = TEXT("/Game/Materials/M_VehicleBody.M_VehicleBody");
	const TCHAR* const FallbackMaterialPath = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");
	constexpr float ProxyCubeSizeCm = 100.0f;
}

ASynthVehicle::ASynthVehicle()
{
	PrimaryActorTick.bCanEverTick = true;

	VehicleRoot = CreateDefaultSubobject<USceneComponent>(TEXT("VehicleRoot"));
	SetRootComponent(VehicleRoot);

	VehicleMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("VehicleMesh"));
	VehicleMesh->SetupAttachment(VehicleRoot);

	// Query-only: the camera's trigger needs to see it, but nothing needs to be blocked.
	VehicleMesh->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	VehicleMesh->SetCollisionProfileName(TEXT("OverlapAllDynamic"));
	VehicleMesh->SetGenerateOverlapEvents(true);
	VehicleMesh->SetMobility(EComponentMobility::Movable);
}

void ASynthVehicle::ApplySpec(const FSynthVehicleSpec& InSpec)
{
	Spec = InSpec;

	if (UStaticMesh* Authored = Spec.Mesh.LoadSynchronous())
	{
		// Real asset: trust its authored origin and scale, only recolour it.
		VehicleMesh->SetStaticMesh(Authored);
		VehicleMesh->SetRelativeLocation(FVector::ZeroVector);
		VehicleMesh->SetRelativeScale3D(FVector::OneVector);
	}
	else
	{
		BuildProxyGeometry();
	}

	if (UMaterialInterface* Base = VehicleMesh->GetMaterial(0))
	{
		UMaterialInstanceDynamic* Dynamic = VehicleMesh->CreateAndSetMaterialInstanceDynamic(0);
		if (Dynamic)
		{
			// ponytail: proxy material exposes "Color"; a real asset may name it otherwise,
			// in which case this is a harmless no-op until the catalog names the parameter.
			Dynamic->SetVectorParameterValue(TEXT("Color"), Spec.LiveryColor);
		}
	}
}

void ASynthVehicle::BuildProxyGeometry()
{
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, ProxyCubePath);
	if (!Cube)
	{
		UE_LOG(LogSynthic, Error,
			TEXT("ApplySpec: proxy cube '%s' failed to load; %s %s will be invisible."),
			ProxyCubePath, *Spec.Make, *Spec.Model);
		return;
	}

	VehicleMesh->SetStaticMesh(Cube);
	VehicleMesh->SetRelativeScale3D(Spec.DimensionsCm / ProxyCubeSizeCm);

	UMaterialInterface* ProxyMaterial = LoadObject<UMaterialInterface>(nullptr, ProxyMaterialPath);
	if (!ProxyMaterial)
	{
		// Say so loudly: the fallback has no Color parameter, so every vehicle would
		// render identically while the labels still claim a livery per vehicle.
		UE_LOG(LogSynthic, Warning,
			TEXT("Proxy material '%s' missing - falling back to an unparameterised material, "
				 "so LiveryColor will NOT be visible. Re-run build_desert.py to create it."),
			ProxyMaterialPath);
		ProxyMaterial = LoadObject<UMaterialInterface>(nullptr, FallbackMaterialPath);
	}

	if (ProxyMaterial)
	{
		VehicleMesh->SetMaterial(0, ProxyMaterial);
	}

	// The cube is origin-centred; lift it so the actor origin is the ground contact point,
	// which is what the road spline and the camera's ground-plane maths both assume.
	VehicleMesh->SetRelativeLocation(FVector(0.0, 0.0, Spec.DimensionsCm.Z * 0.5));
}

void ASynthVehicle::DriveAlong(const FVector& Direction, float SpeedKph)
{
	FVector Normalised = Direction;
	if (!Normalised.Normalize())
	{
		UE_LOG(LogSynthic, Error, TEXT("DriveAlong: zero-length direction for %s %s; vehicle will not move."),
			*Spec.Make, *Spec.Model);
		return;
	}

	DriveDirection = Normalised;
	CommandedSpeedKph = SpeedKph;
	DistanceTravelledCm = 0.0f;
	MeasuredVelocity = FVector::ZeroVector;

	SetActorRotation(DriveDirection.Rotation());
}

void ASynthVehicle::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (CommandedSpeedKph <= 0.0f || DeltaSeconds <= UE_SMALL_NUMBER)
	{
		return;
	}

	// Measure the displacement actually applied, against the delta it was applied for.
	// Comparing positions across two frames instead would divide one frame's movement
	// by another frame's delta, which is meaningless whenever frame times are uneven -
	// and offscreen rendering makes them very uneven.
	const FVector Before = GetActorLocation();
	AddActorWorldOffset(DriveDirection * (CommandedSpeedKph * KphToCmPerSec * DeltaSeconds), false);
	const FVector Applied = GetActorLocation() - Before;

	MeasuredVelocity = Applied / DeltaSeconds;
	DistanceTravelledCm += static_cast<float>(Applied.Size());
}

void ASynthVehicle::GetVisualBounds(FTransform& OutBoxToWorld, FVector& OutLocalCenter, FVector& OutLocalExtent) const
{
	OutBoxToWorld = VehicleMesh->GetComponentTransform();

	if (const UStaticMesh* Mesh = VehicleMesh->GetStaticMesh())
	{
		const FBoxSphereBounds Local = Mesh->GetBounds();
		OutLocalCenter = Local.Origin;
		OutLocalExtent = Local.BoxExtent;
		return;
	}

	OutLocalCenter = FVector::ZeroVector;
	OutLocalExtent = Spec.DimensionsCm * 0.5;
}
