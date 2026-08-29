#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * The only place this module touches the filesystem. Kept as two free functions so
 * everything upstream - projection, labelling, kinematics - stays side-effect free
 * and can be reasoned about without a disk.
 */
namespace SynthDataset
{
	/** Write BGRA pixels as a PNG. Returns false and logs on any failure. */
	bool SavePng(const FString& AbsolutePath, const TArray<FColor>& Pixels, int32 Width, int32 Height);

	/** Append one condensed JSON object as a single line (JSONL). Creates the file if absent. */
	bool AppendJsonLine(const FString& AbsolutePath, const TSharedRef<FJsonObject>& Row);
}
