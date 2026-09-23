#if defined _csai_policy_included
	#endinput
#endif
#define _csai_policy_included

#define FLOAT_E      2.718281828459045

#define POL_H1       32
#define POL_H2       32
#define POL_TRIMS  8
#define POL_COAST    (2 * POL_TRIMS)       // press nothing this decision
#define POL_ACTIONS  (2 * POL_TRIMS + 1)   // 2 sides x trims, then coast

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
	for (int i = 0; i < POL_W3_N; i++) g_fW3[i] = (Pol_Rand() - 0.5) * 0.02;
	for (int i = 0; i < POL_ACTIONS; i++) g_fB3[i] = 0.0;
	g_bPolicyLoaded = true;
	g_iPolicyGen = 0;
}

static float s_fLoadBuf[POL_TOTAL];   // parsed here, committed only if complete

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

		if (n < POL_TOTAL)
			s_fLoadBuf[n] = StringToFloat(line);
		n++;
	}
	delete f;

	if (!ok)
		return false;
	if (n != POL_TOTAL)
	{
		PrintToServer("[CsAI] weights: got %d floats, need %d - ignoring this read", n, POL_TOTAL);
		return false;
	}

	for (int i = 0; i < POL_TOTAL; i++)
	{
		int k = i;
		if (k < POL_W1_N)                          { g_fW1[k] = s_fLoadBuf[i]; continue; }
		k -= POL_W1_N;
		if (k < POL_H1)                            { g_fB1[k] = s_fLoadBuf[i]; continue; }
		k -= POL_H1;
		if (k < POL_W2_N)                          { g_fW2[k] = s_fLoadBuf[i]; continue; }
		k -= POL_W2_N;
		if (k < POL_H2)                            { g_fB2[k] = s_fLoadBuf[i]; continue; }
		k -= POL_H2;
		if (k < POL_W3_N)                          { g_fW3[k] = s_fLoadBuf[i]; continue; }
		k -= POL_W3_N;
		if (k < POL_ACTIONS)                       { g_fB3[k] = s_fLoadBuf[i]; }
	}

	g_bPolicyLoaded = true;
	g_iPolicyGen = gen;
	return true;
}

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

	outLogp = Logarithm(p, FLOAT_E);
	return pick;
}

stock float Pol_TrimValue(int i)
{
	static const float kTrim[POL_TRIMS] = { 0.0, 0.25, -0.25, 0.5, -0.5, 1.0, -1.0, 2.0 };
	if (i < 0 || i >= POL_TRIMS)
		return 0.0;
	return kTrim[i];
}

stock void Pol_ApplyAction(int action, int &side, int &trim)
{
	if (action < 0) action = 0;
	if (action >= POL_ACTIONS) action = POL_ACTIONS - 1;
	// Coasting keeps whatever side was held, so resuming does not read as a
	// strafe-key switch and the side stays meaningful while nothing is pressed.
	if (action == POL_COAST)
		return;
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
