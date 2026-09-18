#if defined _csai_demo_included
	#endinput
#endif
#define _csai_demo_included

#define DEMO_MAX_TICKS 20000
#define DEMO_MAX_FILES 32

int   g_iDemoFiles   = 0;     // how many runs exist on disk
int   g_iDemoFileIdx = 0;     // which one is loaded right now

float g_fDemoPos[DEMO_MAX_TICKS][3];
float g_fDemoAng[DEMO_MAX_TICKS][2];
int   g_iDemoButtons[DEMO_MAX_TICKS];
int   g_iDemoCount = 0;
int   g_iDemoPre   = 0;

bool  g_bDemoActive = false;
int   g_iDemoIndex  = 0;
int   g_iDemoSide   = 1;      // our tracked state, chasing the human's
int   g_iDemoTrim   = 0;
int   g_iDemoWrote  = 0;
File  g_hDemoOut    = null;

stock bool Demo_TickrateOk(const char[] header)
{
	int at = StrContains(header, "tickrate=");
	if (at == -1)
		return true;                       // older file, nothing to check

	char tmp[16];
	int k = 0;
	at += 9;
	while (at + k < strlen(header) && header[at + k] != ' ' && k < 15)
	{
		tmp[k] = header[at + k];
		k++;
	}
	tmp[k] = '\0';

	float rec = StringToFloat(tmp);
	if (rec <= 0.0)
		return true;

	float srv = 1.0 / GetTickInterval();
	return (FloatAbs(rec - srv) / srv) < 0.01;
}

stock void Demo_Path(int i, char[] path, int maxlen)
{
	char map[PLATFORM_MAX_PATH];
	GetCurrentMap(map, sizeof(map));
	if (i <= 0)
		BuildPath(Path_SM, path, maxlen, "data/csai/%s_demo.txt", map);
	else
		BuildPath(Path_SM, path, maxlen, "data/csai/%s_demo_%d.txt", map, i + 1);
}

/** Runs are numbered contiguously; the first gap ends the set. */
stock int Demo_CountFiles()
{
	char path[PLATFORM_MAX_PATH];
	int n = 0;
	for (int i = 0; i < DEMO_MAX_FILES; i++)
	{
		Demo_Path(i, path, sizeof(path));
		if (!FileExists(path))
			break;
		n++;
	}
	return n;
}

stock void Demo_TrimTeleports()
{
	int cut = -1;
	int limit = (g_iDemoPre > 0 && g_iDemoPre < g_iDemoCount) ? g_iDemoPre : g_iDemoCount;
	for (int i = 1; i < limit; i++)
	{
		float dx = g_fDemoPos[i][0] - g_fDemoPos[i - 1][0];
		float dy = g_fDemoPos[i][1] - g_fDemoPos[i - 1][1];
		float dz = g_fDemoPos[i][2] - g_fDemoPos[i - 1][2];
		if (SquareRoot(dx * dx + dy * dy + dz * dz) > 200.0)
			cut = i;
	}
	if (cut < 0)
		return;

	for (int i = cut; i < g_iDemoCount; i++)
	{
		int d = i - cut;
		g_fDemoPos[d][0] = g_fDemoPos[i][0];
		g_fDemoPos[d][1] = g_fDemoPos[i][1];
		g_fDemoPos[d][2] = g_fDemoPos[i][2];
		g_fDemoAng[d][0] = g_fDemoAng[i][0];
		g_fDemoAng[d][1] = g_fDemoAng[i][1];
		g_iDemoButtons[d] = g_iDemoButtons[i];
	}
	g_iDemoCount -= cut;
	g_iDemoPre = (g_iDemoPre > cut) ? (g_iDemoPre - cut) : 0;
	PrintToServer("[CsAI] demo: dropped %d ticks before a teleport (%d left, %d prestrafe)",
	              cut, g_iDemoCount, g_iDemoPre);
}

/** Load one run into the tick arrays, replacing whatever was there. */
stock bool Demo_LoadIndex(int which)
{
	g_iDemoCount = 0;
	g_iDemoPre   = 0;

	char path[PLATFORM_MAX_PATH];
	Demo_Path(which, path, sizeof(path));
	if (!FileExists(path))
		return false;

	File f = OpenFile(path, "r");
	if (f == null)
		return false;

	char line[256];
	char parts[10][32];
	while (!f.EndOfFile() && f.ReadLine(line, sizeof(line)))
	{
		TrimString(line);
		if (line[0] == '\0')
			continue;
		if (line[0] == '#')
		{
			if (!Demo_TickrateOk(line))
			{
				PrintToServer("[CsAI] demo: %s recorded at a different tickrate than this", path);
				PrintToServer("[CsAI]   server - refusing it. Re-time: python tools/setup_map.py <map>");
				delete f;
				g_iDemoCount = 0;
				return false;
			}
			if (StrContains(line, "preframes=") != -1)
			{
				int at = StrContains(line, "preframes=") + 10;
				char tmp[16];
				int k = 0;
				while (at + k < strlen(line) && line[at + k] != ' ' && k < 15)
				{
					tmp[k] = line[at + k];
					k++;
				}
				tmp[k] = '\0';
				g_iDemoPre = StringToInt(tmp);
			}
			continue;
		}
		if (g_iDemoCount >= DEMO_MAX_TICKS)
			break;

		int n = ExplodeString(line, " ", parts, sizeof(parts), sizeof(parts[]));
		if (n < 7)
			continue;

		int i = g_iDemoCount;
		g_fDemoPos[i][0] = StringToFloat(parts[1]);
		g_fDemoPos[i][1] = StringToFloat(parts[2]);
		g_fDemoPos[i][2] = StringToFloat(parts[3]);
		g_fDemoAng[i][0] = StringToFloat(parts[4]);
		g_fDemoAng[i][1] = StringToFloat(parts[5]);
		g_iDemoButtons[i] = StringToInt(parts[6]);
		g_iDemoCount++;
	}
	delete f;

	Demo_TrimTeleports();
	g_iDemoFileIdx = which;
	return g_iDemoCount > 0;
}

stock void Pre_BuildFromDemos()
{
	g_iPreSets = 0;
	int files = Demo_CountFiles();

	for (int i = 0; i < files && g_iPreSets < PRE_MAX_SETS; i++)
	{
		if (!Demo_LoadIndex(i))
			continue;
		int n = g_iDemoPre;
		if (n < 16 || n > PRE_MAX_TICKS)
			continue;

		int k = g_iPreSets;
		for (int t = 0; t < n; t++)
		{
			g_fPreAngSet[k][t][0] = g_fDemoAng[t][0];
			g_fPreAngSet[k][t][1] = g_fDemoAng[t][1];
			g_iPreBtnSet[k][t]    = g_iDemoButtons[t];
		}
		g_fPreStartSet[k][0] = g_fDemoPos[0][0];
		g_fPreStartSet[k][1] = g_fDemoPos[0][1];
		g_fPreStartSet[k][2] = g_fDemoPos[0][2];
		g_fPreStartAngSet[k][0] = g_fDemoAng[0][0];
		g_fPreStartAngSet[k][1] = g_fDemoAng[0][1];
		g_fPreStartAngSet[k][2] = 0.0;
		g_iPreCountSet[k] = n;
		g_iPreSets++;
	}

	if (g_iPreSets > 0)
	{
		int shortest = g_iPreCountSet[0];
		g_iPreLongest = 0;
		for (int i = 1; i < g_iPreSets; i++)
		{
			if (g_iPreCountSet[i] > g_iPreCountSet[g_iPreLongest])
				g_iPreLongest = i;
			if (g_iPreCountSet[i] < shortest)
				shortest = g_iPreCountSet[i];
		}
		Pre_Select(g_iPreLongest);
		PrintToServer("[CsAI] prestrafe: %d recorded set(s), %d..%d ticks (eval cycles all of them)",
		              g_iPreSets, shortest, g_iPreCountSet[g_iPreLongest]);
	}
}

stock void Demo_Load()
{
	g_iDemoFiles = Demo_CountFiles();
	if (g_iDemoFiles == 0)
	{
		char map[PLATFORM_MAX_PATH];
		GetCurrentMap(map, sizeof(map));
		g_iDemoCount = 0;
		PrintToServer("[CsAI] no demo file for %s", map);
		return;
	}

	Demo_LoadIndex(0);
	PrintToServer("[CsAI] demo: %d run(s), first has %d ticks (%d preframes)",
	              g_iDemoFiles, g_iDemoCount, g_iDemoPre);
}

stock bool Demo_HumanState(int idx, const float vel[3], int &side, int &trim)
{
	int b = g_iDemoButtons[idx];
	bool left  = (b & IN_MOVELEFT) != 0;
	bool right = (b & IN_MOVERIGHT) != 0;
	if (left == right)
		return false;                      // no strafe key, or both: nothing to clone

	float speed = SquareRoot(vel[0] * vel[0] + vel[1] * vel[1]);
	if (speed < 50.0)
		return false;

	float wishYaw = g_fDemoAng[idx][1] + (left ? 90.0 : -90.0);
	float velYaw  = RadToDeg(ArcTangent2(vel[1], vel[0]));
	float phi = wishYaw - velYaw;
	while (phi >  180.0) phi -= 360.0;
	while (phi < -180.0) phi += 360.0;

	side = (phi >= 0.0) ? 1 : -1;

	float want = float(side) * phi - 90.0;
	int best = 0;
	float bestd = -1.0;
	for (int i = 0; i < POL_TRIMS; i++)
	{
		float d = FloatAbs(Pol_TrimValue(i) - want);
		if (bestd < 0.0 || d < bestd)
		{
			bestd = d;
			best = i;
		}
	}
	trim = best;
	return true;
}

/** With absolute actions the label is simply the human's own state. */
stock int Demo_ActionToward(int wantSide, int wantTrim)
{
	return Pol_ActionFor(wantSide, wantTrim);
}

/** Put the bot at the loaded run's start and reset the per-run tracked state. */
stock void Demo_StartRun(int client)
{
	float ang[3];
	ang[0] = g_fDemoAng[0][0];
	ang[1] = g_fDemoAng[0][1];
	ang[2] = 0.0;
	float zero[3];
	zero[0] = 0.0; zero[1] = 0.0; zero[2] = 0.0;
	TeleportEntity(client, g_fDemoPos[0], ang, zero);

	g_iDemoIndex = 0;
	g_iDemoSide  = 1;
	g_iDemoTrim  = 0;
	g_iTrackHint = -1;
}

stock bool Demo_Begin(int client)
{
	g_iDemoFiles = Demo_CountFiles();
	if (g_iDemoFiles == 0 || !Demo_LoadIndex(0))
		return false;
	if (g_iDemoCount == 0 || client == -1 || !IsClientInGame(client))
		return false;

	char dir[PLATFORM_MAX_PATH];
	BuildPath(Path_SM, dir, sizeof(dir), "data/csai/out");
	if (!DirExists(dir))
		CreateDirectory(dir, 511);

	char map[PLATFORM_MAX_PATH];
	GetCurrentMap(map, sizeof(map));

	char path[PLATFORM_MAX_PATH];
	BuildPath(Path_SM, path, sizeof(path), "data/csai/out/%s_bc.txt", map);

	delete g_hDemoOut;
	g_hDemoOut = OpenFile(path, "w");
	if (g_hDemoOut == null)
	{
		PrintToServer("[CsAI] demo: could not open %s", path);
		return false;
	}
	g_hDemoOut.WriteLine("# csai bc v1  obs=%d actions=%d frameskip=%d", OBS_DIM, POL_ACTIONS, g_iFrameSkip);

	g_bDemoActive = true;
	g_iDemoWrote  = 0;
	Demo_StartRun(client);
	PrintToServer("[CsAI] demo capture started: %d run(s)", g_iDemoFiles);
	return true;
}

stock void Demo_Finish()
{
	g_bDemoActive = false;
	delete g_hDemoOut;
	PrintToServer("[CsAI] demo capture complete: %d samples from %d run(s)",
	              g_iDemoWrote, g_iDemoFiles);
}

/** One tick of demo playback. Returns false when the record is exhausted. */
stock bool Demo_Tick(int client, int &buttons, float movement[3], float angles[3])
{
	if (!g_bDemoActive)
		return false;

	if (g_iDemoIndex >= g_iDemoCount)
	{
		// This run is spent. Load the next one and keep appending to the same
		// output, so one capture pass yields every run's decisions.
		if (g_iDemoFileIdx + 1 < g_iDemoFiles && Demo_LoadIndex(g_iDemoFileIdx + 1))
		{
			Demo_StartRun(client);
			PrintToServer("[CsAI] demo: run %d/%d (%d ticks, %d samples so far)",
			              g_iDemoFileIdx + 1, g_iDemoFiles, g_iDemoCount, g_iDemoWrote);
			return true;
		}
		Demo_Finish();
		return false;
	}

	int idx = g_iDemoIndex;

	if (idx > 0)
	{
		float dx = g_fDemoPos[idx][0] - g_fDemoPos[idx - 1][0];
		float dy = g_fDemoPos[idx][1] - g_fDemoPos[idx - 1][1];
		float dz = g_fDemoPos[idx][2] - g_fDemoPos[idx - 1][2];
		if (SquareRoot(dx * dx + dy * dy + dz * dz) > 200.0)
		{
			float ang[3], vel[3];
			ang[0] = g_fDemoAng[idx][0];
			ang[1] = g_fDemoAng[idx][1];
			ang[2] = 0.0;
			// velocity from the next recorded tick, so the bot arrives moving as
			// the human was rather than dropping from a standstill
			float rate = 1.0 / GetTickInterval();
			if (idx + 1 < g_iDemoCount)
			{
				vel[0] = (g_fDemoPos[idx + 1][0] - g_fDemoPos[idx][0]) * rate;
				vel[1] = (g_fDemoPos[idx + 1][1] - g_fDemoPos[idx][1]) * rate;
				vel[2] = (g_fDemoPos[idx + 1][2] - g_fDemoPos[idx][2]) * rate;
			}
			else
			{
				vel[0] = 0.0; vel[1] = 0.0; vel[2] = 0.0;
			}
			TeleportEntity(client, g_fDemoPos[idx], ang, vel);
			g_iTrackHint = -1;          // full search: we are somewhere new
		}
	}

	// capture at the policy's own decision cadence, and only over the timed part
	if (idx >= g_iDemoPre && ((idx - g_iDemoPre) % g_iFrameSkip) == 0 && g_hDemoOut != null)
	{
		float pos[3], vel[3];
		GetClientAbsOrigin(client, pos);
		GetEntPropVector(client, Prop_Data, "m_vecVelocity", vel);

		int side, trim;
		if (Demo_HumanState(idx, vel, side, trim))
		{
			g_iTrackHint = Track_Nearest(pos, g_iTrackHint);

			float obs[OBS_DIM];
			Track_BuildObs(pos, vel, g_iTrackHint, obs);

			float probe[PROBE_DIM];
			Track_BuildProbe(client, pos, vel, probe);
			for (int pi = 0; pi < PROBE_DIM; pi++)
				obs[7 + 3 * LOOKAHEAD + pi] = probe[pi];

#if WISH_DIM > 0
			float wr = DegToRad(Pol_WishAngle(g_iDemoSide, g_iDemoTrim));
			obs[7 + 3 * LOOKAHEAD + PROBE_DIM + 0] = Cosine(wr);
			obs[7 + 3 * LOOKAHEAD + PROBE_DIM + 1] = Sine(wr);
#endif

			int action = Demo_ActionToward(side, trim);

			char line[1024];
			Format(line, sizeof(line), "%d", action);
			for (int oi = 0; oi < OBS_DIM; oi++)
			{
				char tmp[24];
				Format(tmp, sizeof(tmp), " %.6f", obs[oi]);
				StrCat(line, sizeof(line), tmp);
			}
			g_hDemoOut.WriteLine("%s", line);
			g_iDemoWrote++;

			// advance the tracked state exactly as the policy would
			Pol_ApplyAction(action, g_iDemoSide, g_iDemoTrim);
		}
	}

	int b = g_iDemoButtons[idx];
	angles[0] = g_fDemoAng[idx][0];
	angles[1] = g_fDemoAng[idx][1];
	angles[2] = 0.0;

	float fwd = 0.0;
	if (b & IN_FORWARD) fwd = 450.0;
	else if (b & IN_BACK) fwd = -450.0;
	float side2 = 0.0;
	if (b & IN_MOVERIGHT) side2 = 450.0;
	else if (b & IN_MOVELEFT) side2 = -450.0;

	movement[0] = fwd;
	movement[1] = side2;
	movement[2] = 0.0;
	buttons = b;

	g_iDemoIndex++;
	return true;
}
