#include "analysis_cut_by_frag.hpp"


static bool AreTeammates(s32 team1, s32 team2)
{
	return
		(team1 == team2) &&
		(team1 >= 0) &&
		(team1 < (s32)udtTeam::Count) &&
		(team1 == (s32)udtTeam::Red || (s32)team1 == udtTeam::Blue);
}


void udtCutByFragAnalyzer::FindCutSections()
{
	const s32 playerIndex = (_info.PlayerIndex >= 0 && _info.PlayerIndex < 64) ? _info.PlayerIndex : _analyzer.RecordingPlayerIndex;
	const bool allowSelfKills = (_info.Flags & (u32)udtCutByFragArgFlags::AllowSelfKills) != 0;
	const bool allowTeamKills = (_info.Flags & (u32)udtCutByFragArgFlags::AllowTeamKills) != 0;
	const bool allowAnyDeath = (_info.Flags & (u32)udtCutByFragArgFlags::AllowDeaths) != 0;

	for(u32 i = 0, count = _analyzer.Obituaries.GetSize(); i < count; ++i)
	{
		const udtParseDataObituary& data = _analyzer.Obituaries[i];

		// Got killed?
		if(data.TargetIdx == playerIndex)
		{
			if(!allowAnyDeath || (!allowSelfKills && data.AttackerIdx == data.TargetIdx))
			{
				AddCurrentSectionIfValid();
			}
			continue;
		}

		// Someone else did the kill?
		if(data.AttackerIdx != playerIndex)
		{
			continue;
		}

		// We killed someone we shouldn't have?
		if(AreTeammates(data.TargetTeamIdx, data.AttackerTeamIdx))
		{
			if(!allowTeamKills)
			{
				AddCurrentSectionIfValid();
			}
			continue;
		}

		//
		// We passed all the filters, so we got a match.
		//

		if(_frags.IsEmpty())
		{
			AddMatch(data);
		}
		else
		{
			const Frag previousMatch = _frags[_frags.GetSize() - 1];
			if(data.GameStateIndex != previousMatch.GameStateIndex ||
			   data.ServerTimeMs > previousMatch.ServerTimeMs + (s32)(_info.TimeBetweenFragsSec * 1000))
			{
				AddCurrentSectionIfValid();
			}

			AddMatch(data);
		}
	}

	AddCurrentSectionIfValid();
}

void udtCutByFragAnalyzer::AddCurrentSectionIfValid()
{
	const u32 fragCount = _frags.GetSize();
	if(fragCount < 2 || fragCount < _info.MinFragCount)
	{
		_frags.Clear();
		return;
	}

	CutSection cut;
	cut.GameStateIndex = _frags[0].GameStateIndex;
	cut.StartTimeMs = _frags[0].ServerTimeMs - (s32)(_info.StartOffsetSec * 1000);
	cut.EndTimeMs = _frags[fragCount - 1].ServerTimeMs + (s32)(_info.EndOffsetSec * 1000);
	CutSections.Add(cut);

	_frags.Clear();
}

void udtCutByFragAnalyzer::AddMatch(const udtParseDataObituary& data)
{
	Frag match;
	match.ServerTimeMs = data.ServerTimeMs;
	match.GameStateIndex = data.GameStateIndex;
	_frags.Add(match);
}