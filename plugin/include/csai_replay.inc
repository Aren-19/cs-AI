#if defined _csai_replay_included
	#endinput
#endif
#define _csai_replay_included

#define REC_CELLS        10          // pos3 ang2 buttons flags movetype mousexy vel
#define REC_MAX_FRAMES   20000       // ~5 minutes at 66 tick
#define SHAVIT_VERSION   9

ArrayList g_hRecFrames = null;
bool      g_bRecording = false;
int       g_iRecPreFrames = 0;   // leading frames that are the replayed prestrafe

stock void Rec_Init()
{
	if (g_hRecFrames == null)
		g_hRecFrames = new ArrayList(REC_CELLS);
}

stock void Rec_Start()
{
	Rec_Init();
	g_hRecFrames.Clear();
	g_iRecPreFrames = 0;
	g_bRecording = true;
}

stock void Rec_Stop()
{
	g_bRecording = false;
}

/** Called every physics tick while recording. */
stock void Rec_Tick(int client, int buttons)
{
	if (!g_bRecording || g_hRecFrames == null || g_hRecFrames.Length >= REC_MAX_FRAMES)
		return;

	float pos[3], ang[3];
	GetClientAbsOrigin(client, pos);
	GetClientEyeAngles(client, ang);

	any row[REC_CELLS];
	row[0] = view_as<any>(pos[0]);
	row[1] = view_as<any>(pos[1]);
	row[2] = view_as<any>(pos[2]);
	row[3] = view_as<any>(ang[0]);
	row[4] = view_as<any>(ang[1]);
	row[5] = buttons;
	row[6] = GetEntityFlags(client);
	row[7] = view_as<int>(GetEntityMoveType(client));
	row[8] = 0;                       // mousexy: unused by playback
	row[9] = 0;                       // vel: packed fwd|side, unused by playback
	g_hRecFrames.PushArray(row, REC_CELLS);
}

stock bool Rec_Save(const char[] tag, float seconds)
{
	if (g_hRecFrames == null || g_hRecFrames.Length == 0)
	{
		PrintToServer("[CsAI] replay: nothing recorded");
		return false;
	}

	char dir[PLATFORM_MAX_PATH];
	BuildPath(Path_SM, dir, sizeof(dir), "data/csai/replays");
	if (!DirExists(dir))
		CreateDirectory(dir, 511);

	char map[PLATFORM_MAX_PATH];
	GetCurrentMap(map, sizeof(map));

	char path[PLATFORM_MAX_PATH];
	BuildPath(Path_SM, path, sizeof(path), "data/csai/replays/%s_%s.replay", map, tag);

	File f = OpenFile(path, "wb");
	if (f == null)
	{
		PrintToServer("[CsAI] replay: could not open %s", path);
		return false;
	}

	int frames = g_hRecFrames.Length;

	// header line, then the NUL-terminated map name
	char head[64];
	Format(head, sizeof(head), "%d:{SHAVITREPLAYFORMAT}{FINAL}\n", SHAVIT_VERSION);
	f.WriteString(head, false);
	f.WriteString(map, true);

	int pre = g_iRecPreFrames;
	if (pre > frames) pre = frames;

	f.WriteInt8(0);                                   // style: Normal
	f.WriteInt8(0);                                   // track: main
	f.WriteInt32(pre);                                // preframes
	f.WriteInt32(frames - pre);                       // frameCount (timed portion)
	f.WriteInt32(view_as<int>(seconds));              // time
	f.WriteInt32(0);                                  // steamid (bot)
	f.WriteInt32(0);                                  // postframes
	f.WriteInt32(view_as<int>(1.0 / GetTickInterval()));
	f.WriteInt32(view_as<int>(0.0));                  // zone offset start
	f.WriteInt32(view_as<int>(0.0));                  // zone offset end

	any row[REC_CELLS];
	for (int i = 0; i < frames; i++)
	{
		g_hRecFrames.GetArray(i, row, REC_CELLS);
		f.Write(row, REC_CELLS, 4);
	}
	delete f;

	PrintToServer("[CsAI] replay: %s (%d prestrafe + %d run frames, %.2fs)",
	              path, pre, frames - pre, seconds);
	return true;
}
