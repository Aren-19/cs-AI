#if defined _csai_episode_included
	#endinput
#endif
#define _csai_episode_included

#define EP_REC_FLOATS   (9 + PROBE_DIM + 1)   // +1: held wish angle
#define EP_MAX_STEPS    4000

// outcomes
#define EP_RUNNING   0
#define EP_FELL      1
#define EP_FINISHED  2
#define EP_TIMEOUT   3
#define EP_STUCK     4

bool  g_bTraining      = false;
bool  g_bEval          = false;
bool  g_bEvalGreedy    = true;
bool  g_bScripted      = false;   // hand-coded controller, for interface testing
int   g_iFrameSkip     = 2;
float g_fMaxDeviation  = 600.0;    // units off the centerline before we call it a fall

float g_fDeviationCost = 0.5;

#define PITCH_GAIN   0.575
#define PITCH_BIAS   6.06
#define PITCH_ALPHA  0.05
#define PITCH_CAP    0.30
#define PITCH_MIN  (-20.0)
#define PITCH_MAX    70.0

int   g_iPitchMode  = 1;
float g_fPitchFixed = 0.0;
float g_fEpPitch    = 0.0;   // current smoothed pitch
int   g_iEpTickBudget  = 4000;
int   g_iTrainStates   = 0;        // 0 = sample from all loaded states

float g_fStateMix      = 0.0;      // share of episodes starting mid-map
float g_fStateLo       = 0.68;     // window to sample those from, as time fraction
float g_fStateHi       = 0.79;

int   g_iEpOutcome     = EP_RUNNING;
int   g_iEpTick        = 0;
int   g_iEpSteps       = 0;
bool  g_bEpCoast       = false;   // this decision presses no strafe key
bool  g_bBatchBroken   = false;   // this batch's file could not be opened

int   g_iForceSide     = 0;
int   g_iForceTrim     = -1;   // >=0 pins the trim too, for diagnosis
int   g_iEpStartState  = 0;
float g_fEpStartS      = 0.0;
float g_fEpLastS       = 0.0;
float g_fEpBestS       = 0.0;
float g_fEpReturn      = 0.0;
int   g_iEpAction      = 0;
float g_fEpLogp        = 0.0;
float g_fEpStepReward  = 0.0;
int   g_iEpStuckTicks  = 0;
int   g_iEpNoProgress  = 0;
int   g_iObsDump       = 0;        // remaining obs rows to dump for the parity check
File  g_hObsDumpFile   = null;
int   g_iEpNoProgMax   = 200;      // ticks without advancing before we give up
float g_fSwitchCost    = 0.40;

float g_fTimeCost      = 0.08;    // per decision
float g_fFinishBonus   = 50.0;    // what finishing exactly on the reference pays
float g_fFinishFloor   =  0.5;    // a finish never scores below this, however slow
float g_fTimePower     =  8.0;    // how sharply the payout falls off with time
float g_fTrimCost      = 0.05;    // per change of trim between decisions
float g_fStartMaxFrac  = 0.90;     // never start so near the end that we finish instantly

ArrayList g_hEpBuf = null;

// The decision in flight. A step record is written on the NEXT decision
// boundary, once the reward accumulated while holding that action is known.
float g_fEpPendPos[3];
float g_fEpPendVel[3];
float g_fEpPendProbe[PROBE_DIM];
float g_fEpPendWish = 90.0;
float g_fEpWishAngle = 90.0;   // = side * (90 + trim)

bool  g_bEpGround   = false;
int   g_iEpPreTicks = 0;

#define PRE_TRIM_SCALE 20.0
int   g_iEpSide = 1;           // sticky: only action 1 changes it
int   g_iEpTrim = 0;

int   g_iBatchEpisodes = 0;
int   g_iBatchTarget   = 64;
int   g_iBatchIndex    = 0;
File  g_hBatchFile     = null;

// rolling stats for the console
float g_fStatReturnSum = 0.0;
float g_fStatProgSum   = 0.0;
int   g_iStatMidStart  = 0;   // episodes that began away from the map start
int   g_iStatFell      = 0;
int   g_iStatFinished  = 0;
int   g_iStatTimeout   = 0;

stock void Ep_Init()
{
	if (g_hEpBuf == null)
		g_hEpBuf = new ArrayList(EP_REC_FLOATS);
}

stock void Ep_OpenBatch()
{
	char dir[PLATFORM_MAX_PATH];
	BuildPath(Path_SM, dir, sizeof(dir), "data/csai/out");
	if (!DirExists(dir))
		CreateDirectory(dir, 511);

	char path[PLATFORM_MAX_PATH];
	BuildPath(Path_SM, path, sizeof(path), "data/csai/out/a%d_batch_%04d.bin", g_iActorId, g_iBatchIndex);

	delete g_hBatchFile;
	g_hBatchFile = OpenFile(path, "wb");

	if (g_hBatchFile == null)
	{
		PrintToServer("[CsAI] could not open batch file %s", path);
		// Train_WriteDoneMarker would otherwise announce N episodes with no
		// data behind them.
		g_bBatchBroken = true;
	}
	else
	{
		g_bBatchBroken = false;
	}

	g_iBatchEpisodes = 0;
	g_fStatReturnSum = 0.0;
	g_fStatProgSum   = 0.0;
	g_iStatMidStart  = 0;
	g_iStatFell = 0;
	g_iStatFinished = 0;
	g_iStatTimeout = 0;
}

stock int Ep_PickStartState()
{
	if (g_fStateMix > 0.0 && g_iStateCount > 1 && Pol_Rand() < g_fStateMix)
	{
		int lo = -1, hi = -1;
		for (int i = 0; i < g_iStateCount; i++)
		{
			if (g_fStateFrac[i] > g_fStartMaxFrac)
				break;
			if (lo < 0 && g_fStateFrac[i] >= g_fStateLo)
				lo = i;
			if (g_fStateFrac[i] <= g_fStateHi)
				hi = i;
		}
		if (lo >= 0 && hi >= lo)
		{
			int span = hi - lo + 1;
			int p = lo + RoundToFloor(Pol_Rand() * float(span));
			return (p > hi) ? hi : p;
		}
	}

	int n = (g_iTrainStates > 0 && g_iTrainStates <= g_iStateCount) ? g_iTrainStates : g_iStateCount;

	// Drop start states too close to the finish: an episode beginning there trips
	// the finish test on its first tick and contributes a meaningless "success".
	int usable = n;
	while (usable > 1 && g_fStateFrac[usable - 1] > g_fStartMaxFrac)
		usable--;

	if (usable <= 1)
		return 0;

	int pick = RoundToFloor(Pol_Rand() * float(usable));
	if (pick >= usable)
		pick = usable - 1;
	return pick;
}

stock float Ep_ScriptedAngle(const float pos[3], const float vel[3])
{
	int idx = g_iTrackHint;
	if (idx < 0 || g_iTrackCount == 0)
		return g_fEpWishAngle;

	float speed = SquareRoot(vel[0] * vel[0] + vel[1] * vel[1]);
	if (speed < 1.0)
		return g_fEpWishAngle;

	// height error against the reference line
	float closest[3], dist;
	Track_Project(pos, idx, closest, dist);
	float vertErr = pos[2] - closest[2];      // >0 too high, <0 too low

	// ramp normal, horizontal part, from the surface probe
	float probe[PROBE_DIM];
	Track_BuildProbe(g_iBot, pos, vel, probe);
	// the probe normal is already in the velocity frame, which is the frame the
	// climb/descend decision needs - no rotation required
	float nleft = probe[PROBE_RAYS + 1];

	bool leftClimbs = (nleft < 0.0);

	bool wantClimb = (vertErr < -24.0);
	bool wantDrop  = (vertErr >  96.0);

	if (wantClimb)
		return leftClimbs ? 90.0 : -90.0;
	if (wantDrop)
		return leftClimbs ? -90.0 : 90.0;

	// in the band: hold the current side, so the line stays smooth
	return g_fEpWishAngle;
}

stock void Ep_PushStep()
{
	if (g_hEpBuf == null || g_hEpBuf.Length >= EP_MAX_STEPS)
		return;

	float row[EP_REC_FLOATS];
	row[0] = g_fEpPendPos[0];
	row[1] = g_fEpPendPos[1];
	row[2] = g_fEpPendPos[2];
	row[3] = g_fEpPendVel[0];
	row[4] = g_fEpPendVel[1];
	row[5] = g_fEpPendVel[2];
	row[6] = float(g_iEpAction);
	row[7] = g_fEpLogp;
	row[8] = g_fEpStepReward;
	for (int pi = 0; pi < PROBE_DIM; pi++)
		row[9 + pi] = g_fEpPendProbe[pi];
	row[9 + PROBE_DIM] = g_fEpPendWish;
	g_hEpBuf.PushArray(row, EP_REC_FLOATS);
	g_fEpReturn += g_fEpStepReward;
}

stock void Ep_ApplyAction(const float vel[3], int &buttons, float movement[3], float angles[3])
{
	float speed  = SquareRoot(vel[0] * vel[0] + vel[1] * vel[1]);
	float velYaw = (speed > 1.0) ? RadToDeg(ArcTangent2(vel[1], vel[0])) : angles[1];

	bool useLeft = (g_fEpWishAngle > 0.0);

	if (g_bEpGround)
	{
		// Pol_WishAngle takes a trim INDEX and looks its value up; the scale has
		// to be applied to the value, not the index.
		float phi = float(g_iEpSide) * (90.0 + Pol_TrimValue(g_iEpTrim) * PRE_TRIM_SCALE);
		useLeft = (phi > 0.0);
		float gYaw = velYaw + phi + (useLeft ? -45.0 : 45.0);
		while (gYaw >  180.0) gYaw -= 360.0;
		while (gYaw < -180.0) gYaw += 360.0;

		angles[0] = 0.0;
		angles[1] = gYaw;
		angles[2] = 0.0;
		movement[0] = 450.0;
		movement[1] = useLeft ? -450.0 : 450.0;
		movement[2] = 0.0;
		buttons = (useLeft ? IN_MOVELEFT : IN_MOVERIGHT) | IN_FORWARD;
		return;
	}

	float viewYaw = velYaw + g_fEpWishAngle + (useLeft ? -90.0 : 90.0);
	while (viewYaw >  180.0) viewYaw -= 360.0;
	while (viewYaw < -180.0) viewYaw += 360.0;

	if (g_iPitchMode == 1)
	{
		float vp = -RadToDeg(ArcTangent2(vel[2], (speed > 1.0) ? speed : 1.0));
		float target = PITCH_GAIN * vp + PITCH_BIAS;
		if (target < PITCH_MIN) target = PITCH_MIN;
		if (target > PITCH_MAX) target = PITCH_MAX;

		float step = (target - g_fEpPitch) * PITCH_ALPHA;
		if (step >  PITCH_CAP) step =  PITCH_CAP;
		if (step < -PITCH_CAP) step = -PITCH_CAP;
		g_fEpPitch += step;
	}
	else if (g_iPitchMode == 2)
	{
		g_fEpPitch = g_fPitchFixed;
	}
	else
	{
		g_fEpPitch = 0.0;
	}

	angles[0] = g_fEpPitch;
	angles[1] = viewYaw;
	angles[2] = 0.0;

	movement[0] = 0.0;
	movement[1] = g_bEpCoast ? 0.0 : (useLeft ? -450.0 : 450.0);
	movement[2] = 0.0;

	buttons = g_bEpCoast ? IN_JUMP : ((useLeft ? IN_MOVELEFT : IN_MOVERIGHT) | IN_JUMP);
}

stock void Ep_Finish(int outcome)
{
	g_iEpOutcome = outcome;

	// terminal reward folded into the last recorded step
	float terminal = 0.0;
	if (outcome == EP_FINISHED)
	{
		terminal = g_fFinishBonus;
		// Only a run from the start has a clock comparable to the human's.
		if (g_iEpStartState == 0 && g_fRefTime > 0.0)
		{
			float secs = float(g_iEpTick - g_iEpPreTicks) * GetTickInterval();

			// Ratio, not a line with a floor: a floored payout stops telling
			// a slow finish from a very slow one, and the run drifts.
			if (secs > 0.01)
				terminal = g_fFinishBonus * Pow(g_fRefTime / secs, g_fTimePower);
			if (terminal < g_fFinishFloor)
				terminal = g_fFinishFloor;
		}
	}
	else if (outcome == EP_FELL || outcome == EP_STUCK)
		terminal = -1.0;

	int n = g_hEpBuf.Length;
	if (n > 0 && terminal != 0.0)
	{
		float row[EP_REC_FLOATS];
		g_hEpBuf.GetArray(n - 1, row, EP_REC_FLOATS);
		row[8] += terminal;
		g_hEpBuf.SetArray(n - 1, row, EP_REC_FLOATS);
		g_fEpReturn += terminal;
	}

	if (g_hBatchFile != null)
	{
		int header[4];
		header[0] = n;
		header[1] = outcome;
		header[2] = g_iEpStartState;
		header[3] = view_as<int>(g_fEpBestS);      // raw float bits
		g_hBatchFile.Write(header, 4, 4);

		float row[EP_REC_FLOATS];
		for (int i = 0; i < n; i++)
		{
			g_hEpBuf.GetArray(i, row, EP_REC_FLOATS);
			g_hBatchFile.Write(row, EP_REC_FLOATS, 4);
		}
	}

	g_fStatReturnSum += g_fEpReturn;
	if (g_fTrackLength > 0.0)
		g_fStatProgSum += (g_fEpBestS - g_fEpStartS) / g_fTrackLength;

	if (outcome == EP_FELL || outcome == EP_STUCK)
		g_iStatFell++;
	else if (outcome == EP_FINISHED)
		g_iStatFinished++;
	else
		g_iStatTimeout++;

	g_iBatchEpisodes++;
}

stock void Ep_Begin()
{
	Ep_Init();
	g_hEpBuf.Clear();

	g_iEpStartState = Ep_PickStartState();
	if (g_iEpStartState != 0)
		g_iStatMidStart++;
	UnfreezeBot();

	if (g_iEpStartState == 0 && Pre_Begin(g_iBot))
		return;

	ApplyStateQuiet(g_iBot, g_iEpStartState);
	Ep_Reset();
}

/** Establish the episode baseline from wherever the bot currently is. */
stock void Ep_Reset()
{
	// Ep_Begin reaches here even when both Pre_Begin and ApplyStateQuiet bailed
	// out on an invalid client, and the bot can be kicked between batches.
	if (g_iBot == -1 || !IsClientInGame(g_iBot))
	{
		PrintToServer("[CsAI] episode reset with no bot - aborting the run");
		g_bTraining = false;
		g_bEval = false;
		return;
	}

	float pos[3], vel[3];
	GetClientAbsOrigin(g_iBot, pos);
	GetEntPropVector(g_iBot, Prop_Data, "m_vecVelocity", vel);

	g_iTrackHint = Track_Nearest(pos, -1);       // full scan after a teleport

	float closest[3], dist;
	g_fEpStartS = Track_Project(pos, g_iTrackHint, closest, dist);

	// Start where the human's view sits at handover rather than snapping from 0.
	g_fEpPitch      = PITCH_BIAS;

	g_fEpLastS      = g_fEpStartS;
	g_fEpBestS      = g_fEpStartS;
	g_iEpTick       = 0;
	g_iEpSteps      = 0;
	g_fEpReturn     = 0.0;
	g_iEpOutcome    = EP_RUNNING;
	g_iEpStuckTicks = 0;
	g_iEpNoProgress = 0;
	g_iEpPreTicks   = 0;
	g_bEpGround     = false;
	g_fEpStepReward = 0.0;
	// start from the speed-optimal perpendicular angle rather than an arbitrary one
	g_iEpSide = 1;
	g_iEpTrim = 0;
	g_fEpWishAngle = Pol_WishAngle(g_iEpSide, g_iEpTrim);
	g_fEpPendWish = g_fEpWishAngle;
}

stock bool Ep_Tick(int client, int &buttons, float movement[3], float angles[3])
{
	if (g_bPreActive)
	{
		if (g_iPreLearned > 0 && g_iPreIndex >= g_iPreCount - g_iPreLearned)
		{
			g_bPreActive = false;
			Ep_Reset();
			g_iEpPreTicks = g_iPreCount - g_iPreIndex;
			g_bEpGround   = true;
		}
		else if (Pre_Tick(buttons, movement, angles))
		{
			// Record it so the saved replay contains the whole run. It is still
			// not a training step - no observation, no action, no reward.
			if (g_bEval)
			{
				Rec_Tick(client, buttons);
				g_iRecPreFrames++;
			}
			return true;
		}
		Ep_Reset();                      // handover: the baseline starts here
	}

	// Still winding up? None of the run's bookkeeping applies until it ends.
	g_bEpGround = (g_iEpTick < g_iEpPreTicks);

	float pos[3], vel[3];
	GetClientAbsOrigin(client, pos);
	GetEntPropVector(client, Prop_Data, "m_vecVelocity", vel);

	g_iTrackHint = Track_Nearest(pos, g_iTrackHint);

	float closest[3], dist;
	float s = Track_Project(pos, g_iTrackHint, closest, dist);
	if (s > g_fEpBestS + 1.0)
	{
		g_fEpBestS = s;
		g_iEpNoProgress = 0;
	}
	else if (!g_bEpGround)
	{
		g_iEpNoProgress++;
	}

	if ((g_iEpTick % g_iFrameSkip) == 0)
	{
		if (g_iEpSteps > 0)
			Ep_PushStep();                 // bank the previous decision
		g_fEpStepReward = 0.0;             // open this decision's accumulator

		float obs[OBS_DIM];
		Track_BuildObs(pos, vel, g_iTrackHint, obs);
		Track_BuildProbe(client, pos, vel, g_fEpPendProbe);
		for (int pi = 0; pi < PROBE_DIM; pi++)
			obs[7 + 3 * LOOKAHEAD + pi] = g_fEpPendProbe[pi];

#if WISH_DIM > 0
		float wr = DegToRad(g_fEpWishAngle);
		obs[7 + 3 * LOOKAHEAD + PROBE_DIM + 0] = Cosine(wr);
		obs[7 + 3 * LOOKAHEAD + PROBE_DIM + 1] = Sine(wr);
#endif
		g_fEpPendWish = g_fEpWishAngle;

		if (g_iObsDump > 0 && g_hObsDumpFile != null)
		{
			char line[1024];
			Format(line, sizeof(line), "%.6f %.6f %.6f %.6f %.6f %.6f",
			       pos[0], pos[1], pos[2], vel[0], vel[1], vel[2]);
			for (int oi = 0; oi < OBS_DIM; oi++)
			{
				char tmp[24];
				Format(tmp, sizeof(tmp), " %.6f", obs[oi]);
				StrCat(line, sizeof(line), tmp);
			}
			g_hObsDumpFile.WriteLine("%s", line);
			g_iObsDump--;
			if (g_iObsDump == 0)
			{
				delete g_hObsDumpFile;
				PrintToServer("[CsAI] obs dump complete");
			}
		}

		g_iEpAction = Pol_Act(obs, g_fEpLogp, g_bEval && g_bEvalGreedy);

		if (g_bScripted)
		{
			g_fEpWishAngle = Ep_ScriptedAngle(pos, vel);
			g_iEpSide = (g_fEpWishAngle >= 0.0) ? 1 : -1;
		}
		else
		{
			int prevSide = g_iEpSide;
			int prevTrim = g_iEpTrim;
			g_bEpCoast = (g_iEpAction == POL_COAST);
			Pol_ApplyAction(g_iEpAction, g_iEpSide, g_iEpTrim);
			if (g_iForceSide != 0)
			{
				g_iEpSide  = (g_iForceSide == 1) ? 1 : -1;
				g_iEpTrim  = (g_iForceTrim >= 0) ? g_iForceTrim : 0;
				g_bEpCoast = false;
			}
			g_fEpWishAngle = Pol_WishAngle(g_iEpSide, g_iEpTrim);
			if (g_iEpSide != prevSide)
				g_fEpStepReward -= g_fSwitchCost;
			else if (g_iEpTrim != prevTrim)
				g_fEpStepReward -= g_fTrimCost;
		}
		while (g_fEpWishAngle >  180.0) g_fEpWishAngle -= 360.0;
		while (g_fEpWishAngle < -180.0) g_fEpWishAngle += 360.0;
		g_iEpSteps++;

		g_fEpPendPos[0] = pos[0];
		g_fEpPendPos[1] = pos[1];
		g_fEpPendPos[2] = pos[2];
		g_fEpPendVel[0] = vel[0];
		g_fEpPendVel[1] = vel[1];
		g_fEpPendVel[2] = vel[2];
	}

	float dS = s - g_fEpLastS;
	g_fEpLastS = s;
	g_fEpStepReward += dS / 100.0;

	// Every decision costs time.
	if ((g_iEpTick % g_iFrameSkip) == 0)
		g_fEpStepReward -= g_fTimeCost;

	if (g_fDeviationCost > 0.0 && g_fMaxDeviation > 0.0)
	{
		float off = dist / g_fMaxDeviation;
		if (off > 1.0) off = 1.0;
		g_fEpStepReward -= g_fDeviationCost * off * off;
	}

	Ep_ApplyAction(vel, buttons, movement, angles);

	// Capture the frame AFTER the action is decoded, so the recorded view angle
	// is the one the bot actually used this tick.
	if (g_bEval)
	{
		Rec_Tick(client, buttons);
		if (g_bEpGround)
			g_iRecPreFrames++;
	}

	g_iEpTick++;

	float speed = SquareRoot(vel[0] * vel[0] + vel[1] * vel[1]);
	// A wind-up starts slow and on the ground, which is the exact signature this
	// test calls stuck.
	if (speed < 40.0 && (GetEntityFlags(client) & FL_ONGROUND) && !g_bEpGround)
		g_iEpStuckTicks++;
	else if (!g_bEpGround)
		g_iEpStuckTicks = 0;

	// Not on a truncated track: its "end" is wherever the point limit cut it off,
	// so this test would award a completion bonus mid-map.
	if (!g_bEpGround && !g_bTrackTruncated && g_fTrackLength > 0.0 && s >= g_fTrackLength - 96.0)
	{
		Ep_PushStep();
		Ep_Finish(EP_FINISHED);
		return false;
	}
	if (!g_bEpGround && dist > g_fMaxDeviation)
	{
		Ep_PushStep();
		Ep_Finish(EP_FELL);
		return false;
	}
	if (g_iEpStuckTicks > 120 || g_iEpNoProgress > g_iEpNoProgMax)
	{
		Ep_PushStep();
		Ep_Finish(EP_STUCK);
		return false;
	}
	if (g_iEpTick >= g_iEpTickBudget)
	{
		Ep_PushStep();
		Ep_Finish(EP_TIMEOUT);
		return false;
	}
	return true;
}
