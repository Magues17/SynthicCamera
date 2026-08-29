#include "Synth/SynthProjection.h"

#include "Camera/CameraTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr int32 TestImageWidth = 1280;
	constexpr int32 TestImageHeight = 720;

	/** Camera at the origin looking down +X: forward +X, right +Y, up +Z. */
	FMatrix MakeTestViewProjection()
	{
		FMinimalViewInfo View;
		View.Location = FVector::ZeroVector;
		View.Rotation = FRotator::ZeroRotator;
		View.FOV = 50.0f;
		View.AspectRatio = static_cast<float>(TestImageWidth) / static_cast<float>(TestImageHeight);
		View.bConstrainAspectRatio = true;
		View.ProjectionMode = ECameraProjectionMode::Perspective;

		FMatrix ViewMatrix;
		FMatrix ProjectionMatrix;
		FMatrix ViewProjectionMatrix;
		UGameplayStatics::GetViewProjectionMatrix(View, ViewMatrix, ProjectionMatrix, ViewProjectionMatrix);
		return ViewProjectionMatrix;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSynthProjectionTest, "Synthic.Projection.WorldToPixelAndBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSynthProjectionTest::RunTest(const FString& /*Parameters*/)
{
	const FMatrix ViewProjection = MakeTestViewProjection();
	const float CentreX = TestImageWidth * 0.5f;
	const float CentreY = TestImageHeight * 0.5f;

	// A point straight ahead lands dead centre.
	FVector2D Pixel;
	TestTrue(TEXT("point ahead projects"),
		SynthProjection::WorldToPixel(ViewProjection, FVector(1000, 0, 0), TestImageWidth, TestImageHeight, Pixel));
	TestEqual(TEXT("centred horizontally"), static_cast<float>(Pixel.X), CentreX, 1.0f);
	TestEqual(TEXT("centred vertically"), static_cast<float>(Pixel.Y), CentreY, 1.0f);

	// +Y is camera-right, +Z is camera-up (and image Y grows downward).
	FVector2D RightPixel;
	SynthProjection::WorldToPixel(ViewProjection, FVector(1000, 200, 0), TestImageWidth, TestImageHeight, RightPixel);
	TestTrue(TEXT("+Y projects right of centre"), RightPixel.X > CentreX);

	FVector2D UpPixel;
	SynthProjection::WorldToPixel(ViewProjection, FVector(1000, 0, 200), TestImageWidth, TestImageHeight, UpPixel);
	TestTrue(TEXT("+Z projects above centre"), UpPixel.Y < CentreY);

	// Behind the camera must fail rather than fold back into frame.
	FVector2D BehindPixel;
	TestFalse(TEXT("point behind camera is rejected"),
		SynthProjection::WorldToPixel(ViewProjection, FVector(-1000, 0, 0), TestImageWidth, TestImageHeight, BehindPixel));

	// A 4m x 2m x 2m box 20m ahead: valid, unclipped, and centred.
	const FSynthScreenBox Box = SynthProjection::OrientedBoxToScreenBounds(ViewProjection,
		FTransform(FVector(2000, 0, 0)), FVector::ZeroVector, FVector(200, 100, 100),
		TestImageWidth, TestImageHeight);

	TestTrue(TEXT("box ahead is valid"), Box.bValid);
	TestFalse(TEXT("box ahead is not clipped"), Box.bClipped);
	TestTrue(TEXT("box has area"), Box.Width() > 1.0f && Box.Height() > 1.0f);
	TestEqual(TEXT("box centred horizontally"), (Box.MinX + Box.MaxX) * 0.5f, CentreX, 2.0f);

	// A nearer box must subtend more pixels than a distant one.
	const FSynthScreenBox NearBox = SynthProjection::OrientedBoxToScreenBounds(ViewProjection,
		FTransform(FVector(1000, 0, 0)), FVector::ZeroVector, FVector(200, 100, 100),
		TestImageWidth, TestImageHeight);
	TestTrue(TEXT("nearer box is larger"), NearBox.Width() > Box.Width());

	// Far off the boresight the box leaves the frame entirely.
	const FSynthScreenBox OffScreen = SynthProjection::OrientedBoxToScreenBounds(ViewProjection,
		FTransform(FVector(1000, 90000, 0)), FVector::ZeroVector, FVector(200, 100, 100),
		TestImageWidth, TestImageHeight);
	TestFalse(TEXT("off-frame box is invalid"), OffScreen.bValid);

	return true;
}

#endif	// WITH_DEV_AUTOMATION_TESTS
