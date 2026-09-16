/**
 * csai_prestrafe - replay the recorded pre-timer inputs before handing control
 * to the policy.
 *
 * A surf run begins with a prestrafe: W plus a strafe key while turning, which
 * builds speed above the ground cap before you drop onto the first ramp.
 * Measured on the reference run: 67 ticks, 0 -> 286 u/s, buttons 1032
 * (IN_MOVERIGHT | IN_FORWARD).
 *
 * The policy cannot produce that. Its action space is the air-strafe angle with
 * no forward movement, by design - that choice is what took the usable-control
 * fraction from 2.1% to 100% at speed. Adding W back to serve one second at the
 * start would widen the space everywhere for no benefit in the air.
 *
 * So the prestrafe is replayed from the human record through real physics: the
 * bot is teleported to the recorded start, the recorded inputs are fed tick by
 * tick, and the policy takes over at the handover point with whatever state the
 * physics produced. Nothing about the prestrafe enters the training signal - the
 * episode's progress baseline is set at handover, not before.
 */

#if defined _csai_prestrafe_included
	#endinput
#endif
#define _csai_prestrafe_included

#define PRE_MAX_TICKS 512

float g_fPreAng[PRE_MAX_TICKS][2];
int   g_iPreButtons[PRE_MAX_TICKS];
float g_fPreStart[3];
float g_fPreStartAng[3];
int   g_iPreCount   = 0;

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
		if (line[0] == '\0' || line[0] == '#')
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

/** Put the bot at the recorded start, stationary, ready to replay. */
stock bool Pre_Begin(int client)
{
	if (!g_bPrestrafeEnabled || g_iPreCount == 0 || client == -1 || !IsClientInGame(client))
		return false;

	float zero[3];
	zero[0] = 0.0; zero[1] = 0.0; zero[2] = 0.0;
	TeleportEntity(client, g_fPreStart, g_fPreStartAng, zero);

	g_bPreActive = true;
	g_iPreIndex = 0;
	return true;
}

/**
 * Feed one recorded tick. Returns false once the record is exhausted, which is
 * the handover moment.
 *
 * The record stores buttons, not the analogue move values, so forwardmove and
 * sidemove are synthesised from the keys at the standard 450 - which is what the
 * client would have sent.
 */
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
