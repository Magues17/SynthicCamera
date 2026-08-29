#include "Synth/SynthVehicle.h"

#include "SynthicCamera.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

namespace
{
	/** Engine primitive used for proxy geometry: a 100uu cube centred on its origin. */
	const TCHAR* const ProxyCubePath = TEXT("/Engine/BasicShapes/Cube.Cube");
	const TCHAR* const ProxyCylinderPath = TEXT("/Engine/BasicShapes/Cylinder.Cylinder");
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

	bLiveryApplied = false;

	if (UStaticMesh* Authored = Spec.Mesh.LoadSynchronous())
	{
		// Real asset: trust its authored origin, scale and materials. Painting the
		// livery over it would replace textured bodywork, glass and tyres with one
		// flat colour, which is worse than the proxy it replaced.
		VehicleMesh->SetStaticMesh(Authored);
		VehicleMesh->SetRelativeLocation(FVector::ZeroVector);
		VehicleMesh->SetRelativeScale3D(FVector::OneVector);

		for (UStaticMeshComponent* Old : PartMeshes)
		{
			if (Old)
			{
				Old->DestroyComponent();
			}
		}
		PartMeshes.Reset();

		CacheVisualBounds();
		return;
	}
	else if (Spec.Parts.Num() > 0)
	{
		BuildAssembledGeometry();
	}
	else
	{
		BuildProxyGeometry();
	}

	if (VehicleMesh->GetStaticMesh())
	{
		ApplyLivery(*VehicleMesh, 1.0f);
	}

	bLiveryApplied = true;
	CacheVisualBounds();
}

void ASynthVehicle::BuildAssembledGeometry()
{
	// The single-box fallback must go, or it sits inside the assembly as a slab that
	// fills the silhouette and makes every class look the same again.
	VehicleMesh->SetStaticMesh(nullptr);

	for (UStaticMeshComponent* Old : PartMeshes)
	{
		if (Old)
		{
			Old->DestroyComponent();
		}
	}
	PartMeshes.Reset();

	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, ProxyCubePath);
	UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, ProxyCylinderPath);
	if (!Cube || !Cylinder)
	{
		UE_LOG(LogSynthic, Error,
			TEXT("BuildAssembledGeometry: engine primitives missing; %s %s falls back to a box."),
			*Spec.Make, *Spec.Model);
		BuildProxyGeometry();
		return;
	}

	for (int32 Index = 0; Index < Spec.Parts.Num(); ++Index)
	{
		const FSynthVehiclePart& Part = Spec.Parts[Index];

		UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(this);
		Component->SetupAttachment(VehicleRoot);
		Component->RegisterComponent();

		Component->SetStaticMesh(Part.Shape == ESynthPartShape::Cylinder ? Cylinder : Cube);

		// Scale is applied in the component's own space, before the rotation, so a
		// cylinder is sized as (diameter, diameter, length) and then laid on its side
		// to become a wheel.
		Component->SetRelativeScale3D(Part.SizeCm / ProxyCubeSizeCm);
		Component->SetRelativeRotation(Part.Rotation);
		Component->SetRelativeLocation(Part.OffsetCm);

		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetMobility(EComponentMobility::Movable);

		ApplyLivery(*Component, Part.ColorScale);
		PartMeshes.Add(Component);
	}
}

void ASynthVehicle::ApplyLivery(UStaticMeshComponent& Component, float ColorScale) const
{
	if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, ProxyMaterialPath))
	{
		Component.SetMaterial(0, Base);
	}

	if (UMaterialInstanceDynamic* Dynamic = Component.CreateAndSetMaterialInstanceDynamic(0))
	{
		Dynamic->SetVectorParameterValue(TEXT("Color"), Spec.LiveryColor * ColorScale);
	}
}

void ASynthVehicle::CacheVisualBounds()
{
	if (Spec.Parts.Num() == 0)
	{
		// Single mesh: its own bounds, expressed relative to the root.
		if (const UStaticMesh* Mesh = VehicleMesh->GetStaticMesh())
		{
			const FBoxSphereBounds Local = Mesh->GetBounds();
			const FVector Scale = VehicleMesh->GetRelativeScale3D();
			LocalBoundsCentre = VehicleMesh->GetRelativeLocation() + (Local.Origin * Scale);
			LocalBoundsExtent = Local.BoxExtent * Scale;
			return;
		}

		LocalBoundsCentre = FVector(0.0, 0.0, Spec.DimensionsCm.Z * 0.5);
		LocalBoundsExtent = Spec.DimensionsCm * 0.5;
		return;
	}

	// Parts are authored in root-local space, so the union of their boxes in that
	// space is exact - no need to go through world space and back.
	FBox Bounds(ForceInit);
	for (const FSynthVehiclePart& Part : Spec.Parts)
	{
		const FVector Half = Part.SizeCm * 0.5;
		const FBox Local(-Half, Half);
		Bounds += Local.TransformBy(FTransform(Part.Rotation, Part.OffsetCm));
	}

	LocalBoundsCentre = Bounds.GetCenter();
	LocalBoundsExtent = Bounds.GetExtent();
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
	// Root transform, not the mesh's: parts are authored relative to the root, and the
	// cached bounds already cover all of them.
	OutBoxToWorld = VehicleRoot->GetComponentTransform();
	OutLocalCenter = LocalBoundsCentre;
	OutLocalExtent = LocalBoundsExtent;
}
