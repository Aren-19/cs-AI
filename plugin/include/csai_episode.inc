/**
 * csai_episode - episode lifecycle, reward, termination, trajectory logging.
 *
 * Reward is progress along the centerline, per agent step. That is dense: every
 * step gets a signal. A finish-only bonus over a 2600-tick episode would be
 * unlearnable.
 *
 * Episodes start from replay-derived checkpoints, not always from the map start.
 * Sampling start states across the whole route means the agent gets gradient on
 * the last third of the map without first having to solve the first two thirds -
 * this is the single biggest reason long maps become learnable. Surf state is
 * just origin+angles+velocity, so these restarts are free.
 *
 * Trajectories are written as raw float32 so the hot loop does no formatting.
 * Only (pos, vel, action, logp, reward) is logged - the observation is
 * recomputed in Python from pos/vel, which keeps the record small and lets the
 * observation definition change without re-collecting data.
 */

#if defined _csai_episode_included
	#endinput
#endif
#define _csai_episode_included

// px py pz vx vy vz action logp reward + PROBE_DIM probe features.
// The probe needs engine collision, so unlike the centerline features it cannot
// be rebuilt in Python - it has to travel with the record.
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
/**
 * Take the argmax at evaluation instead of sampling.
 *
 * Worth being able to turn off. A discrete action space that approximates a
 * continuous steering angle can realise an intermediate angle by dithering
 * between two adjacent trims; the argmax of such a policy collapses the mix to
 * one end of it and is NOT the same controller. If greedy scores far below the
 * sampled policy on identical episodes, that is the explanation to check first.
 */
bool  g_bEvalGreedy    = true;
bool  g_bScripted      = false;   // hand-coded controller, for interface testing
int   g_iFrameSkip     = 2;
float g_fMaxDeviation  = 600.0;    // units off the centerline before we call it a fall

/**
 * Price on being off the reference line, charged every decision as
 * cost * (dist/maxDeviation)^2.
 *
 * Without it, progress reward is the only signal and drifting 590 units off the
 * route costs exactly nothing - then at 600 the episode dies with no warning.
 * A cliff with no slope leading to it cannot be learned from.
 *
 * Measured at the wall this was blocking: through 50-70% of surf_demise the
 * human holds 0 deviation, while the bot wobbles 300-400 off the line and sinks
 * steadily below it (z 4384 against the human's 4630 at 58%, 3626 against 4204
 * at 60%) until it slides off the bottom of the ramp. Every recent run ended
 * within a unit or two of the 600 limit, at 59-60% of the map.
 *
 * Quadratic so that small, normal wobble is nearly free and only real drift
 * bites.
 */
float g_fDeviationCost = 0.5;

/**
 * Where the bot looks vertically.
 *
 * Source zeroes the z component of the forward and right vectors and renormalises
 * them before building wishdir, so pitch does not enter the movement maths at
 * all. It was therefore left at 0 - and the bot spent every run staring dead
 * level, which no player does and which makes the replays look wrong however
 * good the line is.
 *
 * Fitted to the 8 recorded human runs. They track their own velocity direction
 * at about 57% gain with a 6 degree downward bias (correlation 0.69), and they
 * move the mouse very smoothly: 0.08 deg per tick median, 0.30 at p95. A
 * first-order lag of 0.05 with a 0.30 deg/tick cap reproduces that (0.10 median,
 * 0.30 p95) - matching how it MOVES matters more for looking human than matching
 * the exact angle.
 *
 * mode 0 = flat (what it did before), 1 = natural, 2 = fixed at g_fPitchFixed
 * (only for checking that pitch really is inert).
 */
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

/**
 * Mixed starts.
 *
 * PPO improves by comparing better outcomes against worse ones. At the drop at
 * 72% of surf_demise there was nothing to compare: best progress sat between
 * 73.02% and 73.07% for a hundred generations - roughly 9600 episodes, not one
 * of which got past. With every episode ending identically there is no gradient
 * to climb, and more training cannot help. The bot was not short of practice at
 * that drop; it was short of a single example of clearing it.
 *
 * So a fraction of episodes begin from a replay checkpoint inside a chosen
 * stretch of the map, where the policy is close enough that random exploration
 * sometimes succeeds. The rest still run the whole map from the start, so the
 * objective stays "finish the run" rather than "advance from anywhere".
 *
 * The window is in the checkpoint file's own units, which are a fraction of the
 * reference run's TIME, not of track distance - checkpoints are spaced evenly in
 * time, 1.70 s apart on this map. The drop sits at 31-32 s of a 39.1 s run, so
 * 0.68-0.79 picks the three checkpoints just before it (27.2 s, 28.9 s, 30.6 s)
 * and the bot flies into the drop carrying its own speed rather than starting
 * inside it.
 *
 * Set the window around whatever the policy cannot pass; 0 mix disables it.
 */
float g_fStateMix      = 0.0;      // share of episodes starting mid-map
float g_fStateLo       = 0.68;     // window to sample those from, as time fraction
float g_fStateHi       = 0.79;

int   g_iEpOutcome     = EP_RUNNING;
int   g_iEpTick        = 0;
int   g_iEpSteps       = 0;
bool  g_bEpCoast       = false;   // this decision presses no strafe key
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
// Cost charged for switching strafe side. Progress reward alone rewards
// jittering +-90 every couple of ticks, because that hugs the reference line
// more tightly than real technique does - measured 19.79 switches/s against a
// human's 0.95, holding each side 2 ticks against 39.5. That is a tick-level
// exploit of the reward, not surfing. Pricing the switch makes holding a line
// the cheaper option, which is what a player actually does.
/**
 * Price on changing strafe key.
 *
 * This was charged and then overwritten by the accumulator reset one line below
 * it, so every result recorded before 2026-09-16 ran at an effective cost of 0.
 *
 * It is needed. Sampling a stochastic policy that is ~90% sure of its side still
 * flips that side on roughly 2*p*(1-p) of decisions - about 16%, or 5/s at this
 * cadence - purely as sampling noise. Measured: clone 3.94/s, gen 57 6.11/s,
 * gen 143 8.35/s, against a human's 0.95/s, i.e. jitter RISING through training
 * while the KL anchor alone failed to hold it.
 *
 * (It was briefly set to 0 on the strength of a replayqa metric that counted
 * side switches as >90 deg view jumps and therefore reported 0.00/s for every
 * replay, the human's included. The metric was wrong, not the policy.)
 */
float g_fSwitchCost    = 0.15;
float g_fStartMaxFrac  = 0.90;     // never start so near the end that we finish instantly

ArrayList g_hEpBuf = null;

// The decision in flight. A step record is written on the NEXT decision
// boundary, once the reward accumulated while holding that action is known.
float g_fEpPendPos[3];
float g_fEpPendVel[3];
float g_fEpPendProbe[PROBE_DIM];
float g_fEpPendWish = 90.0;
float g_fEpWishAngle = 90.0;   // = side * (90 + trim)
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
		PrintToServer("[CsAI] could not open batch file %s", path);

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
	// The mixed-start window is drawn from every loaded checkpoint, not just the
	// first g_iTrainStates of them - with states=1 that list is only the map
	// start, which is exactly the case this exists to help.
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

/**
 * Reference controller for the interface test.
 *
 * The first version steered on horizontal bearing alone and died the same way
 * the learner does: vertical offset swinging +45 -> +417 -> -596 before dropping
 * through the ramp. Horizontal tracking was never the problem.
 *
 * The physics that matters: air acceleration is applied along a HORIZONTAL
 * wishdir, and only its component perpendicular to velocity does anything
 * (cos(phi) must be below wishspd/speed, so at 3600 u/s only |phi| ~ 90 deg
 * accelerates at all). On a ramp the velocity runs along the surface, which puts
 * that perpendicular direction roughly parallel to the ramp's horizontal normal.
 *
 * So the SIGN of +-90 is the climb/descend control: one side pushes into the ramp
 * face and gains height, the other pushes away and loses it. Which sign does
 * which depends on the ramp, so it is resolved against the measured normal
 * rather than assumed.
 */
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

	// wishdir at phi=+90 is velocity rotated left, i.e. the velocity-frame
	// "left" axis. Pushing INTO the ramp means opposing its normal, so +90
	// climbs when the normal's left-component is negative.
	bool leftClimbs = (nleft < 0.0);

	bool wantClimb = (vertErr < -24.0);
	bool wantDrop  = (vertErr >  96.0);

	if (wantClimb)
		return leftClimbs ? 90.0 : -90.0;
	if (wantDrop)
		return leftClimbs ? -90.0 : 90.0;

	// in the band: hold whichever side we were on, so the line stays smooth
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

/**
 * Apply the currently held wish angle. Actions adjust that angle rather than
 * setting it, so this reads g_fEpWishAngle instead of decoding an action.
 *
 * With sidemove-only, wishdir is the view's right vector, so the view yaw is
 * simply (wish yaw + 90). Pitch is irrelevant: AirMove zeroes the z component of
 * forward/right and renormalises before building wishdir.
 */
stock void Ep_ApplyAction(const float vel[3], int &buttons, float movement[3], float angles[3])
{
	float speed  = SquareRoot(vel[0] * vel[0] + vel[1] * vel[1]);
	float velYaw = (speed > 1.0) ? RadToDeg(ArcTangent2(vel[1], vel[0])) : angles[1];

	/**
	 * Choose the strafe KEY from the sign of phi, not a fixed one.
	 *
	 * wishdir is the view's right vector for sidemove>0 and its left vector for
	 * sidemove<0, so:
	 *     right key -> viewyaw = velYaw + phi + 90
	 *     left  key -> viewyaw = velYaw + phi - 90
	 *
	 * With phi = +-(90 + trim), picking the key that matches the sign collapses
	 * both cases to viewyaw ~ velYaw +- trim: the view simply points where the
	 * player is going, and a side switch changes the KEY rather than snapping the
	 * view 180 degrees. That is exactly what a human does - it is why their
	 * measured view-flip rate is 0.00/s while holding phi at +-90 - and it makes
	 * the recorded replay watchable instead of a spinning blur.
	 */
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

	// Hold jump for the whole run, as a human does - measured 100% of ticks in the
	// reference run. With sv_enablebunnyhopping 1 this means clipping a ramp edge
	// auto-hops instead of sticking to the surface and dumping all momentum. It
	// costs nothing, needs no action, and there is no situation in surf where you
	// want it released.
	buttons = g_bEpCoast ? IN_JUMP : ((useLeft ? IN_MOVELEFT : IN_MOVERIGHT) | IN_JUMP);
}

stock void Ep_Finish(int outcome)
{
	g_iEpOutcome = outcome;

	// terminal reward folded into the last recorded step
	float terminal = 0.0;
	if (outcome == EP_FINISHED)
		terminal = 10.0;
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

	// Starting at the map start means replaying the recorded prestrafe first:
	// W + strafe while turning, which the policy's action space cannot express.
	// Ep_Reset below runs at the handover, so nothing before it is trained on.
	if (g_iEpStartState == 0 && Pre_Begin(g_iBot))
		return;

	ApplyStateQuiet(g_iBot, g_iEpStartState);
	Ep_Reset();
}

/** Establish the episode baseline from wherever the bot currently is. */
stock void Ep_Reset()
{

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
	g_fEpStepReward = 0.0;
	// start from the speed-optimal perpendicular angle rather than an arbitrary one
	g_iEpSide = 1;
	g_iEpTrim = 0;
	g_fEpWishAngle = Pol_WishAngle(g_iEpSide, g_iEpTrim);
	g_fEpPendWish = g_fEpWishAngle;
}

/**
 * One physics tick of a training episode. Called from OnPlayerRunCmd, so this is
 * the hot path: it runs up to ~5400 times per second per server.
 * Returns false when the episode has ended.
 */
stock bool Ep_Tick(int client, int &buttons, float movement[3], float angles[3])
{
	if (g_bPreActive)
	{
		if (Pre_Tick(buttons, movement, angles))
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
	else
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

		// Parity check against tools/rollout.py: the learner recomputes these from
		// (pos, vel), and if the two ever disagree PPO optimises against a
		// distribution the actor never used - with nothing to report the error.
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
			// (scripted path below)
			// Hand-coded reference controller. Not used in training - it exists to
			// answer whether the observation and action space are ADEQUATE at all.
			// If a simple analytic policy cannot beat the learner's plateau, the
			// interface is the problem; if it can, the learning is.
			//
			// Steering: the perpendicular component of wishdir is what turns you,
			// so +90 deg turns left and -90 turns right, both at maximum
			// acceleration. Bang-bang toward the centerline a little way ahead,
			// with a deadband so it does not chatter on the boundary.
			g_fEpWishAngle = Ep_ScriptedAngle(pos, vel);
			g_iEpSide = (g_fEpWishAngle >= 0.0) ? 1 : -1;
		}
		else
		{
			int prevSide = g_iEpSide;
			g_bEpCoast = (g_iEpAction == POL_COAST);
			Pol_ApplyAction(g_iEpAction, g_iEpSide, g_iEpTrim);
			g_fEpWishAngle = Pol_WishAngle(g_iEpSide, g_iEpTrim);
			if (g_iEpSide != prevSide)
				g_fEpStepReward -= g_fSwitchCost;
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
		Rec_Tick(client, buttons);

	g_iEpTick++;

	float speed = SquareRoot(vel[0] * vel[0] + vel[1] * vel[1]);
	if (speed < 40.0 && (GetEntityFlags(client) & FL_ONGROUND))
		g_iEpStuckTicks++;
	else
		g_iEpStuckTicks = 0;

	if (g_fTrackLength > 0.0 && s >= g_fTrackLength - 96.0)
	{
		Ep_PushStep();
		Ep_Finish(EP_FINISHED);
		return false;
	}
	if (dist > g_fMaxDeviation)
	{
		Ep_PushStep();
		Ep_Finish(EP_FELL);
		return false;
	}
	if (g_iEpStuckTicks > 120 || g_iEpNoProgress > g_iEpNoProgMax)
	{
		// Without this, episodes that stop advancing but stay near the line burn
		// the full tick budget. In the first test that was 11 of 32 episodes at
		// 4000 ticks each - more simulation spent on dead runs than live ones.
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
