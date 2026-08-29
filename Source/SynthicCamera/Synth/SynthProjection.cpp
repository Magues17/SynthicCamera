#include "Synth/SynthProjection.h"

namespace SynthProjection
{

bool WorldToPixel(const FMatrix& ViewProj, const FVector& WorldPoint,
	int32 ImageWidth, int32 ImageHeight, FVector2D& OutPixel)
{
	if (ImageWidth <= 0 || ImageHeight <= 0)
	{
		return false;
	}

	const FPlane Clip = ViewProj.TransformFVector4(FVector4(WorldPoint, 1.0f));
	if (Clip.W <= UE_SMALL_NUMBER)
	{
		return false;	// at or behind the near plane
	}

	const float InvW = 1.0f / Clip.W;
	const float NdcX = Clip.X * InvW;
	const float NdcY = Clip.Y * InvW;

	// NDC is [-1,1] with +Y up; image space is [0,size] with +Y down.
	const float NormX = (NdcX * 0.5f) + 0.5f;
	const float NormY = 1.0f - ((NdcY * 0.5f) + 0.5f);

	OutPixel = FVector2D(NormX * ImageWidth, NormY * ImageHeight);
	return true;
}

void BoxCornersToWorld(const FTransform& BoxToWorld, const FVector& LocalCenter,
	const FVector& LocalExtent, TArray<FVector, TInlineAllocator<8>>& OutCorners)
{
	OutCorners.Reset();
	for (int32 Corner = 0; Corner < 8; ++Corner)
	{
		const FVector Signs(
			(Corner & 1) ? 1.0 : -1.0,
			(Corner & 2) ? 1.0 : -1.0,
			(Corner & 4) ? 1.0 : -1.0);

		OutCorners.Add(BoxToWorld.TransformPosition(LocalCenter + LocalExtent * Signs));
	}
}

FSynthProjectedBox ProjectOrientedBox(const FMatrix& ViewProj, const FTransform& BoxToWorld,
	const FVector& LocalCenter, const FVector& LocalExtent, int32 ImageWidth, int32 ImageHeight)
{
	FSynthProjectedBox Result;

	TArray<FVector, TInlineAllocator<8>> Corners;
	BoxCornersToWorld(BoxToWorld, LocalCenter, LocalExtent, Corners);

	float MinX = TNumericLimits<float>::Max();
	float MinY = TNumericLimits<float>::Max();
	float MaxX = TNumericLimits<float>::Lowest();
	float MaxY = TNumericLimits<float>::Lowest();

	for (const FVector& Corner : Corners)
	{
		FVector2D Pixel;
		if (!WorldToPixel(ViewProj, Corner, ImageWidth, ImageHeight, Pixel))
		{
			// One corner behind the near plane makes the whole envelope meaningless,
			// so report nothing rather than a plausible-looking partial box.
			Result.Corners.Reset();
			return Result;
		}

		Result.Corners.Add(Pixel);
		MinX = FMath::Min(MinX, static_cast<float>(Pixel.X));
		MinY = FMath::Min(MinY, static_cast<float>(Pixel.Y));
		MaxX = FMath::Max(MaxX, static_cast<float>(Pixel.X));
		MaxY = FMath::Max(MaxY, static_cast<float>(Pixel.Y));
	}

	const float ImageW = static_cast<float>(ImageWidth);
	const float ImageH = static_cast<float>(ImageHeight);

	FSynthScreenBox& Box = Result.Bounds;
	Box.bClipped = (MinX < 0.0f) || (MinY < 0.0f) || (MaxX > ImageW) || (MaxY > ImageH);
	Box.MinX = FMath::Clamp(MinX, 0.0f, ImageW);
	Box.MinY = FMath::Clamp(MinY, 0.0f, ImageH);
	Box.MaxX = FMath::Clamp(MaxX, 0.0f, ImageW);
	Box.MaxY = FMath::Clamp(MaxY, 0.0f, ImageH);
	Box.bValid = (Box.Width() > 0.0f) && (Box.Height() > 0.0f);

	return Result;
}

FSynthScreenBox OrientedBoxToScreenBounds(const FMatrix& ViewProj, const FTransform& BoxToWorld,
	const FVector& LocalCenter, const FVector& LocalExtent, int32 ImageWidth, int32 ImageHeight)
{
	return ProjectOrientedBox(ViewProj, BoxToWorld, LocalCenter, LocalExtent,
		ImageWidth, ImageHeight).Bounds;
}

}	// namespace SynthProjection
