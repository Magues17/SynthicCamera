#include "Synth/SynthCaptureDirector.h"
#include "Synth/SynthTypes.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Two Car archetypes against one of everything else - the real catalog's shape. */
	TArray<ESynthVehicleClass> MixedCatalog()
	{
		return {
			ESynthVehicleClass::Car,			// Hatchback
			ESynthVehicleClass::Car,			// SportsCar - a second archetype
			ESynthVehicleClass::SUV,
			ESynthVehicleClass::Pickup,
			ESynthVehicleClass::BoxTruck,
			ESynthVehicleClass::LightUtility,
			ESynthVehicleClass::CargoTruck,
			ESynthVehicleClass::APC,
			ESynthVehicleClass::IFV,
			ESynthVehicleClass::MBT
		};
	}

	TMap<ESynthVehicleClass, int32> CountByClass(
		const TArray<int32>& Schedule, const TArray<ESynthVehicleClass>& Classes)
	{
		TMap<ESynthVehicleClass, int32> Counts;
		for (const int32 Index : Schedule)
		{
			++Counts.FindOrAdd(Classes[Index]);
		}
		return Counts;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSynthScheduleTest, "Synthic.Schedule.BalancedByClass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSynthScheduleTest::RunTest(const FString& /*Parameters*/)
{
	const TArray<ESynthVehicleClass> Classes = MixedCatalog();
	const int32 DistinctClasses = 9;		// ten archetypes, Car appearing twice

	// Exactly divisible: every class must land on the same count.
	{
		FRandomStream Stream(1337);
		const TArray<int32> Schedule =
			ASynthCaptureDirector::BuildBalancedSchedule(Classes, DistinctClasses * 4, Stream);

		TestEqual(TEXT("schedule length matches passes"), Schedule.Num(), DistinctClasses * 4);

		const TMap<ESynthVehicleClass, int32> Counts = CountByClass(Schedule, Classes);
		TestEqual(TEXT("every class present"), Counts.Num(), DistinctClasses);
		for (const TPair<ESynthVehicleClass, int32>& Pair : Counts)
		{
			// The failure this guards: uniform sampling starved a class to one sample
			// while another took six. An exact split must stay exact.
			TestEqual(TEXT("class count is even"), Pair.Value, 4);
		}
	}

	// Not divisible: counts may differ by one, never more, and none may be zero.
	{
		FRandomStream Stream(99);
		const TArray<int32> Schedule =
			ASynthCaptureDirector::BuildBalancedSchedule(Classes, 30, Stream);

		TestEqual(TEXT("schedule length matches passes"), Schedule.Num(), 30);

		const TMap<ESynthVehicleClass, int32> Counts = CountByClass(Schedule, Classes);
		TestEqual(TEXT("every class present"), Counts.Num(), DistinctClasses);

		int32 Low = TNumericLimits<int32>::Max();
		int32 High = 0;
		for (const TPair<ESynthVehicleClass, int32>& Pair : Counts)
		{
			Low = FMath::Min(Low, Pair.Value);
			High = FMath::Max(High, Pair.Value);
		}
		TestTrue(TEXT("counts differ by at most one"), High - Low <= 1);
		TestTrue(TEXT("no class is starved"), Low >= 1);
	}

	// A class with two archetypes must use both, or the extra asset never appears.
	{
		FRandomStream Stream(7);
		const TArray<int32> Schedule =
			ASynthCaptureDirector::BuildBalancedSchedule(Classes, DistinctClasses * 4, Stream);

		TestTrue(TEXT("first Car archetype used"), Schedule.Contains(0));
		TestTrue(TEXT("second Car archetype used"), Schedule.Contains(1));
	}

	// Same seed, same schedule - a dataset that cannot be regenerated cannot be debugged.
	{
		FRandomStream First(4242);
		FRandomStream Second(4242);
		TestTrue(TEXT("schedule is reproducible"),
			ASynthCaptureDirector::BuildBalancedSchedule(Classes, 25, First) ==
			ASynthCaptureDirector::BuildBalancedSchedule(Classes, 25, Second));
	}

	// Degenerate inputs return empty rather than indexing off the end.
	{
		FRandomStream Stream(1);
		TestEqual(TEXT("no passes yields nothing"),
			ASynthCaptureDirector::BuildBalancedSchedule(Classes, 0, Stream).Num(), 0);
		TestEqual(TEXT("no catalog yields nothing"),
			ASynthCaptureDirector::BuildBalancedSchedule({}, 10, Stream).Num(), 0);
	}

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
