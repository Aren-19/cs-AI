#if defined _csai_episode_included
	#endinput
#endif
#define _csai_episode_included

// One step record: pos, vel, action, logp, reward, the observation the policy
// acted on, and the actions it was allowed (bit mask stored as raw bits).
#define EP_REC_FLOATS   (9 + OBS_DIM + 1)
#define EP_MAX_STEPS    8000
#define BATCH_MAGIC     0x42534343     // "CCSB"
#define BATCH_VERSION   2

// outcomes
#define EP_RUNNING   0
#define EP_FELL      1
#define EP_FINISHED  2
#define EP_TIMEOUT   3
#define EP_STUCK     4
#define EP_WINDUP    5   // a wind-up episode, scored on the run it led into

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
float g_fMaxDeviation  = 600.0;    // units off the centerline that count as a fall

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
int   g_iEpNoProgMax   = 150;      // ticks without advancing before the run ends
int   g_iEpStuckMax    = 90;       // ticks standing still on the ground
float g_fSwitchCost    = 0.40;

float g_fTimeCost      = 0.08;    // per decision
float g_fFinishBonus   = 50.0;    // what finishing exactly on the reference pays
float g_fFinishFloor   =  0.5;    // a finish never scores below this, however slow
float g_fTimePower     =  8.0;    // how sharply the payout falls off with time
float g_fTrimCost      = 0.05;    // per change of trim between decisions
float g_fStartMaxFrac  = 0.90;     // never start so near the end that the run finishes at once

ArrayList g_hEpBuf = null;

// The decision in flight. A step record is written on the NEXT decision
// boundary, once the reward accumulated while holding that action is known.
float g_fEpPendPos[3];
float g_fEpPendVel[3];
float g_fEpPendObs[OBS_DIM];
int   g_iEpPendMask  = POL_MASK_ALL;
float g_fEpWishAngle = 90.0;   // = side * (90 + trim)

bool  g_bEpGround   = false;   // wind-up slot: the policy being trained is winding up
int   g_iEpPreTicks = 0;

// Hands and mouse, limited to what a person does. Measured on the recorded
// runs: view speed at most 3.2 deg/tick on the ground and 6.8 in the air;
// view acceleration at most 0.9 on the ground, 2.5 in the start zone air and
// mostly under 0.6 in the run; a strafe key held 16 ticks or more.
float g_fMouseAcc      = 1.5;    // in the run, deg/tick^2; 0 = view set straight to the aim, no mouse model
float g_fMouseMax      = 7.0;    // in the air, deg/tick
float g_fWindupAcc     = 2.5;    // in the start zone air
float g_fGroundAcc     = 1.0;
float g_fGroundMax     = 3.5;
int   g_iMinPress      = 12;     // ticks a strafe key stays down before it may change or lift
int   g_iMinCoast      = 6;      // ticks with no key before one may be pressed again

// The mouse eases towards the speed it is asked for, a share of the gap each
// tick, so a swipe speeds up and settles like a hand instead of snapping.
#define MOUSE_EASE   0.35        // share of the speed gap closed per tick
#define MOUSE_AIM    0.3         // run: extra speed per degree the view is off its aim

float g_fMouseYaw      = 0.0;    // the view
float g_fMouseW        = 0.0;    // its speed, deg/tick
float g_fMouseAim      = 0.0;    // run: last aim, for how fast the aim itself moves
bool  g_bMouseAimSet   = false;
int   g_iKeySide       = 0;      // key down now: +1 = A (left), -1 = D (right), 0 = none
int   g_iKeyHeld       = 0;      // ticks since the key state last changed
int   g_iCmdSide       = 0;      // key the policy asked for; the hand lifts it during a reversal
int   g_iCmdHeld       = 0;      // ticks since that last changed
int   g_iPhaseTicks    = 0;      // ticks since the current driver took over

// Wind-up: the bot builds its own speed from standing, jumps well inside the
// start zone, and strafes left and right in the air until it leaves the zone.
// Each action is a gesture: a key and a mouse speed, turning towards that key.
int   g_iWindupTicks   = 0;      // >0 = this slot trains the wind-up
int   g_iWindupMax     = 400;    // ground ticks before the wind-up counts as failed
int   g_iWindupAirMax  = 150;    // air ticks in the zone before it counts as failed
int   g_iWindupHold    = 12;     // ticks a requested side is kept before it may change
float g_fLearnedMix    = 0.0;    // main slot: share of runs opened by the learned wind-up
bool  g_bEvalLearned   = true;   // evals open with the learned wind-up when there is one

bool  g_bWindupLearn   = false;  // ...and its decisions are the ones being trained
int   g_iWindupReqSide = 1;      // side the gesture asks for
int   g_iWindupReqHeld = 0;      // ticks since the requested side changed
float g_fWindupTarget  = 0.0;    // commanded mouse speed, deg/tick
bool  g_bWindupCoast   = false;  // in the air: hands off the keys
bool  g_bWindupJumped  = false;
int   g_iWindupAirTicks = 0;

bool  g_bEpLearnedOpen = false;  // main slot: the learned wind-up is driving
bool  g_bEpFollow      = false;  // wind-up slot: the main policy is flying the run
int   g_iEpClockTick   = 0;      // tick the timer started on: leaving the start zone
int   g_iEpTakeoffTick = 0;      // tick the wind-up jumped
bool  g_bEpClockOn     = false;
float g_fStartSpeedCap = 475.0;  // combined speed kept on leaving the start zone, as on the server
float g_fLineJitter    = 0.0;    // training: how far the line shown to the policy may be shifted sideways
float g_fEpTakeoffSpeed = 0.0;
float g_fEpJumpInset   = 0.0;    // for the eval printout: how far inside the zone the jump was
float g_fEpFailSpeed   = 0.0;    // where a wind-up ended: its speed, and how far from the route
float g_fEpEndDist     = 600.0;

// How a wind-up ended. The recorded runs jump 37 to 53 units inside the start
// zone, spend 43 to 49 ticks in the air there turning 170 to 230 degrees, and
// leave the zone falling.
#define TAKEOFF_NONE    0
#define TAKEOFF_JUMP    1
#define TAKEOFF_EDGE    2      // walked to the zone edge on the ground
#define TAKEOFF_DROP    3      // left the ground without jumping
#define TAKEOFF_LIMIT   4      // never jumped
#define TAKEOFF_LANDED  5      // came down again inside the zone
#define TAKEOFF_AIRLONG 6      // stayed in the air in the zone too long
#define TAKEOFF_EXIT    7      // left the zone: handed over
int   g_iEpTakeoff     = TAKEOFF_NONE;
float g_fJumpInset     = 16.0;   // how far inside the zone edge the jump must be
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
		return;
	}
	int head[4];
	head[0] = BATCH_MAGIC;
	head[1] = BATCH_VERSION;
	head[2] = EP_REC_FLOATS;
	head[3] = OBS_DIM;
	g_hBatchFile.Write(head, 4, 4);
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
	for (int i = 0; i < OBS_DIM; i++)
		row[9 + i] = g_fEpPendObs[i];
	row[9 + OBS_DIM] = view_as<float>(g_iEpPendMask);
	g_hEpBuf.PushArray(row, EP_REC_FLOATS);
	g_fEpReturn += g_fEpStepReward;
}

/** Remembers the decision in flight so it can be written once its reward is known. */
stock void Ep_Pend(const float obs[OBS_DIM], int mask, int act, float logp, const float pos[3], const float vel[3])
{
	for (int i = 0; i < OBS_DIM; i++)
		g_fEpPendObs[i] = obs[i];
	g_iEpPendMask = mask;
	g_iEpAction = act;
	g_fEpLogp = logp;
	for (int k = 0; k < 3; k++)
	{
		g_fEpPendPos[k] = pos[k];
		g_fEpPendVel[k] = vel[k];
	}
	g_iEpSteps++;
}

// ---------------------------------------------------------------- hands ----

stock float Ep_WrapAngle(float a)
{
	while (a >  180.0) a -= 360.0;
	while (a < -180.0) a += 360.0;
	return a;
}

/** Changes the held key, restarting the count of how long it has been held. */
stock void Ep_SetKey(int side)
{
	if (side != g_iKeySide)
	{
		g_iKeySide = side;
		g_iKeyHeld = 0;
	}
}

/** Changes the key the policy asks for, restarting the count of how long it has been asked. */
stock void Ep_SetCmd(int side)
{
	if (side != g_iCmdSide)
	{
		g_iCmdSide = side;
		g_iCmdHeld = 0;
	}
}

/** Moves the mouse one tick towards a commanded speed, easing in within the acceleration limit. */
stock void Ep_MouseToward(float target, float acc, float wmax)
{
	float dw = (target - g_fMouseW) * MOUSE_EASE;
	if (dw >  acc) dw =  acc;
	if (dw < -acc) dw = -acc;
	g_fMouseW += dw;
	if (g_fMouseW >  wmax) g_fMouseW =  wmax;
	if (g_fMouseW < -wmax) g_fMouseW = -wmax;
	g_fMouseYaw = Ep_WrapAngle(g_fMouseYaw + g_fMouseW);
}

/** Run: moves the mouse one tick to follow an aim that itself moves with the velocity. */
stock void Ep_MouseAim(float aimYaw, float acc, float wmax)
{
	float aimRate = g_bMouseAimSet ? Ep_WrapAngle(aimYaw - g_fMouseAim) : 0.0;
	g_fMouseAim = aimYaw;
	g_bMouseAimSet = true;
	float e = Ep_WrapAngle(aimYaw - g_fMouseYaw);
	Ep_MouseToward(aimRate + MOUSE_AIM * e, acc, wmax);
}

/** The key the hand presses for a commanded side: lifted while the mouse still turns the other way. */
stock int Ep_HandKey(int cmd)
{
	if (cmd != 0 && g_fMouseW * float(cmd) < -0.5)
		return 0;
	return cmd;
}

stock void Ep_UpdatePitch(const float vel[3])
{
	if (g_iPitchMode == 1)
	{
		float speed = SquareRoot(vel[0] * vel[0] + vel[1] * vel[1]);
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
		g_fEpPitch = g_fPitchFixed;
	else
		g_fEpPitch = 0.0;
}

/** The policy's own state, appended to the observation. */
stock void Ep_FillExtra(int client, const float pos[3], const float vel[3], float obs[OBS_DIM])
{
	float speed = SquareRoot(vel[0] * vel[0] + vel[1] * vel[1]);
	float off = 0.0;
	if (speed > 1.0)
		off = DegToRad(Ep_WrapAngle(g_fMouseYaw - RadToDeg(ArcTangent2(vel[1], vel[0]))));

	obs[OBS_EXTRA + 0] = g_fMouseW / 7.0;
	obs[OBS_EXTRA + 1] = Sine(off);
	obs[OBS_EXTRA + 2] = Cosine(off);
	obs[OBS_EXTRA + 3] = float(g_iCmdSide);
	obs[OBS_EXTRA + 4] = float((g_iCmdHeld < 48) ? g_iCmdHeld : 48) / 48.0;
	obs[OBS_EXTRA + 5] = (GetEntityFlags(client) & FL_ONGROUND) ? 1.0 : 0.0;
	float edge = 0.0, air = 0.0;
	if (!g_bEpClockOn && g_bZoneLoaded)
	{
		edge = Zone_EdgeDistance(pos) / 256.0;
		if (edge >  1.0) edge =  1.0;
		if (edge < -1.0) edge = -1.0;
		air = float(g_iWindupAirTicks) / 50.0;
		if (air > 2.0) air = 2.0;
	}
	obs[OBS_EXTRA + 6] = edge;
	obs[OBS_EXTRA + 7] = air;
}

/** Everything the policy sees at a decision: the route, the traced surroundings and its own state. */
stock void Ep_BuildObservation(int client, const float pos[3], const float vel[3], float obs[OBS_DIM])
{
	Track_BuildObs(pos, vel, g_iTrackHint, obs);
	float probe[PROBE_DIM];
	Track_BuildProbe(client, pos, vel, probe);
	for (int pi = 0; pi < PROBE_DIM; pi++)
		obs[7 + 3 * LOOKAHEAD + pi] = probe[pi];
	Ep_FillExtra(client, pos, vel, obs);
}

// --------------------------------------------------------------- wind-up ----

stock float Ep_GestureLevel(bool air, int level)
{
	static const float kGround[POL_TRIMS] = { 0.0, 0.4, 0.8, 1.2, 1.6, 2.0, 2.6, 3.2 };
	static const float kAir[POL_TRIMS]    = { 0.0, 1.5, 2.5, 3.5, 4.5, 5.2, 6.0, 6.8 };
	if (level < 0) level = 0;
	if (level >= POL_TRIMS) level = POL_TRIMS - 1;
	return air ? kAir[level] : kGround[level];
}

/** Actions the wind-up may take now: a requested side is kept a minimum time. */
stock int Ep_WindupMask()
{
	int mask = POL_MASK_ALL;
	if (g_iWindupReqHeld < g_iWindupHold)
	{
		int side = g_iWindupReqSide;
		for (int a = 0; a < 2 * POL_TRIMS; a++)
			if (((a < POL_TRIMS) ? 1 : -1) != side)
				mask &= ~(1 << a);
	}
	return mask;
}

/** Starts the wind-up from standing: view as spawned, still mouse, no key. */
stock void Ep_WindupInit(bool learn)
{
	float ang[3];
	GetClientEyeAngles(g_iBot, ang);
	g_fEpPitch = ang[0];
	g_fMouseYaw = ang[1];
	g_fMouseW = 0.0;
	g_iKeySide = 0;
	g_iKeyHeld = 0;
	g_iCmdSide = 0;
	g_iCmdHeld = 0;
	g_iWindupReqSide = 1;
	g_iWindupReqHeld = g_iWindupHold;      // free to pick either side first
	g_fWindupTarget = 0.0;
	g_bWindupCoast = false;
	g_bWindupJumped = false;
	g_iWindupAirTicks = 0;
	g_iPhaseTicks = 0;
	g_bWindupLearn = learn;
}

/**
 * One wind-up tick. Returns TAKEOFF_NONE while the wind-up keeps driving,
 * TAKEOFF_EXIT when the bot leaves the start zone (the handover), or the reason
 * it failed. Sets the controls for this tick when it keeps driving.
 */
stock int Ep_WindupTick(int client, const float pos[3], const float vel[3],
                        int &buttons, float movement[3], float angles[3])
{
	bool onGround = (GetEntityFlags(client) & FL_ONGROUND) != 0;
	bool air = g_bWindupJumped && !onGround;

	// Checks on the state this tick, before deciding.
	if (!g_bWindupJumped)
	{
		if (!onGround)
			return TAKEOFF_DROP;
		if (Zone_EdgeDistance(pos) < g_fJumpInset)
			return TAKEOFF_EDGE;
		if (g_iPhaseTicks >= g_iWindupMax)
			return TAKEOFF_LIMIT;
	}
	else
	{
		if (!g_bZoneLoaded || !Zone_Touching(pos))
			return TAKEOFF_EXIT;
		if (onGround && g_iWindupAirTicks > 4)
			return TAKEOFF_LANDED;
		if (g_iWindupAirTicks >= g_iWindupAirMax)
			return TAKEOFF_AIRLONG;
	}

	bool jumpNow = false;
	if ((g_iPhaseTicks % g_iFrameSkip) == 0)
	{
		if (g_bWindupLearn)
		{
			if (g_iEpSteps > 0)
				Ep_PushStep();
			g_fEpStepReward = 0.0;
		}

		float obs[OBS_DIM];
		Ep_BuildObservation(client, pos, vel, obs);
		int mask = Ep_WindupMask();
		float logp;
		int act = Pol_Act(obs, logp, g_bEval && g_bEvalGreedy,
		                  g_bWindupLearn ? NET_SELF : NET_PARTNER, mask);

		if (act == POL_COAST)
		{
			if (!g_bWindupJumped)
				jumpNow = true;                 // the jump keeps the current gesture
			else
			{
				g_bWindupCoast = true;          // hands off the keys, mouse settles
				g_fWindupTarget = 0.0;
			}
		}
		else
		{
			int side = (act < POL_TRIMS) ? 1 : -1;
			if (side != g_iWindupReqSide)
			{
				g_iWindupReqSide = side;
				g_iWindupReqHeld = 0;
			}
			g_bWindupCoast = false;
			// The key decides the turn: A (side +1) turns left, D turns right.
			g_fWindupTarget = float(side) * Ep_GestureLevel(air || jumpNow, act % POL_TRIMS);
		}

		if (g_bWindupLearn)
			Ep_Pend(obs, mask, act, logp, pos, vel);
	}

	if (jumpNow)
	{
		g_bWindupJumped = true;
		g_iEpTakeoffTick = g_iEpTick;
		g_fEpTakeoffSpeed = SquareRoot(vel[0] * vel[0] + vel[1] * vel[1]);
	}
	if (g_bWindupJumped)
		g_iWindupAirTicks++;

	// The mouse moves with human acceleration towards the commanded speed.
	bool inAir = g_bWindupJumped;
	Ep_MouseToward(g_fWindupTarget, inAir ? g_fWindupAcc : g_fGroundAcc,
	                                inAir ? g_fMouseMax : g_fGroundMax);

	// The key is the gesture's side, lifted while the mouse is still turning the
	// other way, as a person does on a reversal.
	Ep_SetCmd(g_bWindupCoast ? 0 : g_iWindupReqSide);
	int key = Ep_HandKey(g_iCmdSide);
	Ep_SetKey(key);

	if (inAir)
		Ep_UpdatePitch(vel);
	angles[0] = g_fEpPitch;
	angles[1] = g_fMouseYaw;
	angles[2] = 0.0;
	movement[0] = inAir ? 0.0 : 450.0;
	movement[1] = (key > 0) ? -450.0 : ((key < 0) ? 450.0 : 0.0);
	movement[2] = 0.0;
	buttons = (key > 0) ? IN_MOVELEFT : ((key < 0) ? IN_MOVERIGHT : 0);
	if (!inAir)
		buttons |= IN_FORWARD;
	if (jumpNow)
		buttons = (buttons & ~IN_FORWARD) | IN_JUMP;
	else if (inAir)
		buttons |= IN_JUMP | IN_DUCK;

	g_iWindupReqHeld++;
	g_iKeyHeld++;
	g_iCmdHeld++;
	g_iPhaseTicks++;
	return TAKEOFF_NONE;
}

// ------------------------------------------------------------------- run ----

/** Actions the run policy may take now: a key stays down, and stays up, a minimum time. */
stock int Ep_RunMask()
{
	int mask = POL_MASK_ALL;
	if (g_iCmdSide != 0 && g_iCmdHeld < g_iMinPress)
	{
		mask &= ~(1 << POL_COAST);
		for (int a = 0; a < 2 * POL_TRIMS; a++)
			if (((a < POL_TRIMS) ? 1 : -1) != g_iCmdSide)
				mask &= ~(1 << a);
	}
	else if (g_iCmdSide == 0 && g_iCmdHeld < g_iMinCoast)
		mask = (1 << POL_COAST);
	return mask;
}

stock void Ep_ApplyAction(const float vel[3], int &buttons, float movement[3], float angles[3])
{
	float speed  = SquareRoot(vel[0] * vel[0] + vel[1] * vel[1]);
	float velYaw = (speed > 1.0) ? RadToDeg(ArcTangent2(vel[1], vel[0])) : g_fMouseYaw;

	bool useLeft = (g_fEpWishAngle > 0.0);
	float aim = Ep_WrapAngle(velYaw + g_fEpWishAngle + (useLeft ? -90.0 : 90.0));

	// The view follows the aim through a mouse with human acceleration; with the
	// mouse model off it is set directly.
	if (g_fMouseAcc > 0.0)
		Ep_MouseAim(aim, g_fMouseAcc, g_fMouseMax);
	else
	{
		g_fMouseW = Ep_WrapAngle(aim - g_fMouseYaw);
		g_fMouseYaw = aim;
	}
	Ep_UpdatePitch(vel);

	Ep_SetCmd(g_bEpCoast ? 0 : (useLeft ? 1 : -1));
	int key = (g_fMouseAcc > 0.0) ? Ep_HandKey(g_iCmdSide) : g_iCmdSide;
	Ep_SetKey(key);
	g_iKeyHeld++;
	g_iCmdHeld++;

	angles[0] = g_fEpPitch;
	angles[1] = g_fMouseYaw;
	angles[2] = 0.0;

	movement[0] = 0.0;
	movement[1] = (key > 0) ? -450.0 : ((key < 0) ? 450.0 : 0.0);
	movement[2] = 0.0;

	buttons = IN_JUMP;
	if (key > 0) buttons |= IN_MOVELEFT;
	if (key < 0) buttons |= IN_MOVERIGHT;
}

// --------------------------------------------------------------- scoring ----

/** Terminal reward for a wind-up: how the whole run it led into went. */
stock float Ep_WindupScore(int outcome)
{
	// How close to the route the wind-up ended, so that even a failure, or a
	// run that ends at once, points towards the side of the zone the route
	// leaves from.
	float near = 1.0 - g_fEpEndDist / 600.0;
	if (near < 0.0) near = 0.0;
	if (near > 1.0) near = 1.0;
	if (!g_bEpFollow)
	{
		// A little for speed built too. Always below leaving the zone (-20).
		float fast = g_fEpFailSpeed / 475.0;
		if (fast > 1.0) fast = 1.0;
		if (g_iEpTakeoff == TAKEOFF_LANDED || g_iEpTakeoff == TAKEOFF_AIRLONG)
			return -25.0 + 2.0 * fast + 2.0 * near;
		return -30.0 + 2.0 * fast + 2.0 * near;   // never took off properly
	}

	float secs = float(g_iEpTick - g_iEpClockTick) * GetTickInterval();
	if (outcome == EP_FINISHED && g_bEpClockOn && g_fRefTime > 0.0)
	{
		// +10 at the reference time, higher when faster, always above -10, and
		// about 0.4 per tenth of a second at 1-2 s slower: never flat.
		return -10.0 + 20.0 * Exponential(-(secs - g_fRefTime) / 2.0);
	}
	float frac = (g_fTrackLength > 0.0) ? (g_fEpBestS - g_fEpStartS) / g_fTrackLength : 0.0;
	return -20.0 + 9.0 * frac + 1.0 * near;
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
			                               "left the ground without jumping", "never jumped",
			                               "came down inside the zone", "stayed in the air too long", "" };
			if (g_bEpFollow)
				PrintToServer("[CsAI]  wind-up: %d ground ticks, jumped at %.0f u/s %.0f units inside, %d ticks in the air in the zone, run %s in %.2fs, score %.1f",
				              g_iEpTakeoffTick, g_fEpTakeoffSpeed, g_fEpJumpInset, g_iWindupAirTicks, kRun[outcome],
				              float(g_iEpTick - g_iEpClockTick) * GetTickInterval(), terminal);
			else
				PrintToServer("[CsAI]  wind-up: %d ticks, %s, score %.1f",
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
		int header[4 + LINE_PARAMS];
		header[0] = n;
		header[1] = outcome;
		header[2] = g_iEpStartState;
		header[3] = view_as<int>(g_fEpBestS);      // raw float bits
		for (int i = 0; i < LINE_PARAMS; i++)
			header[4 + i] = view_as<int>(g_fLineOff[i]);
		g_hBatchFile.Write(header, 4 + LINE_PARAMS, 4);

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

/** A random slow wave for the line the policy sees this episode; none in evals and wind-up training. */
stock void Ep_PickLineOffset()
{
	Track_ClearLineOffset();
	if (g_fLineJitter <= 0.0 || g_bEval || g_iWindupTicks > 0 || Pol_Rand() < 0.2)
		return;
	float a = g_fLineJitter;
	g_fLineOff[0] = (Pol_Rand() - 0.5) * a;
	g_fLineOff[1] = Pol_Rand() * a;
	g_fLineOff[2] = 2000.0 + Pol_Rand() * 8000.0;
	g_fLineOff[3] = Pol_Rand() * 6.2831853;
	g_fLineOff[4] = (Pol_Rand() - 0.5) * a * 0.5;
	g_fLineOff[5] = Pol_Rand() * a * 0.5;
	g_fLineOff[6] = 2000.0 + Pol_Rand() * 8000.0;
	g_fLineOff[7] = Pol_Rand() * 6.2831853;
}

stock void Ep_Begin()
{
	Ep_Init();
	g_hEpBuf.Clear();
	Ep_PickLineOffset();
	// The parity check rebuilds the observation, so it needs the shift too.
	if (g_iObsDump > 0 && g_hObsDumpFile != null)
		g_hObsDumpFile.WriteLine("# line %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f",
		                         g_fLineOff[0], g_fLineOff[1], g_fLineOff[2], g_fLineOff[3],
		                         g_fLineOff[4], g_fLineOff[5], g_fLineOff[6], g_fLineOff[7]);

	g_iEpStartState = Ep_PickStartState();
	if (g_iEpStartState != 0)
		g_iStatMidStart++;
	UnfreezeBot();
	g_bEpLearnedOpen = false;
	g_fEpJumpInset = 0.0;

	// Wind-up slot: from standing, always at the map start.
	if (g_iWindupTicks > 0)
	{
		g_iEpStartState = 0;
		Pre_Spawn(g_iBot);
		Ep_Reset();
		Ep_WindupInit(true);
		g_bEpGround = true;
		return;
	}

	// Main slot: open with the learned wind-up, or with a recorded one.
	if (g_iEpStartState == 0 && g_bPartnerLoaded &&
	    (g_bEval ? g_bEvalLearned : (Pol_Rand() < g_fLearnedMix)) && Pre_Spawn(g_iBot))
	{
		Ep_Reset();
		Ep_WindupInit(false);
		g_bEpLearnedOpen = true;
		return;
	}

	if (g_iEpStartState == 0 && Pre_Begin(g_iBot))
		return;

	ApplyStateQuiet(g_iBot, g_iEpStartState);
	Ep_Reset();
}

/** Controller state the run policy starts from: it takes over the hands as they are. */
stock void Ep_ResetControl()
{
	g_iEpTrim = 0;
	if (g_iCmdSide != 0)
		g_iEpSide = g_iCmdSide;
	else if (g_iEpSide == 0)
		g_iEpSide = 1;
	g_bEpCoast = (g_iCmdSide == 0);
	g_bMouseAimSet = false;
	g_fEpWishAngle = Pol_WishAngle(g_iEpSide, g_iEpTrim);
	g_iPhaseTicks = 0;
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
	g_iEpTakeoffTick = 0;
	g_bEpClockOn    = !Zone_Touching(pos);    // mid-map starts are already on the clock
	g_bEpGround     = false;
	g_bEpFollow     = false;
	g_iEpTakeoff    = TAKEOFF_NONE;
	g_fEpStepReward = 0.0;

	// A fresh start takes the view it was placed with and no key held.
	float ang[3];
	GetClientEyeAngles(g_iBot, ang);
	g_fEpPitch = ang[0];
	g_fMouseYaw = ang[1];
	g_fMouseW = 0.0;
	g_iKeySide = 0;
	g_iKeyHeld = g_iMinCoast;
	g_iCmdSide = 0;
	g_iCmdHeld = g_iMinCoast;
	g_iEpSide = 1;
	Ep_ResetControl();
	g_bEpCoast = false;
}

/** The wind-up has left the zone (or failed in the main slot): the run policy takes the hands as they are. */
stock void Ep_Handover()
{
	Ep_ResetControl();
}

stock bool Ep_Tick(int client, int &buttons, float movement[3], float angles[3])
{
	// Main slot, learned wind-up: the partner policy drives until the bot leaves
	// the start zone, unrecorded. The run policy's episode starts there.
	if (g_bEpLearnedOpen)
	{
		float p0[3], v0[3];
		GetClientAbsOrigin(client, p0);
		GetEntPropVector(client, Prop_Data, "m_vecVelocity", v0);
		g_iTrackHint = Track_Nearest(p0, g_iTrackHint);
		if (!g_bWindupJumped)
			g_fEpJumpInset = Zone_EdgeDistance(p0);
		int why = Ep_WindupTick(client, p0, v0, buttons, movement, angles);
		if (why == TAKEOFF_NONE)
		{
			if (g_bEval)
			{
				Rec_Tick(client, buttons, angles);
				g_iRecPreFrames++;
			}
			g_iEpTick++;
			return true;
		}
		g_bEpLearnedOpen = false;

		// Keep the hands and the view as they are across the reset.
		float yaw = g_fMouseYaw, w = g_fMouseW, pitch = g_fEpPitch;
		int key = g_iKeySide, held = g_iKeyHeld, cmd = g_iCmdSide, cmdHeld = g_iCmdHeld;
		float takeoff = g_fEpTakeoffSpeed;
		Ep_Reset();                      // the run's baseline starts here
		g_fMouseYaw = yaw; g_fMouseW = w; g_iKeySide = key; g_iKeyHeld = held;
		g_iCmdSide = cmd; g_iCmdHeld = cmdHeld;
		g_fEpTakeoffSpeed = takeoff;
		Ep_Handover();
		g_fEpPitch = pitch;
		g_bEpClockOn = false;            // started below, with the speed cap
	}

	if (g_bPreActive)
	{
		if (Pre_Tick(buttons, movement, angles))
		{
			// Recorded for the replay; not a training step.
			if (g_bEval)
			{
				Rec_Tick(client, buttons, angles);
				g_iRecPreFrames++;
			}
			return true;
		}
		Ep_Reset();                      // handover: the baseline starts here
		g_bEpClockOn = false;
	}

	float pos[3], vel[3];
	GetClientAbsOrigin(client, pos);
	GetEntPropVector(client, Prop_Data, "m_vecVelocity", vel);

	// The timer starts on leaving the start zone, and the server caps the combined
	// speed kept at that moment. shavit ignores bots, so both are applied here.
	if (!g_bEpClockOn && !g_bEpGround && !Zone_Touching(pos))
		Ep_StartClock(client, vel);

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

	// Wind-up slot: the policy being trained winds up and flies the zone.
	if (g_bEpGround)
	{
		if (!g_bWindupJumped)
			g_fEpJumpInset = Zone_EdgeDistance(pos);
		int why = Ep_WindupTick(client, pos, vel, buttons, movement, angles);
		if (why == TAKEOFF_NONE)
		{
			if (g_bEval)
			{
				Rec_Tick(client, buttons, angles);
				g_iRecPreFrames++;
			}
			g_iEpTick++;
			return true;
		}
		g_iEpTakeoff = why;
		g_fEpFailSpeed = SquareRoot(vel[0] * vel[0] + vel[1] * vel[1]);
		g_fEpEndDist = dist;
		if (why != TAKEOFF_EXIT)
		{
			Ep_End(EP_STUCK);
			return false;
		}

		// Out of the zone: bank the last wind-up decision, then the main policy
		// flies the rest frozen, so the wind-up is judged on the run it sets up.
		if (g_iEpSteps > 0)
			Ep_PushStep();
		g_bEpGround = false;
		g_bEpFollow = true;
		g_iEpNoProgress = 0;
		if (!g_bEpClockOn)
			Ep_StartClock(client, vel);
		Ep_Handover();
	}

	if ((g_iPhaseTicks % g_iFrameSkip) == 0)
	{
		if (!g_bEpFollow)
		{
			if (g_iEpSteps > 0)
				Ep_PushStep();             // bank the previous decision
			g_fEpStepReward = 0.0;         // open this decision's accumulator
		}

		float obs[OBS_DIM];
		Ep_BuildObservation(client, pos, vel, obs);

		if (g_iObsDump > 0 && g_hObsDumpFile != null)
		{
			char line[1400];
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

		int mask = Ep_RunMask();
		float logp;
		int act = g_bEpFollow ? Pol_Act(obs, logp, true, NET_PARTNER, mask)
		                      : Pol_Act(obs, logp, g_bEval && g_bEvalGreedy, NET_SELF, mask);

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
		g_fEpWishAngle = Ep_WrapAngle(g_fEpWishAngle);

		if (!g_bEpFollow)
		{
			Ep_Pend(obs, mask, act, logp, pos, vel);
			if (g_bEpClockOn)
				g_fEpStepReward -= g_fTimeCost;
		}
	}

	Ep_ApplyAction(vel, buttons, movement, angles);

	if (g_bEval)
	{
		Rec_Tick(client, buttons, angles);
		if (!g_bEpClockOn)
			g_iRecPreFrames++;
	}

	g_iPhaseTicks++;
	g_iEpTick++;
	return true;
}

/** The timer starts: note the tick and cap the combined speed as the server does. */
stock void Ep_StartClock(int client, float vel[3])
{
	g_bEpClockOn = true;
	g_iEpClockTick = g_iEpTick;
	g_iEpPreTicks = g_iEpTick;
	float sp = GetVectorLength(vel);
	if (g_fStartSpeedCap > 0.0 && sp > g_fStartSpeedCap)
	{
		ScaleVector(vel, g_fStartSpeedCap / sp);
		TeleportEntity(client, NULL_VECTOR, NULL_VECTOR, vel);
	}
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
	if (g_iEpStuckTicks > g_iEpStuckMax || g_iEpNoProgress > g_iEpNoProgMax)
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
