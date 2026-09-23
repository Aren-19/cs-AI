#if defined _csai_prestrafe_included
	#endinput
#endif
#define _csai_prestrafe_included

#define PRE_MAX_TICKS 512
#define PRE_MAX_SETS   16

float g_fPreAngSet[PRE_MAX_SETS][PRE_MAX_TICKS][2];
int   g_iPreBtnSet[PRE_MAX_SETS][PRE_MAX_TICKS];
float g_fPreStartSet[PRE_MAX_SETS][3];
float g_fPreStartAngSet[PRE_MAX_SETS][3];
int   g_iPreCountSet[PRE_MAX_SETS];
int   g_iPreSets = 0;
int   g_iPreLongest = 0;   // the fullest wind-up; the default until a run picks one

float g_fPreAng[PRE_MAX_TICKS][2];
int   g_iPreButtons[PRE_MAX_TICKS];
float g_fPreStart[3];
float g_fPreStartAng[3];
int   g_iPreCount   = 0;

// Trailing ticks of the wind-up the policy drives. Measured worse above 16.
int   g_iPreLearned = 0;

bool  g_bPrestrafeEnabled = true;
bool  g_bPreActive  = false;     // currently replaying
int   g_iPreIndex   = 0;

stock void Pre_Load()
{
	g_iPreCount = 0;

	char map[PLATFORM_MAX_PATH];
	GetCurrentMap(map, sizeof(map));

	char path[PLATFORM_MAX_PATH];
	BuildPath(Path_SM, path, sizeof(path), "data/csai/%s_prestrafe.txt", map);

	if (!FileExists(path))
	{
		PrintToServer("[CsAI] no prestrafe file for %s", map);
		return;
	}

	File f = OpenFile(path, "r");
	if (f == null)
		return;

	char line[256];
	char parts[12][32];

	while (!f.EndOfFile() && f.ReadLine(line, sizeof(line)))
	{
		TrimString(line);
		if (line[0] == '#')
		{
			if (!Demo_TickrateOk(line))
			{
				PrintToServer("[CsAI] prestrafe: recorded at a different tickrate");
				PrintToServer("[CsAI]   than this server - refusing it. Re-time: setup_map.py");
				delete f;
				g_iPreCount = 0;
				return;
			}
			continue;
		}
		if (line[0] == '\0')
			continue;
		if (g_iPreCount >= PRE_MAX_TICKS)
			break;

		int n = ExplodeString(line, " ", parts, sizeof(parts), sizeof(parts[]));
		if (n < 9)
			continue;

		int i = g_iPreCount;
		if (i == 0)
		{
			g_fPreStart[0] = StringToFloat(parts[1]);
			g_fPreStart[1] = StringToFloat(parts[2]);
			g_fPreStart[2] = StringToFloat(parts[3]);
			g_fPreStartAng[0] = StringToFloat(parts[4]);
			g_fPreStartAng[1] = StringToFloat(parts[5]);
			g_fPreStartAng[2] = 0.0;
		}
		g_fPreAng[i][0]   = StringToFloat(parts[4]);
		g_fPreAng[i][1]   = StringToFloat(parts[5]);
		g_iPreButtons[i]  = StringToInt(parts[6]);
		g_iPreCount++;
	}
	delete f;

	PrintToServer("[CsAI] prestrafe: %d recorded ticks for %s", g_iPreCount, map);
}

/** Load one of the recorded prestrafes into the active slot. */
stock void Pre_Select(int which)
{
	if (g_iPreSets <= 0)
		return;
	if (which < 0) which = 0;
	if (which >= g_iPreSets) which = g_iPreSets - 1;

	g_iPreCount = g_iPreCountSet[which];
	for (int i = 0; i < g_iPreCount; i++)
	{
		g_fPreAng[i][0]  = g_fPreAngSet[which][i][0];
		g_fPreAng[i][1]  = g_fPreAngSet[which][i][1];
		g_iPreButtons[i] = g_iPreBtnSet[which][i];
	}
	g_fPreStart[0] = g_fPreStartSet[which][0];
	g_fPreStart[1] = g_fPreStartSet[which][1];
	g_fPreStart[2] = g_fPreStartSet[which][2];
	g_fPreStartAng[0] = g_fPreStartAngSet[which][0];
	g_fPreStartAng[1] = g_fPreStartAngSet[which][1];
	g_fPreStartAng[2] = 0.0;
}

/** Place the bot where a wind-up starts, standing still, and leave it there. */
stock bool Pre_Spawn(int client)
{
	if (g_iPreSets > 0)
		Pre_Select(g_bEval ? (g_iEvalDone % g_iPreSets)
		                   : RoundToFloor(Pol_Rand() * float(g_iPreSets)));
	if (g_iPreCount == 0 || client == -1 || !IsClientInGame(client))
		return false;

	float zero[3];
	zero[0] = 0.0; zero[1] = 0.0; zero[2] = 0.0;
	TeleportEntity(client, g_fPreStart, g_fPreStartAng, zero);
	g_bPreActive = false;          // the policy drives from here, not the recording
	return true;
}

stock bool Pre_Begin(int client)
{
	if (g_iPreSets > 0)
		Pre_Select(g_bEval ? (g_iEvalDone % g_iPreSets)
		                   : RoundToFloor(Pol_Rand() * float(g_iPreSets)));

	if (!g_bPrestrafeEnabled || g_iPreCount == 0 || client == -1 || !IsClientInGame(client))
		return false;

	float zero[3];
	zero[0] = 0.0; zero[1] = 0.0; zero[2] = 0.0;
	TeleportEntity(client, g_fPreStart, g_fPreStartAng, zero);

	g_bPreActive = true;
	g_iPreIndex = 0;
	return true;
}

stock bool Pre_Tick(int &buttons, float movement[3], float angles[3])
{
	if (!g_bPreActive || g_iPreIndex >= g_iPreCount)
	{
		g_bPreActive = false;
		return false;
	}

	int b = g_iPreButtons[g_iPreIndex];

	angles[0] = g_fPreAng[g_iPreIndex][0];
	angles[1] = g_fPreAng[g_iPreIndex][1];
	angles[2] = 0.0;

	float fwd = 0.0;
	if (b & IN_FORWARD) fwd =  450.0;
	else if (b & IN_BACK) fwd = -450.0;

	float side = 0.0;
	if (b & IN_MOVERIGHT) side =  450.0;
	else if (b & IN_MOVELEFT) side = -450.0;

	movement[0] = fwd;
	movement[1] = side;
	movement[2] = 0.0;
	buttons = b;

	g_iPreIndex++;
	return true;
}
