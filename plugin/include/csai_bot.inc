/**
 * csai_bot - the controlled fake client, and episode start states.
 *
 * Start states come from a human replay, parsed by tools/replay.py into a flat
 * text file (SourcePawn has no JSON parser). State 0 is the map's real start;
 * the rest are evenly spaced along the successful line so the agent can be
 * dropped part-way through the map without first solving everything before it.
 *
 * This lives in its own include because csai_episode.inc needs these globals,
 * and Pawn requires variables to be declared before use.
 */

#if defined _csai_bot_included
	#endinput
#endif
#define _csai_bot_included

int g_iBot = -1;

// ----------------------------------------------------------- start states ----
//
// Episode start states come from a human replay, parsed by tools/replay.py into
// a flat text file (SourcePawn has no JSON parser). State 0 is the map's real
// start; the rest are evenly spaced along the successful line and exist so the
// agent can be dropped 80% of the way through the map without having to solve
// the first 80% first. Surf state is fully described by origin+angles+velocity,
// so each of these is a complete, restorable savestate.
//
// Only origin, angles and velocity are restored. The recorded `flags` field
// carries engine bits we must not force onto a live player.

#define MAX_STATES 64

float g_fStateOrigin[MAX_STATES][3];
float g_fStateAngles[MAX_STATES][3];
float g_fStateVel[MAX_STATES][3];
float g_fStateFrac[MAX_STATES];
int   g_iStateCount = 0;
float g_fRefTime    = 0.0;   // the human's clean time for this map, seconds
char  g_sStateMap[PLATFORM_MAX_PATH];

void LoadStates()
{
    g_iStateCount = 0;

    char map[PLATFORM_MAX_PATH];
    GetCurrentMap(map, sizeof(map));

    char path[PLATFORM_MAX_PATH];
    BuildPath(Path_SM, path, sizeof(path), "data/csai/%s_states.txt", map);

    if (!FileExists(path))
    {
        PrintToServer("[CsAI] no states file for %s (looked in %s)", map, path);
        return;
    }

    File f = OpenFile(path, "r");
    if (f == null)
    {
        PrintToServer("[CsAI] could not open %s", path);
        return;
    }

    char line[512];
    char parts[16][32];

    while (!f.EndOfFile() && f.ReadLine(line, sizeof(line)))
    {
        TrimString(line);
        if (line[0] == '#')
        {
            // The reference time rides in this file's own header, so the
            // speed objective is the map's own number rather than a constant.
            int at = StrContains(line, "clean_time=");
            if (at != -1)
            {
                char tmp[24];
                int k = 0;
                at += 11;
                while (at + k < strlen(line) && line[at + k] != ' ' && k < 23)
                {
                    tmp[k] = line[at + k];
                    k++;
                }
                tmp[k] = '\0';
                g_fRefTime = StringToFloat(tmp);
            }
            continue;
        }
        if (line[0] == '\0')
            continue;

        int n = ExplodeString(line, " ", parts, sizeof(parts), sizeof(parts[]));
        if (n < 13)
            continue;
        if (g_iStateCount >= MAX_STATES)
            break;

        int i = g_iStateCount;
        g_fStateFrac[i]      = StringToFloat(parts[1]);
        g_fStateOrigin[i][0] = StringToFloat(parts[3]);
        g_fStateOrigin[i][1] = StringToFloat(parts[4]);
        g_fStateOrigin[i][2] = StringToFloat(parts[5]);
        g_fStateAngles[i][0] = StringToFloat(parts[6]);
        g_fStateAngles[i][1] = StringToFloat(parts[7]);
        g_fStateAngles[i][2] = 0.0;
        g_fStateVel[i][0]    = StringToFloat(parts[8]);
        g_fStateVel[i][1]    = StringToFloat(parts[9]);
        g_fStateVel[i][2]    = StringToFloat(parts[10]);
        g_iStateCount++;
    }
    delete f;

    strcopy(g_sStateMap, sizeof(g_sStateMap), map);
    PrintToServer("[CsAI] loaded %d start states for %s (reference %.2fs)",
                  g_iStateCount, map, g_fRefTime);
    if (g_fRefTime <= 0.0)
        PrintToServer("[CsAI] no reference time in the states file - the speed bonus cannot pay");

    if (g_iStateCount > 0)
    {
        PrintToServer("[CsAI]  state 0: (%.0f %.0f %.0f) yaw %.1f vel(%.0f %.0f %.0f)",
                      g_fStateOrigin[0][0], g_fStateOrigin[0][1], g_fStateOrigin[0][2],
                      g_fStateAngles[0][1],
                      g_fStateVel[0][0], g_fStateVel[0][1], g_fStateVel[0][2]);
    }
}

bool ApplyState(int client, int idx)
{
    if (idx < 0 || idx >= g_iStateCount)
    {
        PrintToServer("[CsAI] state %d out of range (have %d)", idx, g_iStateCount);
        return false;
    }
    if (client == -1 || !IsClientInGame(client))
        return false;

    float ang[3];
    ang[0] = 0.0;                       // AirMove zeroes pitch for wishdir anyway
    ang[1] = g_fStateAngles[idx][1];
    ang[2] = 0.0;

    TeleportEntity(client, g_fStateOrigin[idx], ang, g_fStateVel[idx]);

    float check[3];
    GetClientAbsOrigin(client, check);
    PrintToServer("[CsAI] state %d (%.0f%% through): target (%.0f %.0f %.0f) readback (%.0f %.0f %.0f)",
                  idx, g_fStateFrac[idx] * 100.0,
                  g_fStateOrigin[idx][0], g_fStateOrigin[idx][1], g_fStateOrigin[idx][2],
                  check[0], check[1], check[2]);
    return true;
}

public Action Cmd_State(int args)
{
    char buf[32];
    int idx = 0;
    if (args >= 1) { GetCmdArg(1, buf, sizeof(buf)); idx = StringToInt(buf); }

    if (g_iBot == -1 || !IsClientInGame(g_iBot))
    {
        PrintToServer("[CsAI] no bot - run csai_spawn first");
        return Plugin_Handled;
    }
    UnfreezeBot();
    ApplyState(g_iBot, idx);
    return Plugin_Handled;
}

public Action Cmd_States(int args)
{
    LoadStates();
    return Plugin_Handled;
}

stock bool ApplyStateQuiet(int client, int idx)
{
	if (idx < 0 || idx >= g_iStateCount)
		return false;
	if (client == -1 || !IsClientInGame(client))
		return false;

	float ang[3];
	ang[0] = 0.0;
	ang[1] = g_fStateAngles[idx][1];
	ang[2] = 0.0;

	TeleportEntity(client, g_fStateOrigin[idx], ang, g_fStateVel[idx]);
	return true;
}
