#include "parser.hpp"
#include "utils.hpp"
#include "scoped_stack_allocator.hpp"
#include "path.hpp"


udtBaseParser::udtBaseParser() 
{
	_context = NULL;
	_inProtocol = udtProtocol::Invalid;
	_outProtocol = udtProtocol::Invalid;
	_protocolConverter = NULL;

	UserData = NULL;
	EnablePlugIns = true;

	_inFileName = udtString::NewEmptyConstant();
	_inFilePath = udtString::NewEmptyConstant();
	_inFileOffset = 0;
	_inServerMessageSequence = -1;
	_inServerCommandSequence = -1;
	_inReliableSequenceAcknowledge = -1;
	_inClientNum = -1;
	_inChecksumFeed = -1;
	_inParseEntitiesNum = 0;
	_inGameStateIndex = -1;
	_inServerTime = S32_MIN;
	_inLastSnapshotMessageNumber = S32_MIN;

	_outFileName = udtString::NewEmptyConstant();
	_outFilePath = udtString::NewEmptyConstant();
	_outServerCommandSequence = 0;
	_outSnapshotsWritten = 0;
	_outWriteFirstMessage = false;
	_outWriteMessage = false;
}

udtBaseParser::~udtBaseParser()
{
	Destroy();
}

void udtBaseParser::InitAllocators()
{
	_persistentAllocator.Init(1 << 18, "Parser::Persistent");
	_configStringAllocator.Init(1 << 20, "Parser::ConfigStrings");
	_tempAllocator.Init(1 << 18, "Parser::Temp");
	_privateTempAllocator.Init(1 << 16, "Parser::PrivateTemp");
	PlugIns.Init(1 << 16, "Parser::PlugInsArray");
	_inGameStateFileOffsets.Init(1 << 16, "Parser::GameStateFileOffsetsArray");
	_inChangedEntities.Init(1 << 16, "Parser::ChangedEntitiesArray");
	_inRemovedEntities.Init(1 << 16, "Parser::RemovedEntitiesArray");
	_cuts.Init(1 << 16, "Parser::CutsArray");
}

bool udtBaseParser::Init(udtContext* context, udtProtocol::Id inProtocol, udtProtocol::Id outProtocol, s32 gameStateIndex, bool enablePlugIns)
{
	if(context == NULL || !udtIsValidProtocol(inProtocol) || gameStateIndex < 0)
	{
		return false;
	}

	EnablePlugIns = enablePlugIns;

	_context = context;
	_inProtocol = inProtocol;
	_outProtocol = outProtocol;
	_inProtocolSizeOfEntityState = (s32)udtGetSizeOfIdEntityState(inProtocol);
	_inProtocolSizeOfClientSnapshot = (s32)udtGetSizeOfidClientSnapshot(inProtocol);
	_protocolConverter = context->GetProtocolConverter(outProtocol, inProtocol);
	_protocolConverter->ResetForNextDemo();

	_outMsg.InitContext(context);
	_outMsg.InitProtocol(outProtocol);

	_inFileName = udtString::NewEmptyConstant();
	_inFilePath = udtString::NewEmptyConstant();
	_outFileName = udtString::NewEmptyConstant();
	_outFilePath = udtString::NewEmptyConstant();

	_cuts.Clear();
	_persistentAllocator.Clear();
	_configStringAllocator.Clear();
	_tempAllocator.Clear();
	_privateTempAllocator.Clear();

	_inGameStateIndex = gameStateIndex - 1;
	_inGameStateFileOffsets.Clear();
	if(gameStateIndex > 0)
	{
		_inGameStateFileOffsets.Resize(gameStateIndex);
		for(s32 i = 0; i < gameStateIndex; ++i)
		{
			_inGameStateFileOffsets[0] = 0;
		}
	}

	if(enablePlugIns)
	{
		for(u32 i = 0; i < PlugIns.GetSize(); ++i)
		{
			PlugIns[i]->StartProcessingDemo();
		}
	}

	return true;
}

void udtBaseParser::ResetForGamestateMessage()
{
	_inServerMessageSequence = -1;
	_inServerCommandSequence = -1;
	_inReliableSequenceAcknowledge = -1;
	_inClientNum = -1;
	_inChecksumFeed = -1;
	_inParseEntitiesNum = 0;
	_inServerTime = S32_MIN;
	_inLastSnapshotMessageNumber = S32_MIN;

	_outServerCommandSequence = 0;
	_outSnapshotsWritten = 0;
	_outWriteFirstMessage = false;
	_outWriteMessage = false;

	memset(_inEntityBaselines, 0, sizeof(_inEntityBaselines));
	memset(_inSnapshots, 0, sizeof(_inSnapshots));
	memset(_inConfigStrings, 0, sizeof(_inConfigStrings));
	for(u32 i = 0; i < (u32)UDT_COUNT_OF(_inEntityEventTimesMs); ++i)
	{
		_inEntityEventTimesMs[i] = S32_MIN;
	}

	_configStringAllocator.Clear();
	_tempAllocator.Clear();
	_privateTempAllocator.Clear();
}

void udtBaseParser::SetFilePath(const char* filePath)
{
	_inFilePath = udtString::NewClone(_persistentAllocator, filePath);
	udtPath::GetFileName(_inFileName, _persistentAllocator, _inFilePath);
}

void udtBaseParser::Destroy()
{
}

bool udtBaseParser::ParseNextMessage(const udtMessage& inMsg, s32 inServerMessageSequence, u32 fileOffset)
{
	_inMsg = inMsg;
	_inMsg.SetFileNamePtr(_inFileName.String);
	_inServerMessageSequence = inServerMessageSequence;
	_inFileOffset = fileOffset;

	return ParseServerMessage();
}

bool udtBaseParser::ParseServerMessage()
{
	_outMsg.Init(_outMsgData, sizeof(_outMsgData));

	_outMsg.SetHuffman(_outProtocol >= udtProtocol::Dm66);
	_inMsg.SetHuffman(_inProtocol >= udtProtocol::Dm66);

	//
	// Using the message sequence number as acknowledge number will help avoid 
	// the command overflow playback error for dm3 and dm_48 demos converted to dm_68.
	// For reference in the Q3 source code: "Client command overflow".
	//
	s32 reliableSequenceAcknowledge = _inServerMessageSequence;
	if(_inProtocol > udtProtocol::Dm3)
	{
		const s32 sequAck = _inMsg.ReadLong(); // Reliable sequence acknowledge.
		if(_inProtocol >= udtProtocol::Dm68)
		{
			reliableSequenceAcknowledge = sequAck;
		}
	}
	_inReliableSequenceAcknowledge = reliableSequenceAcknowledge;
	_outMsg.WriteLong(_inReliableSequenceAcknowledge);

	for(;;)
	{
		if(_inMsg.Buffer.readcount > _inMsg.Buffer.cursize) 
		{
			_context->LogError("ParseServerMessage: read past the end of the server message (in file: %s)", GetFileName());
			return false;
		}

		if(_inMsg.Buffer.readcount == _inMsg.Buffer.cursize)
		{
			break;
		}

		s32 command = _inMsg.ReadByte();
		
		if(_inProtocol >= udtProtocol::Dm90)
		{
			// See if this is an extension command after the EOF,
			// which means we got data that a legacy client should ignore.
			if((command == svc_EOF) && (_inMsg.PeekByte() == svc_extension))
			{
				printf("!!!! Command byte: EOF with following svc_extension\n");
				_inMsg.ReadByte(); // Throw the svc_extension byte away.
				command = _inMsg.ReadByte();
				// Sometimes you get a svc_extension at end of stream...
				// dangling bits in the Huffman decoder giving a bogus value?
				if(command == -1)
				{
					command = svc_EOF;
				}
			}
		}

		if(command == svc_EOF || (_inProtocol <= udtProtocol::Dm48 && command == svc_bad))
		{
			break;
		}

		// @NOTE: Don't write the command byte already, we leave that decision for later.

		switch(command) 
		{
		case svc_nop:
			_outMsg.WriteByte(svc_nop);
			break;

		case svc_serverCommand:
			if(!ParseCommandString()) return false;
			break;

		case svc_gamestate:
			if(!ParseGamestate()) return false;
			break;

		case svc_snapshot:
			if(!ParseSnapshot()) return false;
			break;

		case svc_voip:
			_context->LogWarning("@TODO: Command byte: svc_voip");
			_outMsg.WriteByte(svc_nop);
			break;

		case svc_download:
			_context->LogWarning("@TODO: Command byte: svc_download");
			_outMsg.WriteByte(svc_nop);
			break;

		default:
			_context->LogError("ParseServerMessage: unrecognized server message command byte: %d (in file: %s)", command, GetFileName());
			return false;
		}

		if(_inProtocol <= udtProtocol::Dm48)
		{
			_inMsg.GoToNextByte();
		}
	}

	_outMsg.WriteByte(svc_EOF);

	if(_cuts.GetSize() > 0)
	{
		const udtCutInfo cut = _cuts[0];
		const s32 gameTime = _inServerTime;

		if(_inGameStateIndex == cut.GameStateIndex && !_outWriteMessage &&
		   gameTime >= cut.StartTimeMs && gameTime <= cut.EndTimeMs)
		{
			const bool wroteMessage = _outWriteMessage;
			_outWriteMessage = true;
			_outWriteFirstMessage = _outWriteMessage && !wroteMessage;
		}
		else if((_inGameStateIndex == cut.GameStateIndex && _outWriteMessage && gameTime > cut.EndTimeMs) ||
				(_inGameStateIndex > cut.GameStateIndex && _outWriteMessage))
		{
			WriteLastMessage();
			_outWriteMessage = false;
			_outWriteFirstMessage = false;
			_outServerCommandSequence = 0;
			_outSnapshotsWritten = 0;
			_cuts[0].Stream->~udtStream();
			_cuts.Remove(0);
			if(_cuts.GetSize() == 0)
			{
				// It was the last cut, we're done parsing the file now.
				return false;
			}
		}
	}

	if(_outWriteFirstMessage)
	{
		udtCutInfo& cut = _cuts[0];
		udtDemoStreamCreatorArg info;
		memset(&info, 0, sizeof(info));
		info.StartTimeMs = cut.StartTimeMs;
		info.EndTimeMs = cut.EndTimeMs;
		info.Parser = this;
		info.VeryShortDesc = cut.VeryShortDesc;
		info.UserData = cut.UserData;
		info.TempAllocator = &_tempAllocator;
		info.FilePathAllocator = &_persistentAllocator;
		udtString filePath;
		cut.Stream = (*cut.StreamCreator)(filePath, info);
		if(cut.Stream != NULL)
		{
			_outFilePath = filePath;
			udtPath::GetFileName(_outFileName, _persistentAllocator, filePath);
			_outMsg.SetFileNamePtr(_outFileName.String);
			WriteFirstMessage();
			_outWriteFirstMessage = false;
		}
		else
		{
			_cuts.Remove(0);
		}
	}
	else if(_outWriteMessage)
	{
		WriteNextMessage();
	}

	return true;
}

void udtBaseParser::FinishParsing(bool /*success*/)
{
	// Close any output file stream that is still open, if any.
	if(!_cuts.IsEmpty() && _outWriteMessage)
	{
		WriteLastMessage();
		_outWriteMessage = false;
		_outWriteFirstMessage = false;
		_outServerCommandSequence = 0;
		_outSnapshotsWritten = 0;
		_cuts[0].Stream->~udtStream();
		_cuts.Clear();
	}

	if(EnablePlugIns)
	{
		for(u32 i = 0, count = PlugIns.GetSize(); i < count; ++i)
		{
			PlugIns[i]->FinishProcessingDemo();
		}
	}
}

void udtBaseParser::AddCut(s32 gsIndex, s32 startTimeMs, s32 endTimeMs, udtDemoStreamCreator streamCreator, const char* veryShortDesc, void* userData)
{
	udtCutInfo cut;
	cut.VeryShortDesc = veryShortDesc;
	cut.GameStateIndex = gsIndex;
	cut.StartTimeMs = startTimeMs;
	cut.EndTimeMs = endTimeMs;
	cut.Stream = NULL;
	cut.StreamCreator = streamCreator;
	cut.UserData = userData;
	_cuts.Add(cut);
}

bool udtBaseParser::ShouldWriteMessage() const
{
	return _outWriteMessage && _outProtocol >= udtProtocol::Dm66;
}

void udtBaseParser::WriteFirstMessage()
{
	WriteGameState();
	const s32 length = _outMsg.Buffer.cursize;
	udtStream* const stream = _cuts[0].Stream;
	stream->Write(&_inServerMessageSequence, 4, 1);
	stream->Write(&length, 4, 1);
	stream->Write(_outMsg.Buffer.data, length, 1);
}

void udtBaseParser::WriteNextMessage()
{
	const s32 length = _outMsg.Buffer.cursize;
	udtStream* const stream = _cuts[0].Stream;
	stream->Write(&_inServerMessageSequence, 4, 1);
	stream->Write(&length, 4, 1);
	stream->Write(_outMsg.Buffer.data, length, 1);
}

void udtBaseParser::WriteLastMessage()
{
	udtStream* const stream = _cuts[0].Stream;

	s32 length = -1;
	stream->Write(&length, 4, 1);
	stream->Write(&length, 4, 1);
	stream->Close();

	//_context->LogInfo("Stopped writing the demo.");
}

bool udtBaseParser::ParseCommandString()
{
	s32 commandStringLength = 0;
	const s32 commandSequence = _inMsg.ReadLong();
	const char* const commandStringTemp = _inMsg.ReadString(commandStringLength);

	// Do we have it already?
	if(_inServerCommandSequence >= commandSequence) 
	{
		// Yes, don't bother processing it.
		return true;
	}
	
	// Copy the string to some temporary location.
	udtVMScopedStackAllocator scopedTempAllocator(_tempAllocator);
	char* commandString = AllocateString(_tempAllocator, commandStringTemp, (u32)commandStringLength);
	
	// We haven't, so let's store the last sequence number received.
	_inServerCommandSequence = commandSequence;

	bool plugInSkipsThisCommand = false;

tokenize:
	idTokenizer& tokenizer = _tokenizer;
	tokenizer.Tokenize(commandString);
	const int tokenCount = tokenizer.GetArgCount();
	const udtString commandName = (tokenCount > 0) ? tokenizer.GetArg(0) : udtString::NewEmptyConstant();
	s32 csIndex = -1;
	bool isConfigString = false;
	if(tokenCount == 3 && udtString::Equals(commandName, "cs"))
	{
		if(StringParseInt(csIndex, tokenizer.GetArgString(1)) && csIndex >= 0 && csIndex < (s32)UDT_COUNT_OF(_inConfigStrings))
		{
			isConfigString = true;

			const char* const csStringTemp = tokenizer.GetArgString(2);
			u32 csStringLength = (u32)strlen(csStringTemp);

			udtConfigStringConversion outCs;
			_protocolConverter->ConvertConfigString(outCs, _tempAllocator, csIndex, csStringTemp, csStringLength);
			if(outCs.NewString || outCs.Index != csIndex)
			{
				commandString = (char*)_privateTempAllocator.Allocate(2 * BIG_INFO_STRING);
				sprintf(commandString, "cs %d \"%s\"", outCs.Index, outCs.String);
				commandStringLength = (s32)strlen(commandString);
				csStringLength = outCs.StringLength;
			}

			// Copy the config string to some safe location.
			_inConfigStrings[csIndex] = udtString::NewClone(_configStringAllocator, csStringTemp, csStringLength);
		}
	}
	else if(tokenCount == 3 && udtString::Equals(commandName, "bcs0"))
	{
		// Start a new big config string.
		sprintf(_inBigConfigString, "cs %s \"%s", tokenizer.GetArgString(1), tokenizer.GetArgString(2));
		plugInSkipsThisCommand = true;
	}
	else if(tokenCount == 3 && udtString::Equals(commandName, "bcs1"))
	{
		// Append to current big config string.
		strcat(_inBigConfigString, tokenizer.GetArgString(2));
		plugInSkipsThisCommand = true;
	}
	else if(tokenCount == 3 && udtString::Equals(commandName, "bcs2"))
	{
		// Append to current big config string and finalize it.
		strcat(_inBigConfigString, tokenizer.GetArgString(2));
		strcat(_inBigConfigString, "\"");
		commandString = _inBigConfigString;
		commandStringLength = (s32)strlen(_inBigConfigString);
		goto tokenize;
	}

	if(EnablePlugIns && !PlugIns.IsEmpty() && !plugInSkipsThisCommand)
	{
		udtCommandCallbackArg info;
		info.CommandSequence = commandSequence;
		info.String = commandString;
		info.StringLength = commandStringLength;
		info.ConfigStringIndex = csIndex;
		info.IsConfigString = isConfigString;

		for(u32 i = 0, count = PlugIns.GetSize(); i < count; ++i)
		{
			PlugIns[i]->ProcessCommandMessage(info, *this);
		}
	}

	if(ShouldWriteMessage())
	{
		if(csIndex >= 0 && commandStringLength >= MAX_STRING_CHARS)
		{
			WriteBigConfigStringCommand(tokenizer.GetArg(1), tokenizer.GetArg(2));
		}
		else if(commandStringLength < MAX_STRING_CHARS)
		{
			_outMsg.WriteByte(svc_serverCommand);
			_outMsg.WriteLong(_outServerCommandSequence);
			_outMsg.WriteString(commandString, commandStringLength);
			++_outServerCommandSequence;
		}
		else
		{
			_outMsg.WriteByte(svc_nop);
		}
	}
	
	_privateTempAllocator.Clear();

	return true;
}

bool udtBaseParser::ParseGamestate()
{
	// @TODO: Reset some data, but not for the 1st gamestate message.
	ResetForGamestateMessage();

	// A game state always marks a server command sequence.
	_inServerCommandSequence = _inMsg.ReadLong();

	//
	// Parse all the config strings and baselines.
	//

	for(;;)
	{
		const s32 command = _inMsg.ReadByte();

		if(_inProtocol <= udtProtocol::Dm48 && command == svc_bad)
		{
			break;
		}

		if(command == svc_EOF)
		{
			break;
		}

		if(command == svc_configstring)
		{
			const s32 index = _inMsg.ReadShort();
			if(index < 0 || index >= MAX_CONFIGSTRINGS) 
			{
				_context->LogError("ParseGamestate: config string index out of range: %d (in file: %s)", index, GetFileName());
				return false;
			}

			s32 configStringLength = 0;
			const char* const configStringTemp = _inMsg.ReadBigString(configStringLength);
			
			// Copy the string to a safe location.
			_inConfigStrings[index] = udtString::NewClone(_configStringAllocator, configStringTemp, configStringLength);
		} 
		else if(command == svc_baseline)
		{
			const s32 newIndex = _inMsg.ReadBits(GENTITYNUM_BITS);
			if(newIndex < 0 || newIndex >= MAX_GENTITIES) 
			{
				_context->LogError("ParseGamestate: baseline number out of range: %d (in file: %s)", newIndex, GetFileName());
				return false;
			}
			
			idLargestEntityState nullState;
			idEntityStateBase* const newState = GetBaseline(newIndex);

			// We delta from the null state because we read a full entity.
			Com_Memset(&nullState, 0, sizeof(nullState));
			bool addedOrChanged = false;
			if(!_inMsg.ReadDeltaEntity(addedOrChanged, &nullState, newState, newIndex))
			{
				return false;
			}
		} 
		else 
		{
			_context->LogError("ParseGamestate: Unrecognized command byte: %d (in file: %s)", command, GetFileName());
			return false;
		}
	}

	if(_inProtocol >= udtProtocol::Dm66)
	{
		_inClientNum = _inMsg.ReadLong();
		_inChecksumFeed = _inMsg.ReadLong();
	}
	else
	{
		_inClientNum = -1;
		_inChecksumFeed = 0;
	}

	if(EnablePlugIns && !PlugIns.IsEmpty())
	{
		udtGamestateCallbackArg info;
		info.ServerCommandSequence = _inServerCommandSequence;
		info.ClientNum = _inClientNum;
		info.ChecksumFeed = _inChecksumFeed;

		for(u32 i = 0, count = PlugIns.GetSize(); i < count; ++i)
		{
			PlugIns[i]->ProcessGamestateMessage(info, *this);
		}
	}

	++_inGameStateIndex;
	_inGameStateFileOffsets.Add(_inFileOffset);

	return true;
}

bool udtBaseParser::ParseSnapshot()
{
	//
	// Read in the new snapshot to a temporary buffer
	// We will only save it if it is valid.
	// We will have read any new server commands in this
	// message before we got to svc_snapshot.
	//

	if(_inProtocol == udtProtocol::Dm3)
	{
		_inMsg.ReadLong(); // Client command sequence.
	}

	_inServerTime = _inMsg.ReadLong();

	idLargestClientSnapshot newSnap;
	Com_Memset(&newSnap, 0, sizeof(newSnap));
	newSnap.serverCommandNum = _inServerCommandSequence;
	newSnap.serverTime = _inServerTime;
	newSnap.messageNum = _inServerMessageSequence;

	s32 deltaNum = _inMsg.ReadByte();
	if(!deltaNum) 
	{
		newSnap.deltaNum = -1;
	} 
	else 
	{
		newSnap.deltaNum = newSnap.messageNum - deltaNum;
	}

	newSnap.snapFlags = _inMsg.ReadByte();

	//
	// If the frame is delta compressed from data that we
	// no longer have available, we must suck up the rest of
	// the frame, but not use it, then ask for a non-compressed
	// message.
	//

	idClientSnapshotBase* oldSnap;
	if(newSnap.deltaNum <= 0) 
	{
		newSnap.valid = true; // Uncompressed frame.
		oldSnap = NULL;
	} 
	else 
	{
		if(deltaNum >= PACKET_BACKUP)
		{
			_context->LogWarning("udtBaseParser::ParseSnapshot: deltaNum %d invalid.", deltaNum);
		}

		if(newSnap.deltaNum > _inServerMessageSequence)
		{
			_context->LogWarning("udtBaseParser::ParseSnapshot: Need delta from read ahead.");
		}

		oldSnap = GetClientSnapshot(newSnap.deltaNum & PACKET_MASK);
		if(!oldSnap->valid) 
		{
			// Should never happen.
			_context->LogWarning("udtBaseParser::ParseSnapshot: Delta from invalid frame %d (not supposed to happen!).", deltaNum);
		} 
		else if(oldSnap->messageNum != newSnap.deltaNum) 
		{
			// The frame that the server did the delta from
			// is too old, so we can't reconstruct it properly.
			_context->LogWarning("udtBaseParser::ParseSnapshot: Delta frame %d too old.", deltaNum);
		} 
		else if(_inParseEntitiesNum - oldSnap->parseEntitiesNum > MAX_PARSE_ENTITIES - 128) 
		{
			_context->LogWarning("udtBaseParser::ParseSnapshot: Delta parseEntitiesNum %d too old.", _inParseEntitiesNum);
		} 
		else 
		{
			newSnap.valid = true;
		}
	}

	//
	// Read the area mask.
	//

	const s32 areaMaskLength = _inMsg.ReadByte();
	if(areaMaskLength > (s32)sizeof(newSnap.areamask))
	{
		_context->LogError("ParseSnapshot: Invalid size %d for areamask (in file: %s)", areaMaskLength, GetFileName());
		return false;
	}
	_inMsg.ReadData(&newSnap.areamask, areaMaskLength);
	
	// Read the player info.
	if(!_inMsg.ReadDeltaPlayer(oldSnap ? GetPlayerState(oldSnap, _inProtocol) : NULL, GetPlayerState(&newSnap, _inProtocol)))
	{
		return false;
	}

	// Read in all entities.
	if(!ParsePacketEntities(_inMsg, oldSnap, &newSnap))
	{
		return false;
	}

	// Did we write enough snapshots already?
	const bool noDelta = _outSnapshotsWritten < deltaNum;
	if(noDelta) 
	{
		deltaNum = 0;
		oldSnap = NULL;
	}

	// If not valid, dump the entire thing now that 
	// it has been properly read.
	if(!newSnap.valid)
	{
		return true;
	}

	//
	// Clear the valid flags of any snapshots between the last
	// received and this one, so if there was a dropped packet
	// it won't look like something valid to delta from next
	// time we wrap around in the buffer.
	//

	s32 oldMessageNum = _inSnapshot.messageNum + 1;
	if(newSnap.messageNum - oldMessageNum >= PACKET_BACKUP)
	{
		oldMessageNum = newSnap.messageNum - (PACKET_BACKUP - 1);
	}

	for(; oldMessageNum < newSnap.messageNum; ++oldMessageNum)
	{
		GetClientSnapshot(oldMessageNum & PACKET_MASK)->valid = false;
	}

	// Copy to the current good spot.
	_inSnapshot = newSnap;

	// Save the frame off in the backup array for later delta comparisons.
	Com_Memcpy(GetClientSnapshot(_inSnapshot.messageNum & PACKET_MASK), &_inSnapshot, (size_t)_inProtocolSizeOfClientSnapshot);

	// Don't give the same stuff to the plug-ins more than once.
	if(newSnap.messageNum == _inLastSnapshotMessageNumber)
	{
		return true;
	}
	_inLastSnapshotMessageNumber = newSnap.messageNum;

	//
	// Process plug-ins now so that modifiers can alter the snapshots.
	//

	if(EnablePlugIns && !PlugIns.IsEmpty())
	{
		udtSnapshotCallbackArg info;
		info.ServerTime = _inServerTime;
		info.SnapshotArrayIndex = _inSnapshot.messageNum & PACKET_MASK;
		info.Snapshot = &newSnap;
		info.OldSnapshot = oldSnap;
		info.Entities = _inChangedEntities.GetStartAddress();
		info.EntityCount = _inChangedEntities.GetSize();
		info.RemovedEntities = _inRemovedEntities.GetStartAddress();
		info.RemovedEntityCount = _inRemovedEntities.GetSize();

		for(u32 i = 0, count = PlugIns.GetSize(); i < count; ++i)
		{
			PlugIns[i]->ProcessSnapshotMessage(info, *this);
		}
	}

	//
	// Write to the output message.
	//

	if(ShouldWriteMessage())
	{
		_outMsg.WriteByte(svc_snapshot);
		_outMsg.WriteLong(newSnap.serverTime);
		_outMsg.WriteByte(deltaNum);
		_outMsg.WriteByte(newSnap.snapFlags);
		_outMsg.WriteByte(areaMaskLength);
		_outMsg.WriteData(&newSnap.areamask, areaMaskLength);
		_protocolConverter->StartSnapshot(newSnap.serverTime);
		if(_outProtocol == _inProtocol)
		{
			_outMsg.WriteDeltaPlayer(oldSnap ? GetPlayerState(oldSnap, _outProtocol) : NULL, GetPlayerState(&newSnap, _outProtocol));
			EmitPacketEntities(deltaNum ? oldSnap : NULL, &newSnap);
		}
		else
		{
			idLargestClientSnapshot oldSnapOutProto;
			idLargestClientSnapshot newSnapOutProto;
			if(oldSnap)
			{
				_protocolConverter->ConvertSnapshot(oldSnapOutProto, *oldSnap);
			}
			_protocolConverter->ConvertSnapshot(newSnapOutProto, newSnap);
			_outMsg.WriteDeltaPlayer(oldSnap ? GetPlayerState(&oldSnapOutProto, _outProtocol) : NULL, GetPlayerState(&newSnapOutProto, _outProtocol));
			EmitPacketEntities(deltaNum ? &oldSnapOutProto : NULL, &newSnapOutProto);
		}
		++_outSnapshotsWritten;
	}

	return true;
}

void udtBaseParser::WriteGameState()
{
	_outMsg.Init(_outMsgData, sizeof(_outMsgData));
	_outMsg.Bitstream();

	_outMsg.WriteLong(_inReliableSequenceAcknowledge);

	_outMsg.WriteByte(svc_gamestate);
	_outMsg.WriteLong(_outServerCommandSequence);
	++_outServerCommandSequence;

	_protocolConverter->StartGameState();
	
	// Config strings.
	for(u32 i = 0; i < (u32)UDT_COUNT_OF(_inConfigStrings); ++i)
	{
		const udtString& cs = _inConfigStrings[i];
		if(_outProtocol == _inProtocol && !udtString::IsNullOrEmpty(cs))
		{
			_outMsg.WriteByte(svc_configstring);
			_outMsg.WriteShort((s32)i);
			_outMsg.WriteBigString(cs.String, cs.Length);
			continue;
		}

		udtVMScopedStackAllocator allocatorScope(_tempAllocator);
		
		udtConfigStringConversion outCs;
		_protocolConverter->ConvertConfigString(outCs, _tempAllocator, (s32)i, cs.String, cs.Length);
		if(outCs.Index >= 0 && outCs.StringLength > 0)
		{
			_outMsg.WriteByte(svc_configstring);
			_outMsg.WriteShort(outCs.Index);
			_outMsg.WriteBigString(outCs.String, outCs.StringLength);
		}
	}

	idLargestEntityState nullState;
	Com_Memset(&nullState, 0, sizeof(nullState));
	
	// Baseline entities.
	for(s32 i = 0; i < MAX_PARSE_ENTITIES; ++i)
	{
		// We delta from the null state because we write a full entity.
		const idEntityStateBase* const newState = GetBaseline(i);

		// Write the baseline entity if it's not filled with 0 integers.
		if(memcmp(&nullState, newState, _inProtocolSizeOfEntityState))
		{
			_outMsg.WriteByte(svc_baseline);

			// @NOTE: MSG_WriteBits is called in there with newState.number as an argument.
			idLargestEntityState newStateOutProto;
			_protocolConverter->ConvertEntityState(newStateOutProto, *newState);
			_outMsg.WriteDeltaEntity(&nullState, &newStateOutProto, true);
		}
	}
	
	_outMsg.WriteByte(svc_EOF);

	_outMsg.WriteLong(_inClientNum);
	_outMsg.WriteLong(_inChecksumFeed);

	_outMsg.WriteByte(svc_EOF);
}

void udtBaseParser::WriteBigConfigStringCommand(const udtString& csIndex, const udtString& csData)
{
	// Simple example:
	// cs idx "name0\value0\name1\value1\name2\value2\name3\value3"
	// becomes:
	// bcs0 idx "name0\value0\"
	// bcs1 idx "name1\value1\"
	// bcs1 idx "name2\value2\"
	// bcs2 idx "name3\value3"
	const u32 maxLengthPerCmd = MAX_STRING_CHARS - 2;
	const u32 perCmdOverhead = 8 + csIndex.Length;
	const u32 maxDataLength = maxLengthPerCmd - perCmdOverhead;
	u32 outputChunks = 0;
	for(u32 i = 2;; ++i)
	{
		const u32 perCmdData = (csData.Length + i - 1) / i;
		if(perCmdData + perCmdOverhead <= maxLengthPerCmd)
		{
			outputChunks = i;
			break;
		}
	}

	u32 dataOffset = 0;
	for(u32 i = 0; i < outputChunks; ++i)
	{
		const char* bcsIdxRaw = i == 0 ? "0" : (i == outputChunks - 1 ? "2" : "1");
		const udtString data = udtString::NewSubstringRef(csData, dataOffset, bcsIdxRaw[0] == '2' ? (u32)~0 : maxDataLength);
		const udtString bcs = udtString::NewConstRef("bcs");
		const udtString bcsIdx = udtString::NewConstRef(bcsIdxRaw);
		const udtString space = udtString::NewConstRef(" ");
		const udtString quote = udtString::NewConstRef("\"");
		udtString command = udtString::NewEmpty(_tempAllocator, MAX_STRING_CHARS);
		const udtString* cmdPieces[] =
		{
			&bcs,
			&bcsIdx,
			&space,
			&csIndex,
			&space,
			&quote,
			&data,
			&quote
		};
		udtString::AppendMultiple(command, cmdPieces, (u32)UDT_COUNT_OF(cmdPieces));

		_outMsg.WriteByte(svc_serverCommand);
		_outMsg.WriteLong(_outServerCommandSequence);
		_outMsg.WriteString(command.String, (s32)command.Length);

		++_outServerCommandSequence;
		dataOffset += maxDataLength;
	}

}

bool udtBaseParser::ParsePacketEntities(udtMessage& msg, idClientSnapshotBase* oldframe, idClientSnapshotBase* newframe)
{
	_inChangedEntities.Clear();
	_inRemovedEntities.Clear();

	newframe->parseEntitiesNum = _inParseEntitiesNum;
	newframe->numEntities = 0;

	// delta from the entities present in oldframe
	s32	oldnum;
	s32	newnum;
	s32 oldindex = 0;
	idEntityStateBase* oldstate = NULL;
	if(!oldframe) 
	{
		oldnum = 99999;
	} 
	else 
	{
		if(oldindex >= oldframe->numEntities) 
		{
			oldnum = 99999;
		} 
		else 
		{
			oldstate = GetEntity((oldframe->parseEntitiesNum + oldindex) & (MAX_PARSE_ENTITIES-1));
			oldnum = oldstate->number;
		}
	}

	for(;;)
	{
		newnum = msg.ReadBits(GENTITYNUM_BITS);

		if(newnum == (MAX_GENTITIES - 1))
		{
			break;
		}

		if(!msg.ValidState()) 
		{
			return false;
		}

		while(oldnum < newnum) 
		{
			//
			// One or more entities from the old packet is unchanged.
			//

			if(!DeltaEntity(msg, newframe, oldnum, oldstate, true)) return false;
			oldindex++;

			if(oldindex >= oldframe->numEntities) 
			{
				oldnum = 99999;
			} 
			else 
			{
				oldstate = GetEntity((oldframe->parseEntitiesNum + oldindex) & (MAX_PARSE_ENTITIES-1));
				oldnum = oldstate->number;
			}
		}

		if(oldnum == newnum)
		{
			//
			// Delta from previous state.
			//

			if(!DeltaEntity(msg, newframe, newnum, oldstate, false)) return false;
			oldindex++;

			if(oldindex >= oldframe->numEntities) 
			{
				oldnum = 99999;
			} 
			else
			{
				oldstate = GetEntity((oldframe->parseEntitiesNum + oldindex) & (MAX_PARSE_ENTITIES-1));
				oldnum = oldstate->number;
			}
			continue;
		}

		if(oldnum > newnum) 
		{
			//
			// Delta from the baseline.
			//

			if(!DeltaEntity(msg, newframe, newnum, GetBaseline(newnum), false)) return false;
			continue;
		}
	}

	// Any remaining entities in the old frame are copied over.
	while(oldnum != 99999) 
	{
		//
		// One or more entities from the old packet is unchanged.
		//

		if(!DeltaEntity(msg, newframe, oldnum, oldstate, true)) return false;
		oldindex++;

		if(oldindex >= oldframe->numEntities) 
		{
			oldnum = 99999;
		} 
		else 
		{
			oldstate = GetEntity((oldframe->parseEntitiesNum + oldindex) & (MAX_PARSE_ENTITIES-1));
			oldnum = oldstate->number;
		}
	}

	return true;
}

//
// Write a delta update of an entityState_t list to the output message.
//
void udtBaseParser::EmitPacketEntities(idClientSnapshotBase* from, idClientSnapshotBase* to)
{
	idEntityStateBase* oldent = 0;
	idEntityStateBase* newent = 0;
	s32 oldindex = 0;
	s32 newindex = 0;
	s32 oldnum;
	s32 newnum;

	s32 from_num_entities;
	if(!from) 
	{
		from_num_entities = 0;
	} 
	else 
	{
		from_num_entities = from->numEntities;
	}

	while(newindex < to->numEntities || oldindex < from_num_entities) 
	{
		if(newindex >= to->numEntities) 
		{
			newnum = 9999;
		} 
		else 
		{
			s32 entNum = (to->parseEntitiesNum + newindex) & (MAX_PARSE_ENTITIES - 1);
			newent = GetEntity(entNum);
			newnum = newent->number;
		}

		if(oldindex >= from_num_entities) 
		{
			oldnum = 9999;
		}
		else 
		{
			s32 entNum = (from->parseEntitiesNum + oldindex) & (MAX_PARSE_ENTITIES - 1);
			oldent = GetEntity(entNum);
			oldnum = oldent->number;
		}

		if(newnum == oldnum)
		{
			// Delta update from old position
			// because the force parameter is qfalse, this will not result
			// in any bytes being emitted if the entity has not changed at all.
			idLargestEntityState oldEntOutProto;
			idLargestEntityState newEntOutProto;
			_protocolConverter->ConvertEntityState(oldEntOutProto, *oldent);
			_protocolConverter->ConvertEntityState(newEntOutProto, *newent);
			_outMsg.WriteDeltaEntity(&oldEntOutProto, &newEntOutProto, false);
			oldindex++;
			newindex++;
			continue;
		}

		if(newnum < oldnum) 
		{
			// This is a new entity, send it from the baseline.
			idLargestEntityState baselineOutProto;
			idLargestEntityState newEntOutProto;
			idEntityStateBase* baseline = GetBaseline(newnum);
			_protocolConverter->ConvertEntityState(baselineOutProto, *baseline);
			_protocolConverter->ConvertEntityState(newEntOutProto, *newent);
			_outMsg.WriteDeltaEntity(&baselineOutProto, &newEntOutProto, true);
			newindex++;
			continue;
		}

		if(newnum > oldnum) 
		{
			// The old entity isn't present in the new message.
			idLargestEntityState oldEntOutProto;
			_protocolConverter->ConvertEntityState(oldEntOutProto, *oldent);
			_outMsg.WriteDeltaEntity(&oldEntOutProto, NULL, true);
			oldindex++;
			continue;
		}
	}

	_outMsg.WriteBits(MAX_GENTITIES - 1, GENTITYNUM_BITS);
}

//
// Parses deltas from the given base and adds the resulting entity to the current frame.
//
bool udtBaseParser::DeltaEntity(udtMessage& msg, idClientSnapshotBase *frame, s32 newnum, idEntityStateBase* old, bool unchanged)
{
	// Save the parsed entity state into the big circular buffer so
	// it can be used as the source for a later delta.
	idEntityStateBase* const state = GetEntity(_inParseEntitiesNum & (MAX_PARSE_ENTITIES-1));

	s32 removedEntityNumber = old ? old->number : 0;
	if(unchanged) 
	{
		Com_Memcpy(state, old, _inProtocolSizeOfEntityState);
	} 
	else 
	{
		bool addedOrChanged = false;
		if(!msg.ReadDeltaEntity(addedOrChanged, old, state, newnum))
		{
			return false;
		}

		if(addedOrChanged)
		{
			const bool isNewEvent = (state->eType >= ET_EVENTS) && (_inServerTime > _inEntityEventTimesMs[newnum] + EVENT_VALID_MSEC);
			udtChangedEntity info;
			info.Entity = state;
			info.IsNewEvent = isNewEvent;
			_inChangedEntities.Add(info);
			if(isNewEvent)
			{
				_inEntityEventTimesMs[newnum] = _inServerTime;
			}
		}
	}

	// The entity was delta removed?
	if(state->number == (MAX_GENTITIES-1)) 
	{
		// We have to return now.
		_inRemovedEntities.Add(removedEntityNumber);
		return true;
	}

	_inParseEntitiesNum++;
	frame->numEntities++;

	return true;
}

const udtString udtBaseParser::GetConfigString(s32 csIndex) const
{
	if(csIndex < 0 || csIndex >= (s32)UDT_COUNT_OF(_inConfigStrings))
	{
		return udtString::NewEmptyConstant();
	}

	return _inConfigStrings[csIndex];
}

char* udtBaseParser::AllocateString(udtVMLinearAllocator& allocator, const char* string, u32 stringLength, u32* outStringLength)
{
	if(stringLength == 0)
	{
		stringLength = (u32)strlen(string);
	}

	if(outStringLength != NULL)
	{
		*outStringLength = stringLength;
	}

	char* const stringCopy = (char*)allocator.Allocate(stringLength + 1);
	memcpy(stringCopy, string, stringLength);
	stringCopy[stringLength] = '\0';

	return stringCopy;
}

void udtBaseParser::AddPlugIn(udtBaseParserPlugIn* plugIn)
{
	PlugIns.Add(plugIn);
}
