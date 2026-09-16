/**
 * csai_record - record YOUR runs, so the bot has more than one to learn from.
 *
 * The bot copies a human run to pick up technique, and it only ever had one:
 * 1213 decisions, all on a single line. It has never seen a state slightly off
 * that line, which is why it can surf a ramp cleanly and then have no idea how
 * to save itself when it drifts.
 *
 * The timer's own replay file cannot supply more. shavit keeps exactly one
 * replay per map per style and overwrites it only when you beat your time, so a
 * slower or messier run - the kind that is most useful here, because it contains
 * a recovery - is never written at all.
 *
 * So this records independently of the timer. Every completed run is saved, fast
 * or slow, numbered alongside the others.
 *
 * It keeps a rolling buffer while you are alive, marks where the timer started,
 * and writes the file when you finish. The rolling part matters: the run proper
 * begins at the timer, but the prestrafe before it has to be in the file too,
 * and that has already happened by the time the timer fires.
 *
 * Shavit_OnStart and Shavit_OnFinish are global forwards, so implementing them
 * costs no load-time dependency - without the timer they simply never fire, and
 * the manual commands still work.
 */

#if defined _csai_record_included
	#endinput
#endif
#define _csai_record_included

#define REC_HUMAN_MAX   20000    // ~5 minutes at 66 tick
#define REC_PRE_KEEP      256    // prestrafe ticks kept before the timer starts
#define REC_PRE_TRIM     1024    // compact the pre-buffer once it reaches this

float g_fRecHumanPos[REC_HUMAN_MAX][3];
float g_fRecHumanAng[REC_HUMAN_MAX][2];
int   g_iRecHumanBtn[REC_HUMAN_MAX];

int  g_iRecHumanCount  = 0;
int  g_iRecHumanPre    = -1;    // -1 = timer has not started yet
int  g_iRecHumanClient = -1;
bool g_bRecHumanOn     = true;  // recording is on by default; nothing to remember

stock void Rec_Human_Reset()
{
	g_iRecHumanCount = 0;
	g_iRecHumanPre   = -1;
}

/** Next free <map>_demo[_N].txt, so runs never overwrite each other. */
stock int Rec_Human_NextSlot()
{
	char path[PLATFORM_MAX_PATH];
	for (int i = 0; i < DEMO_MAX_FILES; i++)
	{
		Demo_Path(i, path, sizeof(path));
		if (!FileExists(path))
			return i;
	}
	return -1;
}

/**
 * Keep only the last REC_PRE_KEEP ticks before the timer started.
 *
 * Whatever came earlier is idling in the start zone or a failed attempt, and the
 * whole file gets replayed through physics later - a junk prefix is replayed
 * too. The real prestrafe is about 67 ticks.
 */
stock void Rec_Human_TrimPre()
{
	if (g_iRecHumanCount <= REC_PRE_KEEP)
		return;

	int from = g_iRecHumanCount - REC_PRE_KEEP;
	for (int i = 0; i < REC_PRE_KEEP; i++)
	{
		g_fRecHumanPos[i][0] = g_fRecHumanPos[from + i][0];
		g_fRecHumanPos[i][1] = g_fRecHumanPos[from + i][1];
		g_fRecHumanPos[i][2] = g_fRecHumanPos[from + i][2];
		g_fRecHumanAng[i][0] = g_fRecHumanAng[from + i][0];
		g_fRecHumanAng[i][1] = g_fRecHumanAng[from + i][1];
		g_iRecHumanBtn[i]    = g_iRecHumanBtn[from + i];
	}
	g_iRecHumanCount = REC_PRE_KEEP;
}

stock void Rec_Human_Tick(int client, int buttons, const float angles[3])
{
	if (!g_bRecHumanOn || client != g_iRecHumanClient)
		return;

	// Before the timer starts this is just a rolling window. Compact it in
	// blocks rather than shifting every tick - idling in the start zone would
	// otherwise fill the whole buffer.
	if (g_iRecHumanPre < 0 && g_iRecHumanCount >= REC_PRE_TRIM)
	{
		int keep = REC_PRE_KEEP;
		int from = g_iRecHumanCount - keep;
		for (int i = 0; i < keep; i++)
		{
			g_fRecHumanPos[i][0] = g_fRecHumanPos[from + i][0];
			g_fRecHumanPos[i][1] = g_fRecHumanPos[from + i][1];
			g_fRecHumanPos[i][2] = g_fRecHumanPos[from + i][2];
			g_fRecHumanAng[i][0] = g_fRecHumanAng[from + i][0];
			g_fRecHumanAng[i][1] = g_fRecHumanAng[from + i][1];
			g_iRecHumanBtn[i]    = g_iRecHumanBtn[from + i];
		}
		g_iRecHumanCount = keep;
	}

	if (g_iRecHumanCount >= REC_HUMAN_MAX)
		return;

	float pos[3];
	GetClientAbsOrigin(client, pos);

	// A respawn or a timer restart moves the player thousands of units in one
	// tick. Everything buffered before that belongs to the previous attempt, and
	// replaying it would start the run in the wrong place entirely.
	if (g_iRecHumanCount > 0)
	{
		int last = g_iRecHumanCount - 1;
		float dx = pos[0] - g_fRecHumanPos[last][0];
		float dy = pos[1] - g_fRecHumanPos[last][1];
		float dz = pos[2] - g_fRecHumanPos[last][2];
		if (SquareRoot(dx * dx + dy * dy + dz * dz) > 200.0)
			Rec_Human_Reset();
	}

	int i = g_iRecHumanCount;
	g_fRecHumanPos[i][0] = pos[0];
	g_fRecHumanPos[i][1] = pos[1];
	g_fRecHumanPos[i][2] = pos[2];
	g_fRecHumanAng[i][0] = angles[0];
	g_fRecHumanAng[i][1] = angles[1];
	g_iRecHumanBtn[i]    = buttons;
	g_iRecHumanCount++;
}

/** Write the buffer as a demo file in the format csai_demo.inc reads. */
stock bool Rec_Human_Save(int client)
{
	if (g_iRecHumanCount < 64)
	{
		if (client > 0 && IsClientInGame(client))
			PrintToChat(client, "[CsAI] nothing recorded yet");
		return false;
	}

	int slot = Rec_Human_NextSlot();
	if (slot < 0)
	{
		if (client > 0 && IsClientInGame(client))
			PrintToChat(client, "[CsAI] all %d demo slots are full", DEMO_MAX_FILES);
		return false;
	}

	char path[PLATFORM_MAX_PATH];
	Demo_Path(slot, path, sizeof(path));

	File f = OpenFile(path, "w");
	if (f == null)
	{
		PrintToServer("[CsAI] record: could not open %s", path);
		return false;
	}

	char map[PLATFORM_MAX_PATH];
	GetCurrentMap(map, sizeof(map));

	// No timer start seen means the whole buffer is the run; treating it as all
	// preframes would throw the file away.
	int pre = (g_iRecHumanPre >= 0) ? g_iRecHumanPre : 0;

	f.WriteLine("# csai demo v1  map=%s tickrate=%.2f preframes=%d total=%d",
	            map, 1.0 / GetTickInterval(), pre, g_iRecHumanCount);
	f.WriteLine("# idx x y z pitch yaw buttons");

	for (int i = 0; i < g_iRecHumanCount; i++)
	{
		f.WriteLine("%d %.4f %.4f %.4f %.4f %.4f %d", i,
		            g_fRecHumanPos[i][0], g_fRecHumanPos[i][1], g_fRecHumanPos[i][2],
		            g_fRecHumanAng[i][0], g_fRecHumanAng[i][1], g_iRecHumanBtn[i]);
	}
	delete f;

	float secs = float(g_iRecHumanCount - pre) * GetTickInterval();
	PrintToServer("[CsAI] recorded run %d: %d ticks (%d prestrafe), %.2fs -> %s",
	              slot + 1, g_iRecHumanCount, pre, secs, path);
	if (client > 0 && IsClientInGame(client))
	{
		PrintToChat(client, "[CsAI] saved run %d - %.2fs, %d runs now available",
		            slot + 1, secs, slot + 1);
		PrintToChat(client, "[CsAI] run it again for another, or type !csai_runs to check");
	}
	Rec_Human_Reset();
	return true;
}

/** Whoever is playing and is not our bot. */
stock void Rec_Human_Track(int client)
{
	if (client == g_iBot || IsFakeClient(client))
		return;
	if (g_iRecHumanClient != client)
	{
		g_iRecHumanClient = client;
		Rec_Human_Reset();
	}
}
