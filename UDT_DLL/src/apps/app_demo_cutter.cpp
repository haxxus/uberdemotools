#include "parser.hpp"
#include "file_stream.hpp"
#include "utils.hpp"
#include "shared.hpp"
#include "file_system.hpp"
#include "parser_context.hpp"

#include <stdio.h>


static const char* ConfigFilePath = "udt_cutter.cfg";

static const char* DefaultConfig = 
"// Generated by UDT_cutter, feel free to modify :)\n"
"// ChatOperator values: Contains, StartsWith, EndsWith\n"
"\n"
"StartOffset 10\n"
"EndOffset 10\n"
"RecursiveSearch 0\n"
"UseCustomOutputFolder 0\n"
"CustomOutputFolder \"\"\n"
"\n"
"[ChatRule]\n"
"ChatOperator Contains\n"
"Pattern WAXEDDD\n"
"CaseSensitive 1\n"
"IgnoreColorCodes 0\n"
"[/ChatRule]\n"
"\n"
"[ChatRule]\n"
"ChatOperator Contains\n"
"Pattern \"fragmovie frag\"\n"
"CaseSensitive 0\n"
"IgnoreColorCodes 1\n"
"[/ChatRule]\n";


struct CutByChatConfig
{
	CutByChatConfig()
	{
		StartOffsetSec = 10;
		EndOffsetSec = 10;
		RecursiveSearch = false;
		UseCustomOutputFolder = false;
		CustomOutputFolder = NULL;
	}

	int StartOffsetSec;
	int EndOffsetSec;
	bool RecursiveSearch;
	bool UseCustomOutputFolder;
	const char* CustomOutputFolder;
	udtVMArray<udtCutByChatRule> ChatRules;
};


static void InitRule(udtCutByChatRule& rule)
{
	rule.CaseSensitive = true;
	rule.ChatOperator = (u32)udtChatOperator::Contains;
	rule.IgnoreColorCodes = true;
	rule.Pattern = "WAXEDDD";
}

static bool CreateConfig(const char* filePath)
{
	udtFileStream file;
	if(!file.Open(filePath, udtFileOpenMode::Write))
	{
		return false;
	}

	return file.Write(DefaultConfig, (u32)strlen(DefaultConfig), 1) == 1;
}

static bool EnsureConfigExists(const char* filePath)
{
	udtFileStream file;
	if(!file.Open(filePath, udtFileOpenMode::Read))
	{
		if(!CreateConfig(filePath))
		{
			return false;
		}

		if(!file.Open(filePath, udtFileOpenMode::Read))
		{
			return false;
		}
	}

	return true;
}

static bool ReadConfig(CutByChatConfig& config, udtContext& context, udtVMLinearAllocator& persistAllocator, const char* filePath)
{
	if(!EnsureConfigExists(filePath))
	{
		return false;
	}

	bool definingRule = false;
	udtCutByChatRule rule;
	InitRule(rule);
	s32 tempInt;

	udtFileStream file;
	if(!file.Open(filePath, udtFileOpenMode::Read))
	{
		return false;
	}

	char* const fileData = file.ReadAllAsString(context.TempAllocator);
	if(fileData == NULL)
	{
		return false;
	}

	file.Close();

	CommandLineTokenizer& tokenizer = context.Tokenizer;

	udtVMArray<const char*> lines;
	if(!StringSplitLines(lines, (char*)fileData))
	{
		return false;
	}

	for(u32 i = 0, count = lines.GetSize(); i < count; ++i)
	{
		const char* const line = lines[i];
		if(StringIsNullOrEmpty(line) || StringStartsWith(line, "//"))
		{
			continue;
		}

		if(StringEquals(line, "[ChatRule]"))
		{
			definingRule = true;
			InitRule(rule);
			continue;
		}

		if(StringEquals(line, "[/ChatRule]"))
		{
			definingRule = false;
			config.ChatRules.Add(rule);
			continue;
		}

		tokenizer.Tokenize(line);
		if(tokenizer.argc() != 2)
		{
			continue;
		}

		if(definingRule)
		{
			if(StringEquals(tokenizer.argv(0), "ChatOperator"))
			{
				if(StringEquals(tokenizer.argv(1), "Contains")) rule.ChatOperator = (u32)udtChatOperator::Contains;
				else if(StringEquals(tokenizer.argv(1), "StartsWith")) rule.ChatOperator = (u32)udtChatOperator::StartsWith;
				else if(StringEquals(tokenizer.argv(1), "EndsWith")) rule.ChatOperator = (u32)udtChatOperator::EndsWith;
			}
			else if(StringEquals(tokenizer.argv(0), "Pattern"))
			{
				rule.Pattern = AllocateString(persistAllocator, tokenizer.argv(1));
			}
			else if(StringEquals(tokenizer.argv(0), "CaseSensitive"))
			{
				if(StringEquals(tokenizer.argv(1), "1")) rule.CaseSensitive = true;
				else if(StringEquals(tokenizer.argv(1), "0")) rule.CaseSensitive = false;
			}
			else if(StringEquals(tokenizer.argv(0), "IgnoreColorCodes"))
			{
				if(StringEquals(tokenizer.argv(1), "1")) rule.IgnoreColorCodes = true;
				else if(StringEquals(tokenizer.argv(1), "0")) rule.IgnoreColorCodes = false;
			}
		}
		else
		{
			if(StringEquals(tokenizer.argv(0), "StartOffset"))
			{
				if(StringParseInt(tempInt, tokenizer.argv(1))) config.StartOffsetSec = tempInt;
			}
			else if(StringEquals(tokenizer.argv(0), "EndOffset"))
			{
				if(StringParseInt(tempInt, tokenizer.argv(1))) config.EndOffsetSec = tempInt;
			}
			else if(StringEquals(tokenizer.argv(0), "RecursiveSearch"))
			{
				if(StringEquals(tokenizer.argv(1), "1")) config.RecursiveSearch = true;
				else if(StringEquals(tokenizer.argv(1), "0")) config.RecursiveSearch = false;
			}
			else if(StringEquals(tokenizer.argv(0), "UseCustomOutputFolder"))
			{
				if(StringEquals(tokenizer.argv(1), "1")) config.UseCustomOutputFolder = true;
				else if(StringEquals(tokenizer.argv(1), "0")) config.UseCustomOutputFolder = false;
			}
			else if(StringEquals(tokenizer.argv(0), "CustomOutputFolder") && IsValidDirectory(tokenizer.argv(1)))
			{
				config.CustomOutputFolder = AllocateString(persistAllocator, tokenizer.argv(1));
			}
		}
	}

	return true;
}

static bool CutByTime(const char* filePath, const char* outputFolder, s32 startSec, s32 endSec)
{
	udtCutByTimeArg info;
	memset(&info, 0, sizeof(info));
	info.MessageCb = &CallbackConsoleMessage;
	info.ProgressCb = NULL;
	info.FilePath = filePath;
	info.OutputFolderPath = outputFolder;
	info.StartTimeMs = startSec * 1000;
	info.EndTimeMs = endSec * 1000;

	udtParserContext* const context = udtCreateContext();
	if(context == NULL)
	{
		return false;
	}

	const bool result = udtCutDemoByTime(context, &info) == udtErrorCode::None;
	udtDestroyContext(context);

	return result;
}

static bool CutByChat(udtParserContext* context, const char* filePath, const CutByChatConfig& config)
{
	udtCutByChatArg info;
	memset(&info, 0, sizeof(info));
	info.MessageCb = &CallbackConsoleMessage;
	info.ProgressCb = NULL;
	info.FilePath = filePath;
	info.OutputFolderPath = config.UseCustomOutputFolder ? config.CustomOutputFolder : NULL;
	info.StartOffsetSec = config.StartOffsetSec;
	info.EndOffsetSec = config.EndOffsetSec;
	info.Rules = config.ChatRules.GetStartAddress();
	info.RuleCount = config.ChatRules.GetSize();

	return udtCutDemoByChat(context, &info) == udtErrorCode::None;
}

static void PrintHelp()
{
	printf("???? help for UDT_cutter ????\n");
	printf("Syntax: UDT_cutter path [start_time end_time]\n");
	printf("UDT_cutter demo_file_path start_time end_time   <-- cut a single demo by time\n");
	printf("UDT_cutter demo_file_path   <-- cut a single demo by chat\n");
	printf("UDT_cutter demo_folder_path   <-- cut a bunch of demos by chat\n");
	printf("If the start and end times aren't specified, will do \"Cut by Chat\"\n");
	printf("Start and end times can either be written as 'seconds' or 'minutes:seconds'\n");
	printf("All the rules/variables are read from the config file udt_cutter.cfg\n");
}

bool KeepOnlyDemoFiles(const char* name, u64 /*size*/)
{
	return StringHasValidDemoFileExtension(name);
}

int main(int argc, char** argv)
{
	ResetCurrentDirectory(argv[0]);
	EnsureConfigExists(ConfigFilePath);

	udtParserContext* const context = udtCreateContext();
	if(context == NULL)
	{
		return __LINE__;
	}

	CutByChatConfig config;
	udtVMLinearAllocator configAllocator;
	configAllocator.Init(1 << 24, 4096);
	if(!ReadConfig(config, context->Context, configAllocator, ConfigFilePath))
	{
		printf("Could not load config file.\n");
		return __LINE__;
	}

	if(argc != 2 && argc != 4)
	{
		printf("Wrong argument count.\n");
		PrintHelp();
		return __LINE__;
	}

	if(argc == 2)
	{
		printf("Two arguments, enabling cut by chat.\n");

		bool fileMode = false;
		if(udtFileStream::Exists(argv[1]) && StringHasValidDemoFileExtension(argv[1]))
		{
			fileMode = true;
		}
		else if(IsValidDirectory(argv[1]))
		{
		}
		else
		{
			printf("Invalid file/folder path.\n");
			PrintHelp();
			return __LINE__;
		}

		if(fileMode)
		{
			printf("\n");
			return CutByChat(context, argv[1], config) ? 0 : 666;
		}

		udtVMArray<udtFileInfo> files;
		udtVMLinearAllocator persistAlloc;
		udtVMLinearAllocator tempAlloc;
		persistAlloc.Init(1 << 24, 4096);
		tempAlloc.Init(1 << 24, 4096);

		udtFileListQuery query;
		memset(&query, 0, sizeof(query));
		query.FileFilter = &KeepOnlyDemoFiles;
		query.Files = &files;
		query.FolderPath = argv[1];
		query.PersistAllocator = &persistAlloc;
		query.Recursive = false;
		query.TempAllocator = &tempAlloc;
		GetDirectoryFileList(query);

		int errorCode = -666;
		for(u32 i = 0; i < files.GetSize(); ++i)
		{
			printf("\n");
			errorCode -= CutByChat(context, files[i].Path, config) ? 1 : 0;
		}

		return errorCode;
	}

	printf("Four arguments, enabling cut by time.\n");

	if(!udtFileStream::Exists(argv[1]) || !StringHasValidDemoFileExtension(argv[1]))
	{
		printf("Invalid file path.\n");
		PrintHelp();
		return __LINE__;
	}

	s32 startSec = -1;
	if(!StringParseSeconds(startSec, argv[2]))
	{
		printf("Invalid start time-stamp.\n");
		PrintHelp();
		return __LINE__;
	}

	s32 endSec = -1;
	if(!StringParseSeconds(endSec, argv[3]))
	{
		printf("Invalid end time-stamp.\n");
		PrintHelp();
		return __LINE__;
	}

	printf("\n");

	return CutByTime(argv[1], config.UseCustomOutputFolder ? config.CustomOutputFolder : NULL, startSec, endSec) ? 0 : 666;
}
