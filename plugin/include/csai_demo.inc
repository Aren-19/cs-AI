/**
 * csai_demo - capture behaviour-cloning data from the human run.
 *
 * Progress reward alone does not teach technique. It rewarded the policy for
 * micro-oscillating +-90 degrees every couple of ticks, because that tracks the
 * reference line more tightly than real surfing does: measured 19.79 side
 * switches per second against a human's 0.95, holding each side 2 ticks against
 * 39.5. That is not something more training fixes - it is what the reward asked
 * for.
 *
 * Cloning asks a different question: given this situation, what did the HUMAN
 * do? Their inputs contain all the technique the reward cannot express - holding
 * a key through a curve and steering with the view, not strafing through tight
 * ramp sections, strafing on long airborne stretches.
 *
 * Method: replay the recorded inputs through real physics so the bot travels the
 * human's actual trajectory, and at every decision boundary record
 *   (observation the policy would see, action the policy would have to take).
 *
 * The second half used to need care, because actions were TRANSITIONS (hold,
 * flip side, set trim) and the label had to be the move from our tracked state
 * toward the human's. Cloning could not fit that: val accuracy 0.166 against a
 * 0.724 always-HOLD baseline, because the same situation gets the label HOLD or
 * FLIP depending on a state the observation does not contain.
 *
 * Actions are now ABSOLUTE (side, trim), so the label is just the human's own
 * state, which the observation does predict - 0.959 val accuracy on side alone
 * against a 0.613 majority baseline. Their (side, trim) is recovered from the
 * wish angle: phi = wishYaw - velYaw, side = sign(phi), trim = side*phi - 90.
 */

#if defined _csai_demo_included
	#endinput
#endif
#define _csai_demo_included

#define DEMO_MAX_TICKS 20000
#define DEMO_MAX_FILES 32

/**
 * Cloning one trajectory is the binding constraint: a single run gives ~1213
 * decisions, all on one line, so the policy has never seen a state it did not
 * arrive at perfectly and cannot recover from one. More recorded runs are the
 * direct fix, so capture takes a SET of them.
 *
 * Runs are held one at a time rather than all at once - 32 runs x 20000 ticks
 * would not fit - and every run appends to the same output file.
 *
 *   <map>_demo.txt      run 1
 *   <map>_demo_2.txt    run 2
 *   <map>_demo_<n>.txt  run n
 */
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

	g_iDemoFileIdx = which;
	return g_iDemoCount > 0;
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

/**
 * The human's absolute (side, trim) at this tick.
 *
 * wishdir is the view's right vector when sidemove > 0 and its left vector when
 * sidemove < 0, so phi = wishYaw - velYaw and, since phi = side * (90 + trim),
 * trim = side * phi - 90.
 */
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

			// advance our tracked state exactly as the policy would
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
