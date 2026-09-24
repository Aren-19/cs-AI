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
char  g_sPartner[64] = "";      // slot whose policy runs as the partner net; "main" = weights.txt

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

bool  g_bEpGround   = false;   // wind-up slot: the policy being trained is on the ground
int   g_iEpPreTicks = 0;

// Wind-up: the bot builds its own speed from standing, then jumps. The view
// turns at a capped rate and a strafe key is held a minimum time, as a person
// does it: the recorded wind-ups move the view 0.53 deg/tick median, 3.06 at
// most, and hold a key 16 to 41 ticks.
int   g_iWindupTicks   = 0;      // >0 = this slot trains the wind-up
int   g_iWindupMax     = 400;    // ground ticks before takeoff is forced
float g_fWindupYawCap  = 3.5;    // degrees of view movement per tick
int   g_iWindupHold    = 12;     // ticks a strafe key must be held
float g_fLearnedMix    = 0.0;    // main slot: share of runs opened by the learned wind-up
bool  g_bEvalLearned   = true;   // evals open with the learned wind-up when there is one

float g_fWindupYaw     = 0.0;    // the view angle being steered, integrated
int   g_iWindupHeld    = 0;      // ticks the current side has been held
int   g_iWindupSide    = 1;
bool  g_bEpLearnedOpen = false;  // main slot: the learned wind-up is driving
bool  g_bEpFollow      = false;  // wind-up slot: the main policy is flying the run
int   g_iEpClockTick   = 0;      // tick the timer started on
float g_fEpTakeoffSpeed = 0.0;

// How a wind-up ended. Only a jump from well inside the start zone is the real
// technique: the recorded runs jump 28 to 75 units in from its edge and drop off
// the ledge about 53 ticks later. Walking to the edge instead lets the timer
// start late, which reads as faster and is not a prestrafe.
#define TAKEOFF_NONE   0
#define TAKEOFF_JUMP   1
#define TAKEOFF_EDGE   2      // walked to the zone edge on the ground
#define TAKEOFF_DROP   3      // left the ground without jumping
#define TAKEOFF_LIMIT  4      // never jumped
int   g_iEpTakeoff     = TAKEOFF_NONE;
float g_fJumpInset     = 24.0;   // how far inside the zone edge the jump must be
int   g_iEpOutcomeRun  = EP_RUNNING;   // how the run itself ended
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

/** Ground wind-up controls: forward held, a strafe key held a minimum time, and a view that turns at a capped rate. */
stock void Ep_ApplyGround(int &buttons, float movement[3], float angles[3])
{
	if (g_iEpSide != g_iWindupSide && g_iWindupHeld >= g_iWindupHold)
	{
		g_iWindupSide = g_iEpSide;
		g_iWindupHeld = 0;
	}
	g_iWindupHeld++;
	bool useLeft = (g_iWindupSide < 0);

	// Trim picks a turn rate, not an angle: a person sweeps the mouse.
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
	buttons = (useLeft ? IN_MOVELEFT : IN_MOVERIGHT) | IN_FORWARD;
}

stock void Ep_ApplyAction(const float vel[3], int &buttons, float movement[3], float angles[3])
{
	float speed  = SquareRoot(vel[0] * vel[0] + vel[1] * vel[1]);
	float velYaw = (speed > 1.0) ? RadToDeg(ArcTangent2(vel[1], vel[0])) : angles[1];

	bool useLeft = (g_fEpWishAngle > 0.0);

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

/** Terminal reward for a wind-up: how the whole run it led into went. */
stock float Ep_WindupScore(int outcome)
{
	if (!g_bEpFollow)
		return -30.0;                        // never took off

	float secs = float(g_iEpTick - g_iEpClockTick) * GetTickInterval();
	if (outcome == EP_FINISHED && g_fRefTime > 0.0)
	{
		// 20 per second against the reference, so a tenth of a second is worth 2.
		float score = 20.0 * (g_fRefTime - secs) + 10.0;
		return (score < -10.0) ? -10.0 : score;
	}
	float frac = (g_fTrackLength > 0.0) ? (g_fEpBestS - g_fEpStartS) / g_fTrackLength : 0.0;
	return -20.0 + 10.0 * frac;
}

stock void Ep_Finish(int outcome)
{
	// terminal reward folded into the last recorded step
	float terminal = 0.0;
	g_iEpOutcomeRun = outcome;
	if (g_iWindupTicks > 0)
	{
		terminal = Ep_WindupScore(outcome);
		if (g_bEval)
		{
			static const char kRun[][] = { "running", "fell", "finished", "timed out", "stuck", "wind-up" };
			static const char kWhy[][] = { "never took off", "", "walked to the zone edge",
			                               "left the ground without jumping", "never jumped" };
			if (g_bEpFollow)
				PrintToServer("[CsAI]  wind-up: %d ground ticks, jumped at %.0f u/s, run %s in %.2fs, score %.1f",
				              g_iEpClockTick, g_fEpTakeoffSpeed, kRun[outcome],
				              float(g_iEpTick - g_iEpClockTick) * GetTickInterval(), terminal);
			else
				PrintToServer("[CsAI]  wind-up: %d ground ticks, %s, score %.1f",
				              g_iEpTick, kWhy[g_iEpTakeoff], terminal);
		}
		outcome = EP_WINDUP;
	}
	else if (outcome == EP_FINISHED)
	{
		terminal = g_fFinishBonus;
		// Only a run from the start has a clock comparable to the human's.
		if (g_iEpStartState == 0 && g_fRefTime > 0.0)
		{
			float secs = float(g_iEpTick - g_iEpPreTicks) * GetTickInterval();
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

	if (g_iEpOutcomeRun == EP_FELL || g_iEpOutcomeRun == EP_STUCK)
		g_iStatFell++;
	else if (g_iEpOutcomeRun == EP_FINISHED)
		g_iStatFinished++;
	else
		g_iStatTimeout++;

	g_iBatchEpisodes++;
}

/** The view and strafe state a wind-up starts from. */
stock void Ep_WindupInit()
{
	g_iWindupHeld = g_iWindupHold;      // free to pick a side on the first decision
	g_iWindupSide = 1;
	g_iEpSide = 1;
	g_iEpTrim = 0;
	float ang[3];
	GetClientEyeAngles(g_iBot, ang);
	g_fWindupYaw = ang[1];
}

stock void Ep_Begin()
{
	Ep_Init();
	g_hEpBuf.Clear();

	g_iEpStartState = Ep_PickStartState();
	if (g_iEpStartState != 0)
		g_iStatMidStart++;
	UnfreezeBot();
	g_bEpLearnedOpen = false;

	// Wind-up slot: from standing, always at the map start.
	if (g_iWindupTicks > 0)
	{
		g_iEpStartState = 0;
		Pre_Spawn(g_iBot);
		Ep_Reset();
		Ep_WindupInit();
		g_bEpGround = true;
		return;
	}

	// Main slot: open with the learned wind-up, or with a recorded one.
	if (g_iEpStartState == 0 && g_bPartnerLoaded &&
	    (g_bEval ? g_bEvalLearned : (Pol_Rand() < g_fLearnedMix)) && Pre_Spawn(g_iBot))
	{
		Ep_Reset();
		Ep_WindupInit();
		g_bEpLearnedOpen = true;
		return;
	}

	if (g_iEpStartState == 0 && Pre_Begin(g_iBot))
		return;

	ApplyStateQuiet(g_iBot, g_iEpStartState);
	Ep_Reset();
}

/** Controller state the air policy starts from. */
stock void Ep_ResetControl()
{
	g_fEpPitch = PITCH_BIAS;
	g_iEpSide = 1;
	g_iEpTrim = 0;
	g_bEpCoast = false;
	g_fEpWishAngle = Pol_WishAngle(g_iEpSide, g_iEpTrim);
	g_fEpPendWish = g_fEpWishAngle;
}

/** Establish the episode baseline from wherever the bot currently is. */
stock void Ep_Reset()
{
	// The bot can be kicked between batches.
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

	g_fEpLastS      = g_fEpStartS;
	g_fEpBestS      = g_fEpStartS;
	g_iEpTick       = 0;
	g_iEpSteps      = 0;
	g_fEpReturn     = 0.0;
	g_iEpStuckTicks = 0;
	g_iEpNoProgress = 0;
	g_iEpPreTicks   = 0;
	g_iEpClockTick  = 0;
	g_bEpGround     = false;
	g_bEpFollow     = false;
	g_iEpTakeoff    = TAKEOFF_NONE;
	g_fEpStepReward = 0.0;
	Ep_ResetControl();
}

/**
 * One wind-up decision on the ground. Returns false when it hands over: the
 * policy chose to jump, or the bot left the ground or the start zone. The timer
 * starts on that tick, and the air policy jumps from it.
 */
stock bool Ep_WindupTick(int client, const float pos[3], const float vel[3], bool learn)
{
	bool jump = false;
	if ((g_iEpTick % g_iFrameSkip) == 0)
	{
		if (learn)
		{
			if (g_iEpSteps > 0)
				Ep_PushStep();
			g_fEpStepReward = 0.0;
		}

		float obs[OBS_DIM];
		float probe[PROBE_DIM];
		Track_BuildObs(pos, vel, g_iTrackHint, obs);
		Track_BuildProbe(client, pos, vel, probe);
		for (int pi = 0; pi < PROBE_DIM; pi++)
			obs[7 + 3 * LOOKAHEAD + pi] = probe[pi];

		float logp;
		int act = Pol_Act(obs, logp, g_bEval && g_bEvalGreedy, learn ? NET_SELF : NET_PARTNER);
		int prevSide = g_iEpSide;
		int prevTrim = g_iEpTrim;
		jump = (act == POL_COAST);
		Pol_ApplyAction(act, g_iEpSide, g_iEpTrim);

		if (learn)
		{
			g_iEpAction = act;
			g_fEpLogp = logp;
			if (g_iEpSide != prevSide)
				g_fEpStepReward -= g_fSwitchCost;
			else if (g_iEpTrim != prevTrim)
				g_fEpStepReward -= g_fTrimCost;
			for (int pi = 0; pi < PROBE_DIM; pi++)
				g_fEpPendProbe[pi] = probe[pi];
			g_fEpPendWish = g_fEpWishAngle;
			g_fEpPendPos[0] = pos[0];
			g_fEpPendPos[1] = pos[1];
			g_fEpPendPos[2] = pos[2];
			g_fEpPendVel[0] = vel[0];
			g_fEpPendVel[1] = vel[1];
			g_fEpPendVel[2] = vel[2];
			g_iEpSteps++;
		}
	}

	int why = TAKEOFF_NONE;
	if (jump)
		why = TAKEOFF_JUMP;
	else if (!(GetEntityFlags(client) & FL_ONGROUND))
		why = TAKEOFF_DROP;
	else if (Zone_EdgeDistance(pos) < g_fJumpInset)
		why = TAKEOFF_EDGE;
	else if (g_iEpTick >= g_iWindupMax)
		why = TAKEOFF_LIMIT;
	if (why == TAKEOFF_NONE)
		return true;

	g_iEpTakeoff = why;
	g_fEpTakeoffSpeed = SquareRoot(vel[0] * vel[0] + vel[1] * vel[1]);
	return false;
}

stock bool Ep_Tick(int client, int &buttons, float movement[3], float angles[3])
{
	// Main slot, learned wind-up: the partner policy drives until takeoff, unrecorded.
	if (g_bEpLearnedOpen)
	{
		float p0[3], v0[3];
		GetClientAbsOrigin(client, p0);
		GetEntPropVector(client, Prop_Data, "m_vecVelocity", v0);
		g_iTrackHint = Track_Nearest(p0, g_iTrackHint);
		if (Ep_WindupTick(client, p0, v0, false))
		{
			Ep_ApplyGround(buttons, movement, angles);
			if (g_bEval)
			{
				Rec_Tick(client, buttons);
				g_iRecPreFrames++;
			}
			g_iEpTick++;
			return true;
		}
		g_bEpLearnedOpen = false;
		float takeoff = g_fEpTakeoffSpeed;
		Ep_Reset();                      // the clock starts on this tick
		g_fEpTakeoffSpeed = takeoff;
	}

	if (g_bPreActive)
	{
		if (Pre_Tick(buttons, movement, angles))
		{
			// Recorded for the replay; not a training step.
			if (g_bEval)
			{
				Rec_Tick(client, buttons);
				g_iRecPreFrames++;
			}
			return true;
		}
		Ep_Reset();                      // handover: the baseline starts here
	}

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
	// A wind-up is scored on its run alone.
	float dS = s - g_fEpLastS;
	g_fEpLastS = s;
	if (g_iWindupTicks == 0)
	{
		g_fEpStepReward += dS / 100.0;
		if (g_fDeviationCost > 0.0 && g_fMaxDeviation > 0.0)
		{
			float off = dist / g_fMaxDeviation;
			if (off > 1.0) off = 1.0;
			g_fEpStepReward -= g_fDeviationCost * off * off;
		}
	}

	if (Ep_CheckEnd(client, s, dist, vel))
		return false;

	// Wind-up slot: the policy being trained drives the ground phase.
	if (g_bEpGround)
	{
		if (Ep_WindupTick(client, pos, vel, true))
		{
			Ep_ApplyGround(buttons, movement, angles);
			if (g_bEval)
			{
				Rec_Tick(client, buttons);
				g_iRecPreFrames++;
			}
			g_iEpTick++;
			return true;
		}
		// Anything but a jump from inside the zone is a failed wind-up.
		if (g_iEpTakeoff != TAKEOFF_JUMP)
		{
			Ep_End(EP_STUCK);
			return false;
		}

		// Takeoff: bank the last wind-up decision, then the main policy flies the
		// rest frozen, so the wind-up is judged on the run it sets up.
		if (g_iEpSteps > 0)
			Ep_PushStep();
		g_bEpGround = false;
		g_bEpFollow = true;
		g_iEpClockTick = g_iEpTick;
		g_iEpPreTicks = g_iEpTick;
		g_iEpNoProgress = 0;
		Ep_ResetControl();
	}

	if (((g_iEpTick - g_iEpClockTick) % g_iFrameSkip) == 0)
	{
		if (!g_bEpFollow)
		{
			if (g_iEpSteps > 0)
				Ep_PushStep();             // bank the previous decision
			g_fEpStepReward = 0.0;         // open this decision's accumulator
		}

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

		float logp;
		int act = g_bEpFollow ? Pol_Act(obs, logp, true, NET_PARTNER)
		                      : Pol_Act(obs, logp, g_bEval && g_bEvalGreedy, NET_SELF);

		if (g_bScripted)
		{
			g_fEpWishAngle = Ep_ScriptedAngle(pos, vel);
			g_iEpSide = (g_fEpWishAngle >= 0.0) ? 1 : -1;
		}
		else
		{
			int prevSide = g_iEpSide;
			int prevTrim = g_iEpTrim;
			g_bEpCoast = (act == POL_COAST);
			Pol_ApplyAction(act, g_iEpSide, g_iEpTrim);
			if (g_iForceSide != 0)
			{
				g_iEpSide  = (g_iForceSide == 1) ? 1 : -1;
				g_iEpTrim  = (g_iForceTrim >= 0) ? g_iForceTrim : 0;
				g_bEpCoast = false;
			}
			g_fEpWishAngle = Pol_WishAngle(g_iEpSide, g_iEpTrim);
			if (!g_bEpFollow)
			{
				if (g_iEpSide != prevSide)
					g_fEpStepReward -= g_fSwitchCost;
				else if (g_iEpTrim != prevTrim)
					g_fEpStepReward -= g_fTrimCost;
			}
		}
		while (g_fEpWishAngle >  180.0) g_fEpWishAngle -= 360.0;
		while (g_fEpWishAngle < -180.0) g_fEpWishAngle += 360.0;

		if (!g_bEpFollow)
		{
			g_iEpAction = act;
			g_fEpLogp = logp;
			g_iEpSteps++;
			g_fEpStepReward -= g_fTimeCost;

			g_fEpPendPos[0] = pos[0];
			g_fEpPendPos[1] = pos[1];
			g_fEpPendPos[2] = pos[2];
			g_fEpPendVel[0] = vel[0];
			g_fEpPendVel[1] = vel[1];
			g_fEpPendVel[2] = vel[2];
		}
	}

	Ep_ApplyAction(vel, buttons, movement, angles);

	// Captured after the action is decoded, so the view angle is the one used.
	if (g_bEval)
		Rec_Tick(client, buttons);

	g_iEpTick++;
	return true;
}

/** Ends the episode if the current state is terminal. g_iEpTick ticks have run. */
stock bool Ep_CheckEnd(int client, float s, float dist, const float vel[3])
{
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
	if (g_iEpSteps > 0 && !g_bEpFollow)
		Ep_PushStep();
	Ep_Finish(outcome);
	return true;
}
