/**
 * csai_replay - write the bot's run as a real shavit .replay file.
 *
 * The web viewer parses shavit's format, not ours. Our training trajectories are
 * a different thing entirely: one record per *agent step* (frame-skipped), with
 * no view angles, because the learner does not need them. A replay needs every
 * tick, with angles and buttons, or playback stutters and the camera has nothing
 * to point.
 *
 * So recording is separate from training logging: this captures the full per-tick
 * frame during evaluation runs, and writes the exact v9 layout that
 * addons/sourcemod/scripting/include/shavit/replay-file.inc defines.
 *
 * Header (little-endian, no padding):
 *   "<version>:<format>\n"   ASCII line
 *   map (NUL-terminated), style u8, track u8, preframes i32
 *   frameCount i32, time f32, steamid i32, postframes i32, tickrate f32
 *   zoneOffset[0] f32, zoneOffset[1] f32
 * Frame (40 bytes):
 *   pos[3] f32, ang[2] f32, buttons i32, flags i32, movetype i32,
 *   mousexy i32 (packed shorts), vel i32 (fwd|side packed shorts)
 */

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

/**
 * Write the buffer out. `seconds` is the run time the HUD shows; pass the
 * episode's own elapsed time so the scrubber matches reality.
 */
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

	// The prestrafe is stored as preframes, exactly as a timer does: the viewer
	// then plays the complete run - stationary start, prestrafe, handover, surf -
	// while the timed portion stays the part the policy is responsible for.
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
