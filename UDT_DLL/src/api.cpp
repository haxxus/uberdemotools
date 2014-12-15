#include "api.h"
#include "parser_context.hpp"
#include "common.hpp"
#include "utils.hpp"
#include "file_system.hpp"
#include "timer.hpp"
#include "crash.hpp"
#include "scoped_stack_allocator.hpp"
#include "multi_threaded_processing.hpp"
#include "analysis_splitter.hpp"
#include "analysis_cut_by_chat.hpp"
#include "analysis_cut_by_frag.hpp"

// For malloc and free.
#include <stdlib.h>
#if defined(UDT_MSVC)
#	include <malloc.h>
#endif

// For the placement new operator.
#include <new>


#define UDT_API UDT_API_DEF


static const char* VersionString = "0.3.3";


#define UDT_ERROR_ITEM(Enum, Desc) Desc,
static const char* ErrorCodeStrings[udtErrorCode::AfterLastError + 1] =
{
	UDT_ERROR_LIST(UDT_ERROR_ITEM)
	"invalid error code"
};
#undef UDT_ERROR_ITEM

#define UDT_PROTOCOL_ITEM(Enum, Ext) Ext,
static const char* DemoFileExtensions[udtProtocol::AfterLastProtocol + 1] =
{
	UDT_PROTOCOL_LIST(UDT_PROTOCOL_ITEM)
	".after_last"
};
#undef UDT_PROTOCOL_ITEM

#define UDT_WEAPON_ITEM(Enum, Desc, Bit) Desc,
static const char* WeaponNames[] =
{
	UDT_WEAPON_LIST(UDT_WEAPON_ITEM)
	"after last weapon"
};
#undef UDT_WEAPON_ITEM

#define UDT_POWER_UP_ITEM(Enum, Desc, Bit) Desc,
static const char* PowerUpNames[] =
{
	UDT_POWER_UP_LIST(UDT_POWER_UP_ITEM)
	"after last power-up"
};
#undef UDT_POWER_UP_ITEM

#define UDT_MOD_ITEM(Enum, Desc, Bit) Desc,
static const char* MeansOfDeathNames[] =
{
	UDT_MOD_LIST(UDT_MOD_ITEM)
	"after last MoD"
};
#undef UDT_MOD_ITEM

#define UDT_PLAYER_MOD_ITEM(Enum, Desc, Bit) Desc,
static const char* PlayerMeansOfDeathNames[] =
{
	UDT_PLAYER_MOD_LIST(UDT_PLAYER_MOD_ITEM)
	"after last player MoD"
};
#undef UDT_PLAYER_MOD_ITEM

#define UDT_TEAM_ITEM(Enum, Desc) Desc,
static const char* TeamNames[] =
{
	UDT_TEAM_LIST(UDT_TEAM_ITEM)
	"after last team"
};
#undef UDT_TEAM_ITEM


struct SingleThreadProgressContext
{
	u64 TotalByteCount;
	u64 ProcessedByteCount;
	u64 CurrentJobByteCount;
	udtProgressCallback UserCallback;
	void* UserData;
	udtTimer* Timer;
};

static void SingleThreadProgressCallback(f32 jobProgress, void* userData)
{
	SingleThreadProgressContext* const context = (SingleThreadProgressContext*)userData;
	if(context == NULL || context->Timer == NULL || context->UserCallback == NULL)
	{
		return;
	}

	if(context->Timer->GetElapsedMs() < UDT_MIN_PROGRESS_TIME_MS)
	{
		return;
	}

	context->Timer->Restart();

	const u64 jobProcessed = (u64)((f64)context->CurrentJobByteCount * (f64)jobProgress);
	const u64 totalProcessed = context->ProcessedByteCount + jobProcessed;
	const f32 realProgress = udt_clamp((f32)totalProcessed / (f32)context->TotalByteCount, 0.0f, 1.0f);

	(*context->UserCallback)(realProgress, context->UserData);
}

UDT_API(const char*) udtGetVersionString()
{
	return VersionString;
}

UDT_API(const char*) udtGetErrorCodeString(s32 errorCode)
{
	if(errorCode < 0 || errorCode > (s32)udtErrorCode::AfterLastError)
	{
		errorCode = (s32)udtErrorCode::AfterLastError;
	}

	return ErrorCodeStrings[errorCode];
}

UDT_API(s32) udtIsValidProtocol(udtProtocol::Id protocol)
{
	return (protocol >= udtProtocol::AfterLastProtocol || (s32)protocol < (s32)1) ? 0 : 1;
}

UDT_API(u32) udtGetSizeOfIdEntityState(udtProtocol::Id protocol)
{
	if(protocol == udtProtocol::Dm68)
	{
		return sizeof(idEntityState68);
	}

	if(protocol == udtProtocol::Dm73)
	{
		return sizeof(idEntityState73);
	}

	if(protocol == udtProtocol::Dm90)
	{
		return sizeof(idEntityState90);
	}

	return 0;
}

UDT_API(u32) udtGetSizeOfidClientSnapshot(udtProtocol::Id protocol)
{
	if(protocol == udtProtocol::Dm68)
	{
		return sizeof(idClientSnapshot68);
	}

	if(protocol == udtProtocol::Dm73)
	{
		return sizeof(idClientSnapshot73);
	}

	if(protocol == udtProtocol::Dm90)
	{
		return sizeof(idClientSnapshot90);
	}

	return 0;
}

UDT_API(const char*) udtGetFileExtensionByProtocol(udtProtocol::Id protocol)
{
	if(!udtIsValidProtocol(protocol))
	{
		return NULL;
	}

	return DemoFileExtensions[protocol];
}

UDT_API(udtProtocol::Id) udtGetProtocolByFilePath(const char* filePath)
{
	for(s32 i = (s32)udtProtocol::Invalid + 1; i < (s32)udtProtocol::AfterLastProtocol; ++i)
	{
		if(StringEndsWith_NoCase(filePath, DemoFileExtensions[i]))
		{
			return (udtProtocol::Id)i;
		}
	}
	
	return udtProtocol::Invalid;
}

UDT_API(s32) udtCrash(udtCrashType::Id crashType)
{
	if((u32)crashType >= (u32)udtCrashType::Count)
	{
		return udtErrorCode::InvalidArgument;
	}

	switch(crashType)
	{
		case udtCrashType::FatalError:
			FatalError(__FILE__, __LINE__, __FUNCTION__, "udtCrash test");
			break;

		case udtCrashType::ReadAccess:
			printf("Bullshit: %d\n", *(int*)0);
			break;

		case udtCrashType::WriteAccess:
			*(int*)0 = 1337;
			break;

		default:
			break;
	}

	return (s32)udtErrorCode::None;
}

UDT_API(s32) udtGetStringArray(udtStringArray::Id arrayId, const char*** elements, u32* elementCount)
{
	if(elements == NULL || elementCount == NULL)
	{
		return (s32)udtErrorCode::InvalidArgument;
	}

	switch(arrayId)
	{
		case udtStringArray::Weapons:
			*elements = WeaponNames;
			*elementCount = (u32)(UDT_COUNT_OF(WeaponNames) - 1);
			break;

		case udtStringArray::PowerUps:
			*elements = PowerUpNames;
			*elementCount = (u32)(UDT_COUNT_OF(PowerUpNames) - 1);
			break;

		case udtStringArray::MeansOfDeath:
			*elements = MeansOfDeathNames;
			*elementCount = (u32)(UDT_COUNT_OF(MeansOfDeathNames) - 1);
			break;

		case udtStringArray::PlayerMeansOfDeath:
			*elements = PlayerMeansOfDeathNames;
			*elementCount = (u32)(UDT_COUNT_OF(PlayerMeansOfDeathNames) - 1);
			break;

		case udtStringArray::Teams:
			*elements = TeamNames;
			*elementCount = (u32)(UDT_COUNT_OF(TeamNames) - 1);
			break;

		default:
			return (s32)udtErrorCode::InvalidArgument;
	}

	return (s32)udtErrorCode::None;
}

UDT_API(s32) udtSetCrashHandler(udtCrashCallback crashHandler)
{
	SetCrashHandler(crashHandler);

	return (s32)udtErrorCode::None;
}

static bool CreateDemoFileSplit(udtContext& context, udtStream& file, const char* filePath, const char* outputFolderPath, u32 index, u32 startOffset, u32 endOffset)
{
	if(endOffset <= startOffset)
	{
		return false;
	}

	if(file.Seek((s32)startOffset, udtSeekOrigin::Start) != 0)
	{
		return false;
	}

	const udtProtocol::Id protocol = udtGetProtocolByFilePath(filePath);
	if(protocol == udtProtocol::Invalid)
	{
		return false;
	}

	udtVMLinearAllocator& tempAllocator = context.TempAllocator;
	udtVMScopedStackAllocator scopedTempAllocator(tempAllocator);

	char* fileName = NULL;
	if(!GetFileNameWithoutExtension(fileName, tempAllocator, filePath))
	{
		fileName = AllocateString(tempAllocator, "NEW_UDT_SPLIT_DEMO");
	}
	
	char* outputFilePathStart = NULL;
	if(outputFolderPath == NULL)
	{
		char* inputFolderPath = NULL;
		GetFolderPath(inputFolderPath, tempAllocator, filePath);
		StringPathCombine(outputFilePathStart, tempAllocator, inputFolderPath, fileName);
	}
	else
	{
		StringPathCombine(outputFilePathStart, tempAllocator, outputFolderPath, fileName);
	}

	char* newFilePath = AllocateSpaceForString(tempAllocator, UDT_MAX_PATH_LENGTH);
	sprintf(newFilePath, "%s_SPLIT_%u%s", outputFilePathStart, index + 1, udtGetFileExtensionByProtocol(protocol));

	context.LogInfo("Writing demo %s...", newFilePath);

	udtFileStream outputFile;
	if(!outputFile.Open(newFilePath, udtFileOpenMode::Write))
	{
		context.LogError("Could not open file");
		return false;
	}

	const bool success = CopyFileRange(file, outputFile, tempAllocator, startOffset, endOffset);
	if(!success)
	{
		context.LogError("File copy failed");
	}

	return success;
}

static bool CreateDemoFileSplit(udtContext& context, udtStream& file, const char* filePath, const char* outputFolderPath, const u32* fileOffsets, const u32 count)
{
	if(fileOffsets == NULL || count == 0)
	{
		return true;
	}

	// Exactly one gamestate message starting with the file.
	if(count == 1 && fileOffsets[0] == 0)
	{
		return true;
	}

	const u32 fileLength = (u32)file.Length();

	bool success = true;

	u32 start = 0;
	u32 end = 0;
	u32 indexOffset = 0;
	for(u32 i = 0; i < count; ++i)
	{
		end = fileOffsets[i];
		if(start == end)
		{
			++indexOffset;
			start = end;
			continue;
		}

		success = success && CreateDemoFileSplit(context, file, filePath, outputFolderPath, i - indexOffset, start, end);

		start = end;
	}

	end = fileLength;
	success = success && CreateDemoFileSplit(context, file, filePath, outputFolderPath, count - indexOffset, start, end);

	return success;
}

UDT_API(s32) udtSplitDemoFile(udtParserContext* context, const udtParseArg* info, const char* demoFilePath)
{
	if(context == NULL || info == NULL || demoFilePath == NULL)
	{
		return (s32)udtErrorCode::InvalidArgument;
	}

	const udtProtocol::Id protocol = udtGetProtocolByFilePath(demoFilePath);
	if(protocol == udtProtocol::Invalid)
	{
		return (s32)udtErrorCode::InvalidArgument;
	}

	context->Reset();

	if(!context->Context.SetCallbacks(info->MessageCb, info->ProgressCb, info->ProgressContext))
	{
		return (s32)udtErrorCode::OperationFailed;
	}

	udtFileStream file;
	if(!file.Open(demoFilePath, udtFileOpenMode::Read))
	{
		return (s32)udtErrorCode::OperationFailed;
	}

	if(!context->Parser.Init(&context->Context, protocol))
	{
		return (s32)udtErrorCode::OperationFailed;
	}

	context->Parser.SetFilePath(demoFilePath);

	udtVMScopedStackAllocator tempAllocScope(context->Context.TempAllocator);

	DemoSplitterAnalyzer analyzer;
	context->Parser.AddPlugIn(&analyzer);
	if(!RunParser(context->Parser, file, info->CancelOperation))
	{
		return (s32)udtErrorCode::OperationFailed;
	}

	if(analyzer.GamestateFileOffsets.GetSize() <= 1)
	{
		return (s32)udtErrorCode::None;
	}

	if(!CreateDemoFileSplit(context->Context, file, demoFilePath, info->OutputFolderPath, &analyzer.GamestateFileOffsets[0], analyzer.GamestateFileOffsets.GetSize()))
	{
		return (s32)udtErrorCode::OperationFailed;
	}

	return (s32)udtErrorCode::None;
}

UDT_API(s32) udtCutDemoFileByTime(udtParserContext* context, const udtParseArg* info, const udtCutByTimeArg* cutInfo, const char* demoFilePath)
{
	if(context == NULL || info == NULL || demoFilePath == NULL || 
	   cutInfo == NULL || cutInfo->Cuts == NULL || cutInfo->CutCount == 0)
	{
		return (s32)udtErrorCode::InvalidArgument;
	}

	const udtProtocol::Id protocol = udtGetProtocolByFilePath(demoFilePath);
	if(protocol == udtProtocol::Invalid)
	{
		return (s32)udtErrorCode::OperationFailed;
	}

	context->Reset();
	if(!context->Context.SetCallbacks(info->MessageCb, info->ProgressCb, info->ProgressContext))
	{
		return (s32)udtErrorCode::OperationFailed;
	}

	udtFileStream file;
	if(!file.Open(demoFilePath, udtFileOpenMode::Read))
	{
		return (s32)udtErrorCode::OperationFailed;
	}

	if(info->FileOffset > 0 && file.Seek((s32)info->FileOffset, udtSeekOrigin::Start) != 0)
	{
		return (s32)udtErrorCode::OperationFailed;
	}

	if(!context->Parser.Init(&context->Context, protocol, info->GameStateIndex))
	{
		return (s32)udtErrorCode::OperationFailed;
	}

	CallbackCutDemoFileStreamCreationInfo streamInfo;
	streamInfo.OutputFolderPath = info->OutputFolderPath;

	context->Parser.SetFilePath(demoFilePath);

	for(u32 i = 0; i < cutInfo->CutCount; ++i)
	{
		const udtCut& cut = cutInfo->Cuts[i];
		if(cut.StartTimeMs < cut.EndTimeMs)
		{
			context->Parser.AddCut(info->GameStateIndex, cut.StartTimeMs, cut.EndTimeMs, &CallbackCutDemoFileStreamCreation, &streamInfo);
		}
	}

	context->Context.LogInfo("Processing for a timed cut: %s", demoFilePath);

	if(!RunParser(context->Parser, file, info->CancelOperation))
	{
		return (s32)udtErrorCode::OperationFailed;
	}

	return (s32)udtErrorCode::None;
}

static bool GetCutByChatMergedSections(udtParserContext* context, udtParserPlugInCutByChat& plugIn, udtProtocol::Id protocol, const udtParseArg* info, const char* filePath)
{
	context->Reset();
	if(!context->Context.SetCallbacks(info->MessageCb, info->ProgressCb, info->ProgressContext))
	{
		return false;
	}

	udtFileStream file;
	if(!file.Open(filePath, udtFileOpenMode::Read))
	{
		return false;
	}

	if(!context->Parser.Init(&context->Context, protocol))
	{
		return false;
	}

	context->Parser.SetFilePath(filePath);
	context->Parser.AddPlugIn(&plugIn);

	context->Context.LogInfo("Processing for chat analysis: %s", filePath);

	udtVMScopedStackAllocator tempAllocScope(context->Context.TempAllocator);

	if(!RunParser(context->Parser, file, info->CancelOperation))
	{
		return false;
	}

	return true;
}

// @TODO: Move this.
bool CutByChat(udtParserContext* context, const udtParseArg* info, const udtCutByChatArg* chatInfo, const char* demoFilePath)
{
	const udtProtocol::Id protocol = udtGetProtocolByFilePath(demoFilePath);
	if(protocol == udtProtocol::Invalid)
	{
		return false;
	}

	udtParserPlugInCutByChat plugIn(*chatInfo);
	if(!GetCutByChatMergedSections(context, plugIn, protocol, info, demoFilePath))
	{
		return false;
	}

	if(plugIn.Analyzer.MergedCutSections.IsEmpty())
	{
		return true;
	}

	context->Reset();
	if(!context->Context.SetCallbacks(info->MessageCb, info->ProgressCb, info->ProgressContext))
	{
		return false;
	}

	udtFileStream file;
	if(!file.Open(demoFilePath, udtFileOpenMode::Read))
	{
		return false;
	}

	const s32 gsIndex = plugIn.Analyzer.MergedCutSections[0].GameStateIndex;
	const u32 fileOffset = context->Parser._inGameStateFileOffsets[gsIndex];
	if(fileOffset > 0 && file.Seek((s32)fileOffset, udtSeekOrigin::Start) != 0)
	{
		return false;
	}
	
	if(!context->Parser.Init(&context->Context, protocol, gsIndex))
	{
		return false;
	}

	context->Parser.SetFilePath(demoFilePath);

	CallbackCutDemoFileStreamCreationInfo cutCbInfo;
	cutCbInfo.OutputFolderPath = info->OutputFolderPath;

	const udtCutByChatAnalyzer::CutSectionVector& sections = plugIn.Analyzer.MergedCutSections;
	for(u32 i = 0, count = sections.GetSize(); i < count; ++i)
	{
		const udtCutByChatAnalyzer::CutSection& section = sections[i];
		context->Parser.AddCut(section.GameStateIndex, section.StartTimeMs, section.EndTimeMs, &CallbackCutDemoFileStreamCreation, &cutCbInfo);
	}

	context->Context.LogInfo("Processing for chat cut(s): %s", demoFilePath);

	udtVMScopedStackAllocator tempAllocScope(context->Context.TempAllocator);

	context->Context.SetCallbacks(info->MessageCb, NULL, NULL);
	const bool result = RunParser(context->Parser, file, info->CancelOperation);
	context->Context.SetCallbacks(info->MessageCb, info->ProgressCb, info->ProgressContext);

	return result;
}

UDT_API(s32) udtCutDemoFileByChat(udtParserContext* context, const udtParseArg* info, const udtCutByChatArg* chatInfo, const char* demoFilePath)
{
	if(context == NULL || info == NULL || demoFilePath == NULL ||
	   chatInfo == NULL || chatInfo->Rules == NULL || chatInfo->RuleCount == 0)
	{
		return (s32)udtErrorCode::InvalidArgument;
	}

	if(info->OutputFolderPath != NULL && !IsValidDirectory(info->OutputFolderPath))
	{
		return (s32)udtErrorCode::InvalidArgument;
	}

	if(!CutByChat(context, info, chatInfo, demoFilePath))
	{
		return (s32)udtErrorCode::OperationFailed;
	}

	return (s32)udtErrorCode::None;
}

static bool GetCutByFragSections(udtParserContext* context, udtParserPlugInCutByFrag& plugIn, udtProtocol::Id protocol, const udtParseArg* info, const char* filePath)
{
	context->Reset();
	if(!context->Context.SetCallbacks(info->MessageCb, info->ProgressCb, info->ProgressContext))
	{
		return false;
	}

	udtFileStream file;
	if(!file.Open(filePath, udtFileOpenMode::Read))
	{
		return false;
	}

	if(!context->Parser.Init(&context->Context, protocol))
	{
		return false;
	}

	context->Parser.SetFilePath(filePath);
	context->Parser.AddPlugIn(&plugIn);

	context->Context.LogInfo("Processing for frag analysis: %s", filePath);

	udtVMScopedStackAllocator tempAllocScope(context->Context.TempAllocator);

	if(!RunParser(context->Parser, file, info->CancelOperation))
	{
		return false;
	}

	return true;
}

// @TODO: Move this.
bool CutByFrag(udtParserContext* context, const udtParseArg* info, const udtCutByFragArg* fragInfo, const char* demoFilePath)
{
	const udtProtocol::Id protocol = udtGetProtocolByFilePath(demoFilePath);
	if(protocol == udtProtocol::Invalid)
	{
		return false;
	}

	udtParserPlugInCutByFrag plugIn(*fragInfo);
	if(!GetCutByFragSections(context, plugIn, protocol, info, demoFilePath))
	{
		return false;
	}

	if(plugIn.Analyzer.CutSections.IsEmpty())
	{
		return true;
	}

	context->Reset();
	if(!context->Context.SetCallbacks(info->MessageCb, info->ProgressCb, info->ProgressContext))
	{
		return false;
	}

	udtFileStream file;
	if(!file.Open(demoFilePath, udtFileOpenMode::Read))
	{
		return false;
	}

	const s32 gsIndex = plugIn.Analyzer.CutSections[0].GameStateIndex;
	const u32 fileOffset = context->Parser._inGameStateFileOffsets[gsIndex];
	if(fileOffset > 0 && file.Seek((s32)fileOffset, udtSeekOrigin::Start) != 0)
	{
		return false;
	}

	if(!context->Parser.Init(&context->Context, protocol, gsIndex))
	{
		return false;
	}

	context->Parser.SetFilePath(demoFilePath);

	CallbackCutDemoFileStreamCreationInfo cutCbInfo;
	cutCbInfo.OutputFolderPath = info->OutputFolderPath;

	const udtCutByFragAnalyzer::CutSectionVector& sections = plugIn.Analyzer.CutSections;
	for(u32 i = 0, count = sections.GetSize(); i < count; ++i)
	{
		const udtCutByFragAnalyzer::CutSection& section = sections[i];
		context->Parser.AddCut(section.GameStateIndex, section.StartTimeMs, section.EndTimeMs, &CallbackCutDemoFileStreamCreation, &cutCbInfo);
	}

	context->Context.LogInfo("Processing for frag cut(s): %s", demoFilePath);

	udtVMScopedStackAllocator tempAllocScope(context->Context.TempAllocator);

	context->Context.SetCallbacks(info->MessageCb, NULL, NULL);
	const bool result = RunParser(context->Parser, file, info->CancelOperation);
	context->Context.SetCallbacks(info->MessageCb, info->ProgressCb, info->ProgressContext);

	return result;
}

UDT_API(s32) udtCutDemoFileByFrag(udtParserContext* context, const udtParseArg* info, const udtCutByFragArg* fragInfo, const char* demoFilePath)
{
	if(context == NULL || info == NULL || demoFilePath == NULL ||
	   fragInfo == NULL || fragInfo->MinFragCount < 2 || fragInfo->TimeBetweenFragsSec == 0)
	{
		return (s32)udtErrorCode::InvalidArgument;
	}

	if(info->OutputFolderPath != NULL && !IsValidDirectory(info->OutputFolderPath))
	{
		return (s32)udtErrorCode::InvalidArgument;
	}

	if(!CutByFrag(context, info, fragInfo, demoFilePath))
	{
		return (s32)udtErrorCode::OperationFailed;
	}

	return (s32)udtErrorCode::OperationFailed;
}

UDT_API(udtParserContext*) udtCreateContext()
{
	// @NOTE: We don't use the standard operator new approach to avoid C++ exceptions.
	udtParserContext* const context = (udtParserContext*)malloc(sizeof(udtParserContext));
	if(context == NULL)
	{
		return NULL;
	}

	new (context) udtParserContext;

	return context;
}

UDT_API(s32) udtDestroyContext(udtParserContext* context)
{
	if(context == NULL)
	{
		return (s32)udtErrorCode::InvalidArgument;
	}

	// @NOTE: We don't use the standard operator new approach to avoid C++ exceptions.
	context->~udtParserContext();
	free(context);

	return (s32)udtErrorCode::None;
}

// @TODO: Move this.
bool ParseDemoFile(udtParserContext* context, const udtParseArg* info, const char* demoFilePath, bool clearPlugInData)
{
	if(clearPlugInData)
	{
		context->Reset();
	}
	else
	{
		context->ResetButKeepPlugInData();
	}
	
	context->CreateAndAddPlugIns(info->PlugIns, info->PlugInCount);

	const udtProtocol::Id protocol = udtGetProtocolByFilePath(demoFilePath);
	if(protocol == udtProtocol::Invalid)
	{
		return false;
	}

	if(!context->Context.SetCallbacks(info->MessageCb, info->ProgressCb, info->ProgressContext))
	{
		return false;
	}

	udtFileStream file;
	if(!file.Open(demoFilePath, udtFileOpenMode::Read))
	{
		return false;
	}

	if(!context->Parser.Init(&context->Context, protocol))
	{
		return false;
	}

	udtVMScopedStackAllocator tempAllocScope(context->Context.TempAllocator);

	context->Parser.SetFilePath(demoFilePath);
	if(!RunParser(context->Parser, file, info->CancelOperation))
	{
		return false;
	}

	return true;
}

UDT_API(s32) udtParseDemoFile(udtParserContext* context, const udtParseArg* info, const char* demoFilePath)
{
	if(context == NULL || info == NULL || demoFilePath == 0 ||
	   info->PlugInCount == 0 || info->PlugIns == NULL)
	{
		return (s32)udtErrorCode::InvalidArgument;
	}

	if(!ParseDemoFile(context, info, demoFilePath, true))
	{
		return udtErrorCode::OperationFailed;
	}

	return (s32)udtErrorCode::None;
}

UDT_API(s32) udtGetDemoDataInfo(udtParserContext* context, u32 demoIdx, u32 plugInId, void** buffer, u32* count)
{
	if(context == NULL || plugInId >= (u32)udtParserPlugIn::Count || buffer == NULL || count == NULL ||
	   demoIdx >= context->DemoCount)
	{
		return (s32)udtErrorCode::InvalidArgument;
	}

	if(!context->GetDataInfo(demoIdx, plugInId, buffer, count))
	{
		return udtErrorCode::OperationFailed;
	}

	return (s32)udtErrorCode::None;
}

struct udtParserContextGroup
{
	udtParserContext* Contexts;
	u32 ContextCount;
};

static bool CreateContextGroup(udtParserContextGroup** contextGroupPtr, u32 contextCount)
{
	if(contextCount == 0)
	{
		return false;
	}

	const size_t byteCount = sizeof(udtParserContextGroup) + contextCount * sizeof(udtParserContext);
	udtParserContextGroup* const contextGroup = (udtParserContextGroup*)malloc(byteCount);
	if(contextGroup == NULL)
	{
		return false;
	}

	new (contextGroup) udtParserContextGroup;

	udtParserContext* const contexts = (udtParserContext*)(contextGroup + 1);
	for(u32 i = 0; i < contextCount; ++i)
	{
		new (contexts + i) udtParserContext;
	}

	contextGroup->Contexts = contexts;
	contextGroup->ContextCount = contextCount;
	*contextGroupPtr = contextGroup;

	return true;
}

static void DestroyContextGroup(udtParserContextGroup* contextGroup)
{
	if(contextGroup == NULL)
	{
		return;
	}

	const u32 contextCount = contextGroup->ContextCount;
	for(u32 i = 0; i < contextCount; ++i)
	{
		contextGroup->Contexts[i].~udtParserContext();
	}

	contextGroup->~udtParserContextGroup();
}

UDT_API(s32) udtDestroyContextGroup(udtParserContextGroup* contextGroup)
{
	if(contextGroup == NULL)
	{
		return (s32)udtErrorCode::InvalidArgument;
	}

	DestroyContextGroup(contextGroup);

	return (s32)udtErrorCode::None;
}

static s32 udtParseDemoFiles_SingleThread(udtParserContext* context, const udtParseArg* info, const udtMultiParseArg* extraInfo)
{
	udtTimer timer;
	timer.Start();

	udtVMArray<u64> fileSizes;
	fileSizes.Resize(extraInfo->FileCount);

	u64 totalByteCount = 0;
	for(u32 i = 0; i < extraInfo->FileCount; ++i)
	{
		const u64 byteCount = udtFileStream::GetFileLength(extraInfo->FilePaths[i]);
		fileSizes[i] = byteCount;
		totalByteCount += byteCount;
	}

	context->InputIndices.Resize(extraInfo->FileCount);
	for(u32 i = 0; i < extraInfo->FileCount; ++i)
	{
		context->InputIndices[i] = i;
		extraInfo->OutputErrorCodes[i] = (s32)udtErrorCode::Unprocessed;
	}

	SingleThreadProgressContext progressContext;
	progressContext.Timer = &timer;
	progressContext.UserCallback = info->ProgressCb;
	progressContext.UserData = info->ProgressContext;
	progressContext.CurrentJobByteCount = 0;
	progressContext.ProcessedByteCount = 0;
	progressContext.TotalByteCount = totalByteCount;

	udtParseArg newInfo = *info;
	newInfo.ProgressCb = &SingleThreadProgressCallback;
	newInfo.ProgressContext = &progressContext;

	for(u32 i = 0; i < extraInfo->FileCount; ++i)
	{
		if(info->CancelOperation != NULL && *info->CancelOperation != 0)
		{
			break;
		}

		const u64 jobByteCount = fileSizes[i];
		progressContext.CurrentJobByteCount = jobByteCount;

		const bool success = ParseDemoFile(context, &newInfo, extraInfo->FilePaths[i], false);
		extraInfo->OutputErrorCodes[i] = GetErrorCode(success, info->CancelOperation);

		progressContext.ProcessedByteCount += jobByteCount;
	}

	return GetErrorCode(true, info->CancelOperation);
}

UDT_API(s32) udtParseDemoFiles(udtParserContextGroup** contextGroup, const udtParseArg* info, const udtMultiParseArg* extraInfo)
{
	if(contextGroup == NULL || info == NULL || extraInfo == NULL ||
	   extraInfo->FileCount == 0 || extraInfo->FilePaths == NULL || extraInfo->OutputErrorCodes == NULL)
	{
		return (s32)udtErrorCode::InvalidArgument;
	}

	udtDemoThreadAllocator threadAllocator;
	const bool threadJob = threadAllocator.Process(extraInfo->FilePaths, extraInfo->FileCount, extraInfo->MaxThreadCount);
	const u32 threadCount = threadJob ? threadAllocator.Threads.GetSize() : 1;
	if(!CreateContextGroup(contextGroup, threadCount))
	{
		return (s32)udtErrorCode::OperationFailed;
	}

	if(!threadJob)
	{
		return udtParseDemoFiles_SingleThread((*contextGroup)->Contexts, info, extraInfo);
	}
	
	udtMultiThreadedParsing parser;
	const bool success = parser.Process((*contextGroup)->Contexts, threadAllocator, info, extraInfo, udtParsingJobType::General, NULL);

	return GetErrorCode(success, info->CancelOperation);
}

static s32 udtCutDemoFilesByChat_SingleThread(const udtParseArg* info, const udtMultiParseArg* extraInfo, const udtCutByChatArg* chatInfo)
{
	udtParserContext* context = udtCreateContext();
	if(context == NULL)
	{
		return (s32)udtErrorCode::OperationFailed;
	}

	udtTimer timer;
	timer.Start();

	udtVMArray<u64> fileSizes;
	fileSizes.Resize(extraInfo->FileCount);

	u64 totalByteCount = 0;
	for(u32 i = 0; i < extraInfo->FileCount; ++i)
	{
		const u64 byteCount = udtFileStream::GetFileLength(extraInfo->FilePaths[i]);
		fileSizes[i] = byteCount;
		totalByteCount += byteCount;
	}

	SingleThreadProgressContext progressContext;
	progressContext.Timer = &timer;
	progressContext.UserCallback = info->ProgressCb;
	progressContext.UserData = info->ProgressContext;
	progressContext.CurrentJobByteCount = 0;
	progressContext.ProcessedByteCount = 0;
	progressContext.TotalByteCount = totalByteCount;

	udtParseArg newInfo = *info;
	newInfo.ProgressCb = &SingleThreadProgressCallback;
	newInfo.ProgressContext = &progressContext;

	context->InputIndices.Resize(extraInfo->FileCount);
	for(u32 i = 0; i < extraInfo->FileCount; ++i)
	{
		context->InputIndices[i] = i;
		extraInfo->OutputErrorCodes[i] = (s32)udtErrorCode::Unprocessed;
	}

	for(u32 i = 0; i < extraInfo->FileCount; ++i)
	{
		if(info->CancelOperation != NULL && *info->CancelOperation != 0)
		{
			break;
		}

		const u64 jobByteCount = fileSizes[i];
		progressContext.CurrentJobByteCount = jobByteCount;

		const bool success = CutByChat(context, &newInfo, chatInfo, extraInfo->FilePaths[i]);
		extraInfo->OutputErrorCodes[i] = GetErrorCode(success, info->CancelOperation);

		progressContext.ProcessedByteCount += jobByteCount;
	}

	udtDestroyContext(context);

	return GetErrorCode(true, info->CancelOperation);
}

UDT_API(s32) udtCutDemoFilesByChat(const udtParseArg* info, const udtMultiParseArg* extraInfo, const udtCutByChatArg* chatInfo)
{
	if(info == NULL || extraInfo == NULL || chatInfo == NULL ||
	   chatInfo->Rules == NULL || chatInfo->RuleCount == 0 || 
	   extraInfo->FileCount == 0 || extraInfo->FilePaths == NULL || extraInfo->OutputErrorCodes == NULL)
	{
		return (s32)udtErrorCode::InvalidArgument;
	}

	if(info->OutputFolderPath != NULL && !IsValidDirectory(info->OutputFolderPath))
	{
		return (s32)udtErrorCode::InvalidArgument;
	}

	udtDemoThreadAllocator threadAllocator;
	const bool threadJob = threadAllocator.Process(extraInfo->FilePaths, extraInfo->FileCount, extraInfo->MaxThreadCount);
	if(!threadJob)
	{
		return udtCutDemoFilesByChat_SingleThread(info, extraInfo, chatInfo);
	}

	udtParserContextGroup* contextGroup;
	if(!CreateContextGroup(&contextGroup, threadAllocator.Threads.GetSize()))
	{
		return udtCutDemoFilesByChat_SingleThread(info, extraInfo, chatInfo);
	}

	udtMultiThreadedParsing parser;
	const bool success = parser.Process(contextGroup->Contexts, threadAllocator, info, extraInfo, udtParsingJobType::CutByChat, chatInfo);

	DestroyContextGroup(contextGroup);

	return GetErrorCode(success, info->CancelOperation);
}

static s32 udtCutDemoFilesByFrag_SingleThread(const udtParseArg* info, const udtMultiParseArg* extraInfo, const udtCutByFragArg* fragInfo)
{
	udtParserContext* context = udtCreateContext();
	if(context == NULL)
	{
		return (s32)udtErrorCode::OperationFailed;
	}

	udtTimer timer;
	timer.Start();

	udtVMArray<u64> fileSizes;
	fileSizes.Resize(extraInfo->FileCount);

	u64 totalByteCount = 0;
	for(u32 i = 0; i < extraInfo->FileCount; ++i)
	{
		const u64 byteCount = udtFileStream::GetFileLength(extraInfo->FilePaths[i]);
		fileSizes[i] = byteCount;
		totalByteCount += byteCount;
	}

	SingleThreadProgressContext progressContext;
	progressContext.Timer = &timer;
	progressContext.UserCallback = info->ProgressCb;
	progressContext.UserData = info->ProgressContext;
	progressContext.CurrentJobByteCount = 0;
	progressContext.ProcessedByteCount = 0;
	progressContext.TotalByteCount = totalByteCount;

	udtParseArg newInfo = *info;
	newInfo.ProgressCb = &SingleThreadProgressCallback;
	newInfo.ProgressContext = &progressContext;

	context->InputIndices.Resize(extraInfo->FileCount);
	for(u32 i = 0; i < extraInfo->FileCount; ++i)
	{
		context->InputIndices[i] = i;
		extraInfo->OutputErrorCodes[i] = (s32)udtErrorCode::Unprocessed;
	}

	for(u32 i = 0; i < extraInfo->FileCount; ++i)
	{
		if(info->CancelOperation != NULL && *info->CancelOperation != 0)
		{
			break;
		}

		const u64 jobByteCount = fileSizes[i];
		progressContext.CurrentJobByteCount = jobByteCount;

		const bool success = CutByFrag(context, &newInfo, fragInfo, extraInfo->FilePaths[i]);
		extraInfo->OutputErrorCodes[i] = GetErrorCode(success, info->CancelOperation);

		progressContext.ProcessedByteCount += jobByteCount;
	}

	udtDestroyContext(context);

	return GetErrorCode(true, info->CancelOperation);
}

UDT_API(s32) udtCutDemoFilesByFrag(const udtParseArg* info, const udtMultiParseArg* extraInfo, const udtCutByFragArg* fragInfo)
{
	if(info == NULL || extraInfo == NULL || fragInfo == NULL ||
	   fragInfo->MinFragCount < 2 || fragInfo->TimeBetweenFragsSec == 0 ||
	   extraInfo->FileCount == 0 || extraInfo->FilePaths == NULL || extraInfo->OutputErrorCodes == NULL)
	{
		return (s32)udtErrorCode::InvalidArgument;
	}

	if(info->OutputFolderPath != NULL && !IsValidDirectory(info->OutputFolderPath))
	{
		return (s32)udtErrorCode::InvalidArgument;
	}

	udtDemoThreadAllocator threadAllocator;
	const bool threadJob = threadAllocator.Process(extraInfo->FilePaths, extraInfo->FileCount, extraInfo->MaxThreadCount);
	if(!threadJob)
	{
		return udtCutDemoFilesByFrag_SingleThread(info, extraInfo, fragInfo);
	}

	udtParserContextGroup* contextGroup;
	if(!CreateContextGroup(&contextGroup, threadAllocator.Threads.GetSize()))
	{
		return udtCutDemoFilesByFrag_SingleThread(info, extraInfo, fragInfo);
	}

	udtMultiThreadedParsing parser;
	const bool success = parser.Process(contextGroup->Contexts, threadAllocator, info, extraInfo, udtParsingJobType::CutByFrag, fragInfo);

	DestroyContextGroup(contextGroup);

	return GetErrorCode(success, info->CancelOperation);
}

UDT_API(s32) udtGetContextCountFromGroup(udtParserContextGroup* contextGroup, u32* count)
{
	if(contextGroup == NULL || count == NULL)
	{
		return (s32)udtErrorCode::InvalidArgument;
	}

	*count = contextGroup->ContextCount;

	return (s32)udtErrorCode::None;
}

UDT_API(s32) udtGetContextFromGroup(udtParserContextGroup* contextGroup, u32 contextIdx, udtParserContext** context)
{
	if(contextGroup == NULL || context == NULL || contextIdx >= contextGroup->ContextCount)
	{
		return (s32)udtErrorCode::InvalidArgument;
	}

	*context = &contextGroup->Contexts[contextIdx];

	return (s32)udtErrorCode::None;
}

UDT_API(s32) udtGetDemoCountFromGroup(udtParserContextGroup* contextGroup, u32* count)
{
	if(contextGroup == NULL || count == NULL)
	{
		return (s32)udtErrorCode::InvalidArgument;
	}

	u32 demoCount = 0;
	for(u32 ctxIdx = 0, ctxCount = contextGroup->ContextCount; ctxIdx < ctxCount; ++ctxIdx)
	{
		const udtParserContext& context = contextGroup->Contexts[ctxIdx];
		demoCount += context.GetDemoCount();
	}

	*count = demoCount;

	return (s32)udtErrorCode::None;
}

UDT_API(s32) udtGetDemoCountFromContext(udtParserContext* context, u32* count)
{
	if(context == NULL || count == NULL)
	{
		return (s32)udtErrorCode::InvalidArgument;
	}

	*count = context->GetDemoCount();

	return (s32)udtErrorCode::None;
}

UDT_API(s32) udtGetDemoInputIndex(udtParserContext* context, u32 demoIdx, u32* demoInputIdx)
{
	if(context == NULL || demoInputIdx == NULL || 
	   demoIdx >= context->GetDemoCount() || demoIdx >= context->InputIndices.GetSize())
	{
		return (s32)udtErrorCode::InvalidArgument;
	}

	*demoInputIdx = context->InputIndices[demoIdx];

	return (s32)udtErrorCode::None;
}
