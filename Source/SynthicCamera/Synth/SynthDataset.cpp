#include "Synth/SynthDataset.h"

#include "SynthicCamera.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace SynthDataset
{

bool SavePng(const FString& AbsolutePath, const TArray<FColor>& Pixels, int32 Width, int32 Height)
{
	const int64 Expected = static_cast<int64>(Width) * static_cast<int64>(Height);
	if (Width <= 0 || Height <= 0 || Pixels.Num() != Expected)
	{
		UE_LOG(LogSynthic, Error, TEXT("SavePng: %dx%d needs %lld pixels but got %d; not writing '%s'."),
			Width, Height, Expected, Pixels.Num(), *AbsolutePath);
		return false;
	}

	IImageWrapperModule& ImageWrapperModule =
		FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));

	const TSharedPtr<IImageWrapper> PngWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
	if (!PngWrapper.IsValid())
	{
		UE_LOG(LogSynthic, Error, TEXT("SavePng: could not create a PNG wrapper."));
		return false;
	}

	if (!PngWrapper->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
	{
		UE_LOG(LogSynthic, Error, TEXT("SavePng: PNG wrapper rejected the raw buffer for '%s'."), *AbsolutePath);
		return false;
	}

	const TArray64<uint8>& Compressed = PngWrapper->GetCompressed(100);
	if (Compressed.Num() == 0)
	{
		UE_LOG(LogSynthic, Error, TEXT("SavePng: PNG compression produced no bytes for '%s'."), *AbsolutePath);
		return false;
	}

	if (!FFileHelper::SaveArrayToFile(Compressed, *AbsolutePath))
	{
		UE_LOG(LogSynthic, Error, TEXT("SavePng: failed writing '%s' (disk full or path not writable?)."), *AbsolutePath);
		return false;
	}

	return true;
}

bool AppendJsonLine(const FString& AbsolutePath, const TSharedRef<FJsonObject>& Row)
{
	FString Line;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Line);

	if (!FJsonSerializer::Serialize(Row, Writer))
	{
		UE_LOG(LogSynthic, Error, TEXT("AppendJsonLine: serialisation failed; dropping a label for '%s'."), *AbsolutePath);
		return false;
	}

	Line.Append(LINE_TERMINATOR);

	if (!FFileHelper::SaveStringToFile(Line, *AbsolutePath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
		&IFileManager::Get(), EFileWrite::FILEWRITE_Append))
	{
		UE_LOG(LogSynthic, Error, TEXT("AppendJsonLine: failed appending to '%s'; label lost."), *AbsolutePath);
		return false;
	}

	return true;
}

}	// namespace SynthDataset
