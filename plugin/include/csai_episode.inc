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
#define EP_WINDUP    5   // a wind-up episode reached its handover tick

// Which weights file and batch directory this instance uses. Set per run so a
// second experiment can train at the same time without either seeing the
// other's data.
char  g_sSlot[64] = "";

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
float g_fStateLo       = 0.61;     // window to sample those from, as track fraction
float g_fStateHi       = 0.72;

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

// Wind-up episodes: the bot builds its own ground speed instead of replaying a
// recording. Scored on the state it hands over, one second away, rather than on
// a finish forty seconds away.
int   g_iWindupTicks   = 0;      // 0 = off; otherwise the length of a wind-up episode
float g_fWindupYawCap  = 3.5;    // degrees of view movement per tick
int   g_iWindupHold    = 12;     // ticks a strafe direction must be held
float g_fWindupPosCost = 0.05;   // per unit off the line at ramp contact

// Measured on the recorded wind-up: the view moves 0.53 deg/tick median and
// 3.06 at its fastest, and a strafe direction is held 16 to 41 ticks. The caps
// sit just outside that, so the bot can do what a person does and cannot
// tick-switch its way to a score that no human could reproduce.
float g_fWindupYaw     = 0.0;    // the view angle being steered, integrated
int   g_iWindupHeld    = 0;      // ticks the current side has been held
int   g_iWindupSide    = 1;
float g_fWindupPrevVz  = 0.0;
bool  g_bWindupFlew    = false;   // has left the ground at least once
ConVar g_cvGravity     = null;
int   g_iEpSide = 1;           // strafe side of the decision in flight
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
	delete g_hBatchFile;
	g_bBatchBroken = false;
	g_iBatchEpisodes = 0;
	g_fStatReturnSum = 0.0;
	g_fStatProgSum   = 0.0;
	g_iStatMidStart  = 0;
	g_iStatFell = 0;
	g_iStatFinished = 0;
	g_iStatTimeout = 0;

	// An eval writes a replay, not training data.
	if (g_bEval)
		return;

	char dir[PLATFORM_MAX_PATH];
	if (g_sSlot[0] == '\0')
		BuildPath(Path_SM, dir, sizeof(dir), "data/csai/out");
	else
		BuildPath(Path_SM, dir, sizeof(dir), "data/csai/out_%s", g_sSlot);
	if (!DirExists(dir))
		CreateDirectory(dir, 511);

	char path[PLATFORM_MAX_PATH];
	Format(path, sizeof(path), "%s/a%d_batch_%04d.bin", dir, g_iActorId, g_iBatchIndex);

	g_hBatchFile = OpenFile(path, "wb");
	if (g_hBatchFile == null)
	{
		PrintToServer("[CsAI] could not open batch file %s", path);
		g_bBatchBroken = true;      // no .done marker for a batch with no data
	}
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
		// Strafe key, with a floor on how long it must be held. The recorded
		// wind-up holds a direction for 16 to 41 ticks; without this the policy
		// can alternate every decision, which scores well and is not surfing.
		if (g_iEpSide != g_iWindupSide && g_iWindupHeld >= g_iWindupHold)
		{
			g_iWindupSide = g_iEpSide;
			g_iWindupHeld = 0;
		}
		g_iWindupHeld++;
		useLeft = (g_iWindupSide < 0);

		// Coast means jump here. Leaving the ledge is a decision a person makes,
		// and it is the one action the ground phase needs that steering cannot
		// express, so the otherwise unused action carries it.
		bool wantJump = g_bEpCoast;

		// Trim picks a turn RATE, not an angle, and the view integrates it. A
		// person sweeps the mouse; they do not teleport it. Symmetric and dense
		// near zero, spanning the 0.53 deg/tick median and 3.06 peak measured on
		// the recorded wind-up.
		static const float kWindupTurn[POL_TRIMS] =
			{ -3.0, -1.5, -0.75, -0.25, 0.25, 0.75, 1.5, 3.0 };
		int ti = g_iEpTrim;
		if (ti < 0) ti = 0;
		if (ti >= POL_TRIMS) ti = POL_TRIMS - 1;
		float turn = kWindupTurn[ti];
		if (turn >  g_fWindupYawCap) turn =  g_fWindupYawCap;
		if (turn < -g_fWindupYawCap) turn = -g_fWindupYawCap;

		g_fWindupYaw += turn;
		while (g_fWindupYaw >  180.0) g_fWindupYaw -= 360.0;
		while (g_fWindupYaw < -180.0) g_fWindupYaw += 360.0;

		angles[0] = 0.0;
		angles[1] = g_fWindupYaw;
		angles[2] = 0.0;
		movement[0] = 450.0;
		movement[1] = useLeft ? -450.0 : 450.0;
		movement[2] = 0.0;
		// Jump only when the policy coasts.
		buttons = (useLeft ? IN_MOVELEFT : IN_MOVERIGHT) | IN_FORWARD;
		if (wantJump)
			buttons |= IN_JUMP;
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
	if (outcome == EP_WINDUP)
	{
		// Scored on the state handed over, not on a finish forty seconds later.
		// Speed is only worth anything in the direction the run leaves in, and
		// only from the place it leaves from, so both are priced here.
		float pos[3], vel[3];
		GetClientAbsOrigin(g_iBot, pos);
		GetEntPropVector(g_iBot, Prop_Data, "m_vecVelocity", vel);

		// Speed carried onto the ramp is the whole point: the recorded sequence
		// reaches 282 u/s on the ground and 466 by the time it lands. Being fast
		// somewhere off the route is worth nothing, so distance from the line is
		// priced too.
		float along = SquareRoot(vel[0] * vel[0] + vel[1] * vel[1]);

		int hint = Track_Nearest(pos, -1);
		float closest[3], off;
		Track_Project(pos, hint, closest, off);

		terminal = along - g_fWindupPosCost * off;
		if (!g_bWindupFlew)
			terminal -= 200.0;      // never left the ground: not a wind-up at all

		if (g_bEval)
		{
			float sp = SquareRoot(vel[0] * vel[0] + vel[1] * vel[1]);
			PrintToServer("[CsAI]  wind-up: %d ticks, %.0f u/s onto the ramp, %.0f units off the line, airborne %s, score %.1f",
			              g_iEpTick, sp, off, g_bWindupFlew ? "yes" : "NO", terminal);
		}
	}
	else if (outcome == EP_FINISHED)
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

	// A wind-up episode builds ground speed from a standing start and is scored
	// on the state it hands over. Always from the recorded spawn, never a
	// checkpoint: there is nothing to wind up from in the middle of a map.
	if (g_iWindupTicks > 0)
	{
		g_iEpStartState = 0;
		Pre_Spawn(g_iBot);
		Ep_Reset();
		g_iEpPreTicks = g_iWindupTicks;
		g_bEpGround   = true;
		g_iWindupHeld = g_iWindupHold;      // free to pick a side on the first decision
		g_iWindupSide = 1;
		g_fWindupPrevVz = 0.0;
		g_bWindupFlew = false;
		float ang[3];
		GetClientEyeAngles(g_iBot, ang);
		g_fWindupYaw = ang[1];
		return;
	}

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

	// Wind-up: the ground phase lasts as long as the bot stands on the ground.
	if (g_iWindupTicks > 0)
		g_bEpGround = (GetEntityFlags(client) & FL_ONGROUND) != 0;
	else
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

	// This tick's position is the result of the decision in flight, so its
	// progress and deviation are charged to that decision before a new one starts.
	float dS = s - g_fEpLastS;
	g_fEpLastS = s;
	g_fEpStepReward += dS / 100.0;

	if (g_fDeviationCost > 0.0 && g_fMaxDeviation > 0.0)
	{
		float off = dist / g_fMaxDeviation;
		if (off > 1.0) off = 1.0;
		g_fEpStepReward -= g_fDeviationCost * off * off;
	}

	if (Ep_CheckEnd(client, s, dist, vel))
		return false;

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

		// Ground time in a wind-up is off the clock: the timer starts on the jump.
		if (!(g_iWindupTicks > 0 && g_bEpGround))
			g_fEpStepReward -= g_fTimeCost;

		g_fEpPendPos[0] = pos[0];
		g_fEpPendPos[1] = pos[1];
		g_fEpPendPos[2] = pos[2];
		g_fEpPendVel[0] = vel[0];
		g_fEpPendVel[1] = vel[1];
		g_fEpPendVel[2] = vel[2];
	}

	Ep_ApplyAction(vel, buttons, movement, angles);

	// Captured after the action is decoded, so the view angle is the one used.
	if (g_bEval)
	{
		Rec_Tick(client, buttons);
		if (g_bEpGround)
			g_iRecPreFrames++;
	}

	g_iEpTick++;
	return true;
}

/** Ends the episode if the current state is terminal. g_iEpTick ticks have run. */
stock bool Ep_CheckEnd(int client, float s, float dist, const float vel[3])
{
	if (g_iWindupTicks > 0)
	{
		// A wind-up ends on the first ramp. A surf ramp does not set FL_ONGROUND,
		// so contact shows as vertical acceleration that is no longer gravity.
		if (!g_bEpGround)
			g_bWindupFlew = true;

		if (g_cvGravity == null)
			g_cvGravity = FindConVar("sv_gravity");
		float gravity = (g_cvGravity != null) ? g_cvGravity.FloatValue : 800.0;

		float dvz = (vel[2] - g_fWindupPrevVz) * (1.0 / GetTickInterval());
		g_fWindupPrevVz = vel[2];

		bool onRamp = (g_bWindupFlew && !g_bEpGround && g_iEpTick > 1 && vel[2] < -200.0
		               && FloatAbs(dvz + gravity) > 150.0);

		if (onRamp || g_iEpTick >= g_iWindupTicks)
			return Ep_End(EP_WINDUP);
	}

	float speed = SquareRoot(vel[0] * vel[0] + vel[1] * vel[1]);
	if (speed < 40.0 && (GetEntityFlags(client) & FL_ONGROUND) && !g_bEpGround)
		g_iEpStuckTicks++;
	else if (!g_bEpGround)
		g_iEpStuckTicks = 0;

	// A truncated track ends where the point limit cut it, not at the finish.
	if (!g_bEpGround && !g_bTrackTruncated && g_fTrackLength > 0.0 && s >= g_fTrackLength - 96.0)
		return Ep_End(EP_FINISHED);
	if (!g_bEpGround && dist > g_fMaxDeviation)
		return Ep_End(EP_FELL);
	if (g_iEpStuckTicks > 120 || g_iEpNoProgress > g_iEpNoProgMax)
		return Ep_End(EP_STUCK);
	if (g_iEpTick >= g_iEpTickBudget)
		return Ep_End(EP_TIMEOUT);
	return false;
}

stock bool Ep_End(int outcome)
{
	if (g_iEpSteps > 0)
		Ep_PushStep();
	Ep_Finish(outcome);
	return true;
}
