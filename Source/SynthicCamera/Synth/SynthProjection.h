#pragma once

#include "CoreMinimal.h"

/** A 2D bounding box in image pixels, plus why it might not be usable. */
struct FSynthScreenBox
{
	float MinX = 0.0f;
	float MinY = 0.0f;
	float MaxX = 0.0f;
	float MaxY = 0.0f;

	/** False when the box crosses the near plane or falls entirely off-image. */
	bool bValid = false;

	/** True when the box was clamped to the image edge - the vehicle is part-out-of-frame. */
	bool bClipped = false;

	float Width() const { return MaxX - MinX; }
	float Height() const { return MaxY - MinY; }
};

/**
 * Pure projection helpers. No actors, no world, no side effects - just matrices in,
 * pixels out, so the label geometry can be checked without standing up an engine.
 */
/**
 * A projected oriented box: the eight corners in pixels plus their axis-aligned
 * envelope.
 *
 * The corners carry orientation the envelope throws away - which face is toward the
 * camera, how the vehicle is yawed - which is what a 3D-cuboid annotation needs and
 * a 2D rectangle cannot express.
 */
struct FSynthProjectedBox
{
	/** Corner order matches SynthProjection::BoxCornersToWorld: bit 0 = +X, 1 = +Y, 2 = +Z. */
	TArray<FVector2D, TInlineAllocator<8>> Corners;

	FSynthScreenBox Bounds;
};

namespace SynthProjection
{
	/**
	 * Project a world point through a view-projection matrix into pixel coordinates.
	 * Returns false if the point is at or behind the near plane, in which case
	 * OutPixel is left untouched.
	 */
	bool WorldToPixel(const FMatrix& ViewProj, const FVector& WorldPoint,
		int32 ImageWidth, int32 ImageHeight, FVector2D& OutPixel);

	/** The 8 corners of a local-space box, transformed into world space. */
	void BoxCornersToWorld(const FTransform& BoxToWorld, const FVector& LocalCenter,
		const FVector& LocalExtent, TArray<FVector, TInlineAllocator<8>>& OutCorners);

	/** Project all eight corners and their envelope in one pass. */
	FSynthProjectedBox ProjectOrientedBox(const FMatrix& ViewProj, const FTransform& BoxToWorld,
		const FVector& LocalCenter, const FVector& LocalExtent, int32 ImageWidth, int32 ImageHeight);

	/**
	 * Axis-aligned screen bounds of an oriented world-space box. Invalid if any corner
	 * is behind the near plane (the min/max would be meaningless) or if the box lands
	 * wholly off-image.
	 */
	FSynthScreenBox OrientedBoxToScreenBounds(const FMatrix& ViewProj, const FTransform& BoxToWorld,
		const FVector& LocalCenter, const FVector& LocalExtent, int32 ImageWidth, int32 ImageHeight);
}
