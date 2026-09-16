/**
 * csai_policy - policy network, evaluated inside the plugin.
 *
 * There is no socket extension on this server (only webternet/HTTP), and per-tick
 * IPC would throttle throughput anyway: at timescale 80 the server runs ~5400
 * ticks/s, so a round trip per tick is not on the table. So the actor runs here
 * and Python is the learner - weights in, trajectories out, at episode
 * granularity over files.
 *
 * Deliberately small. The forward pass is ~2600 multiply-adds; SourcePawn is
 * fast enough for that at full timescale, but not for anything much larger.
 * Hidden layers use ReLU so the hot path contains no transcendental calls -
 * only the 25-wide softmax needs Exponential().
 *
 * Action space is ONE scalar: the angle of the wish vector relative to current
 * velocity. Source air acceleration only applies the component of wishdir
 * perpendicular to velocity, so that angle is the entire control input; the
 * (yaw, strafe key) pair is a decoding step at the end. This collapses 2D
 * continuous control to 1D and is what makes the problem tractable.
 */

#if defined _csai_policy_included
	#endinput
#endif
#define _csai_policy_included

#define FLOAT_E      2.718281828459045

#define POL_H1       32
#define POL_H2       32
#define POL_ACTIONS  16      // 2 sides x 8 trims, chosen absolutely

#define POL_W1_N  (OBS_DIM * POL_H1)
#define POL_W2_N  (POL_H1 * POL_H2)
#define POL_W3_N  (POL_H2 * POL_ACTIONS)
#define POL_TOTAL (POL_W1_N + POL_H1 + POL_W2_N + POL_H2 + POL_W3_N + POL_ACTIONS)

float g_fW1[POL_W1_N];
float g_fB1[POL_H1];
float g_fW2[POL_W2_N];
float g_fB2[POL_H2];
float g_fW3[POL_W3_N];
float g_fB3[POL_ACTIONS];

bool  g_bPolicyLoaded = false;
int   g_iPolicyGen    = -1;      // generation counter from the weights file

// xorshift32: a native RNG call per tick is avoidable overhead, and a local
// generator keeps sampling reproducible from a seed.
int   g_iRngState = 0x1234567;

stock float Pol_Rand()
{
	int x = g_iRngState;
	x ^= (x << 13);
	x ^= (x >>> 17);
	x ^= (x << 5);
	g_iRngState = x;
	return float(x >>> 8) / 16777216.0;      // [0,1)
}

stock void Pol_Seed(int seed)
{
	g_iRngState = (seed == 0) ? 0x1234567 : seed;
}

/** Small random init, so the harness runs before any training has happened. */
stock void Pol_RandomInit()
{
	for (int i = 0; i < POL_W1_N; i++) g_fW1[i] = (Pol_Rand() - 0.5) * 0.5;
	for (int i = 0; i < POL_H1;   i++) g_fB1[i] = 0.0;
	for (int i = 0; i < POL_W2_N; i++) g_fW2[i] = (Pol_Rand() - 0.5) * 0.5;
	for (int i = 0; i < POL_H2;   i++) g_fB2[i] = 0.0;
	// Near-zero head: with a normally-scaled one the untrained policy already
	// commits ~30% of its mass to a single action, which throttles exploration
	// from the first batch. Measured: logp ranged -1.68..-1.19 vs uniform -3.22.
	for (int i = 0; i < POL_W3_N; i++) g_fW3[i] = (Pol_Rand() - 0.5) * 0.02;
	for (int i = 0; i < POL_ACTIONS; i++) g_fB3[i] = 0.0;
	g_bPolicyLoaded = true;
	g_iPolicyGen = 0;
}

/**
 * Weights file: one float per line after a header line carrying the generation.
 * Flat and dumb on purpose - SourcePawn has no JSON, and this is read once per
 * batch, never in the hot path.
 */
stock bool Pol_Load(const char[] path)
{
	if (!FileExists(path))
		return false;

	File f = OpenFile(path, "r");
	if (f == null)
		return false;

	char line[64];
	int gen = -1;
	int n = 0;
	bool ok = true;

	while (!f.EndOfFile() && f.ReadLine(line, sizeof(line)))
	{
		TrimString(line);
		if (line[0] == '\0')
			continue;

		if (line[0] == '#')
		{
			// "# gen <N> dim <OBS_DIM> actions <A> total <T>"
			char parts[10][24];
			int np = ExplodeString(line, " ", parts, sizeof(parts), sizeof(parts[]));
			for (int i = 0; i + 1 < np; i++)
			{
				if (StrEqual(parts[i], "gen"))
					gen = StringToInt(parts[i + 1]);
				else if (StrEqual(parts[i], "total"))
				{
					int t = StringToInt(parts[i + 1]);
					if (t != POL_TOTAL)
					{
						PrintToServer("[CsAI] weights size mismatch: file %d, plugin %d", t, POL_TOTAL);
						ok = false;
					}
				}
			}
			continue;
		}

		float v = StringToFloat(line);
		int i = n;
		if (i < POL_W1_N)                                     { g_fW1[i] = v; }
		else if ((i -= POL_W1_N) < POL_H1)                    { g_fB1[i] = v; }
		else if ((i -= POL_H1) < POL_W2_N)                    { g_fW2[i] = v; }
		else if ((i -= POL_W2_N) < POL_H2)                    { g_fB2[i] = v; }
		else if ((i -= POL_H2) < POL_W3_N)                    { g_fW3[i] = v; }
		else if ((i -= POL_W3_N) < POL_ACTIONS)               { g_fB3[i] = v; }
		n++;
	}
	delete f;

	if (!ok)
		return false;

	if (n != POL_TOTAL)
	{
		PrintToServer("[CsAI] weights: read %d floats, expected %d", n, POL_TOTAL);
		return false;
	}

	g_bPolicyLoaded = true;
	g_iPolicyGen = gen;
	return true;
}

/**
 * Forward pass. Returns the sampled action index and fills `outLogp` with its
 * log-probability, which PPO needs for the importance ratio.
 * `greedy` takes the argmax instead of sampling, for evaluation runs.
 */
stock int Pol_Act(const float obs[OBS_DIM], float &outLogp, bool greedy)
{
	float h1[POL_H1];
	for (int j = 0; j < POL_H1; j++)
	{
		float a = g_fB1[j];
		int base = j * OBS_DIM;
		for (int i = 0; i < OBS_DIM; i++)
			a += g_fW1[base + i] * obs[i];
		h1[j] = (a > 0.0) ? a : 0.0;
	}

	float h2[POL_H2];
	for (int j = 0; j < POL_H2; j++)
	{
		float a = g_fB2[j];
		int base = j * POL_H1;
		for (int i = 0; i < POL_H1; i++)
			a += g_fW2[base + i] * h1[i];
		h2[j] = (a > 0.0) ? a : 0.0;
	}

	float logits[POL_ACTIONS];
	float maxl = -1.0e30;
	for (int j = 0; j < POL_ACTIONS; j++)
	{
		float a = g_fB3[j];
		int base = j * POL_H2;
		for (int i = 0; i < POL_H2; i++)
			a += g_fW3[base + i] * h2[i];
		logits[j] = a;
		if (a > maxl)
			maxl = a;
	}

	float expv[POL_ACTIONS];
	float sum = 0.0;
	for (int j = 0; j < POL_ACTIONS; j++)
	{
		expv[j] = Exponential(logits[j] - maxl);
		sum += expv[j];
	}

	int pick = 0;
	if (greedy)
	{
		float best = logits[0];
		for (int j = 1; j < POL_ACTIONS; j++)
			if (logits[j] > best) { best = logits[j]; pick = j; }
	}
	else
	{
		float r = Pol_Rand() * sum;
		float acc = 0.0;
		pick = POL_ACTIONS - 1;
		for (int j = 0; j < POL_ACTIONS; j++)
		{
			acc += expv[j];
			if (r <= acc) { pick = j; break; }
		}
	}

	float p = expv[pick] / sum;
	if (p < 1.0e-9)
		p = 1.0e-9;

	// SourceMod's Logarithm() defaults to base 10. PPO needs a NATURAL log: the
	// importance ratio is exp(logp_new - logp_old), so mixing bases silently
	// produces nonsense ratios. Symptom when this was wrong: entropy sat at
	// log(25)=3.219 while the reported KL was a constant +1.82, which is exactly
	// ln(1/25) - log10(1/25).
	outLogp = Logarithm(p, FLOAT_E);
	return pick;
}

/**
 * Action space, derived from measured human play rather than assumed.
 *
 * Analysis of the 39.10 s human run on this map (2525 strafing frames):
 *
 *     phi percentiles   p1 -90.54   p50 +89.43   p99 +90.73
 *     |phi| in (85,95)  98.1% of frames
 *
 * The player holds the wish angle at +-90 degrees essentially always. The physics
 * says why: addspeed = wishspd - speed*cos(phi), so at this map's median 3614 u/s
 * any |phi| < 89.5 deg yields addspeed <= 0 and therefore NO acceleration at all,
 * while phi near 180 brakes at up to sv_airaccelerate*maxspeed*frametime = 720
 * u/s in a single tick.
 *
 * Earlier versions offered the full 360 degrees - 25 bins spread over a range
 * where only two points are usable. The agent spent every run exploring
 * directions that either did nothing or destroyed its speed.
 *
 * So: the angle is (side * (90 + trim)). `side` is what makes you climb or
 * descend the ramp (measured: phi>0 averaged -382 u/s vertical, phi<0 -621 u/s),
 * and it is STICKY - changing it takes a deliberate action. That gives temporal
 * coherence by construction, which the absolute parameterisation could not
 * express and the delta ladder only approximated.
 */
#define POL_TRIMS  8

stock float Pol_TrimValue(int i)
{
	// Offsets from 90 degrees, SIGNED - the first version only allowed >= 0, so
	// everything below perpendicular was unreachable. Measured on the human run,
	// median |phi| is 89.43: half their play sits just UNDER 90, which the old
	// ladder could not express at all.
	//
	// Range now spans the human's actual band (89.4 .. 90.7, max 93.18) with fine
	// resolution. The old rungs at 8/15/25 degrees are gone: they put |phi| past
	// 98 where accelspeed saturates at sv_airaccelerate*maxspeed*frametime and the
	// player brakes hard. A human never goes there, and the bot was spending 34.9%
	// of its frames at 93-96 and 11.1% past 95.
	//
	// Sharp turns do not need large |phi|: at 90 degrees the turn rate is already
	// 30/speed rad/tick (~114 deg/s at 1000 u/s). Big direction changes come from
	// flipping side, not from over-rotating.
	static const float kTrim[POL_TRIMS] = { 0.0, 0.25, -0.25, 0.5, -0.5, 1.0, -1.0, 2.0 };
	if (i < 0 || i >= POL_TRIMS)
		return 0.0;
	return kTrim[i];
}

/**
 * Actions are ABSOLUTE: action -> (side, trim) directly.
 *
 * The previous space was transitional - hold / flip side / set trim - chosen to
 * make smoothness the default. It works for PPO but is close to uncloneable:
 * 72% of a human's labels become "hold", so a network scores well while learning
 * nothing about when to act; the labels depend on a tracked state that is our
 * bookkeeping rather than anything the human had; and one mispredicted flip
 * desynchronises every label after it.
 *
 * Measured on the captured human data: predicting which SIDE the player is on,
 * from the observation alone, reaches 95.9% held-out accuracy against a 61.3%
 * majority baseline. The technique is in there - it just has to be asked for as
 * an absolute state.
 *
 * Jitter is then held down by the per-switch cost in the reward rather than by
 * the shape of the action space.
 */
stock void Pol_ApplyAction(int action, int &side, int &trim)
{
	if (action < 0) action = 0;
	if (action >= POL_ACTIONS) action = POL_ACTIONS - 1;
	side = (action < POL_TRIMS) ? 1 : -1;
	trim = action % POL_TRIMS;
}

/** Inverse: the action index that expresses this (side, trim). */
stock int Pol_ActionFor(int side, int trim)
{
	if (trim < 0) trim = 0;
	if (trim >= POL_TRIMS) trim = POL_TRIMS - 1;
	return (side > 0) ? trim : (POL_TRIMS + trim);
}

stock float Pol_WishAngle(int side, int trim)
{
	// trim is a signed offset from perpendicular, so this spans both sides of 90
	return float(side) * (90.0 + Pol_TrimValue(trim));
}
