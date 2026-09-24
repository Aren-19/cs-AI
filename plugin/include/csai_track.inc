#if defined _csai_track_included
	#endinput
#endif
#define _csai_track_included

#define MAX_TRACK       16384

#define LOOKAHEAD       7
#define PROBE_RAYS      8        // 5 down-facing, then left, right, forward
#define PROBE_DOWN_RAYS 5
#define PROBE_DIM       (PROBE_RAYS + 3)
#define WISH_DIM        0
#define OBS_DIM         (7 + 3 * LOOKAHEAD + PROBE_DIM + WISH_DIM)

int g_iLookOffsets[LOOKAHEAD] = {4, 8, 16, 32, 64, 128, 256};
bool g_bTrackTruncated = false;   // the file had more points than MAX_TRACK

float g_fTrackX[MAX_TRACK];
float g_fTrackY[MAX_TRACK];
float g_fTrackZ[MAX_TRACK];
float g_fTrackS[MAX_TRACK];      // cumulative arc length
int   g_iTrackCount  = 0;
float g_fTrackLength = 0.0;
int   g_iTrackHint   = -1;       // last nearest index, for incremental search

// shavit's start zone, from data/csai/<map>_zone.txt: "start x0 y0 z0 x1 y1 z1".
float g_fZoneMin[3];
float g_fZoneMax[3];
bool  g_bZoneLoaded = false;

stock void Zone_Load()
{
	g_bZoneLoaded = false;

	char map[PLATFORM_MAX_PATH];
	GetCurrentMap(map, sizeof(map));
	char path[PLATFORM_MAX_PATH];
	BuildPath(Path_SM, path, sizeof(path), "data/csai/%s_zone.txt", map);

	File f = OpenFile(path, "r");
	if (f == null)
	{
		PrintToServer("[CsAI] no start zone for %s - the wind-up hands over on takeoff only", map);
		return;
	}
	char line[256];
	char parts[8][32];
	while (!f.EndOfFile() && f.ReadLine(line, sizeof(line)))
	{
		TrimString(line);
		if (ExplodeString(line, " ", parts, sizeof(parts), sizeof(parts[])) < 7 || !StrEqual(parts[0], "start"))
			continue;
		for (int k = 0; k < 3; k++)
		{
			float a = StringToFloat(parts[1 + k]);
			float b = StringToFloat(parts[4 + k]);
			g_fZoneMin[k] = (a < b) ? a : b;
			g_fZoneMax[k] = (a < b) ? b : a;
		}
		g_bZoneLoaded = true;
	}
	delete f;
	if (g_bZoneLoaded)
		PrintToServer("[CsAI] start zone (%.0f %.0f %.0f) to (%.0f %.0f %.0f)",
		              g_fZoneMin[0], g_fZoneMin[1], g_fZoneMin[2], g_fZoneMax[0], g_fZoneMax[1], g_fZoneMax[2]);
}

/** Inside the start zone as shavit counts it: its trigger is the box shrunk by 16 sideways and 31 vertically. */
stock bool Zone_Touching(const float pos[3])
{
	if (!g_bZoneLoaded)
		return false;
	return pos[0] >= g_fZoneMin[0] && pos[0] <= g_fZoneMax[0]
	    && pos[1] >= g_fZoneMin[1] && pos[1] <= g_fZoneMax[1]
	    && pos[2] < g_fZoneMax[2] - 31.0 && pos[2] + 72.0 > g_fZoneMin[2] + 31.0;
}

/** Horizontal distance from the player's hull to the nearest side of the start zone; negative outside it. */
stock float Zone_EdgeDistance(const float pos[3])
{
	if (!g_bZoneLoaded)
		return 1.0e9;
	float d = pos[0] - (g_fZoneMin[0] - 16.0);
	float e = (g_fZoneMax[0] + 16.0) - pos[0];
	if (e < d) d = e;
	e = pos[1] - (g_fZoneMin[1] - 16.0);
	if (e < d) d = e;
	e = (g_fZoneMax[1] + 16.0) - pos[1];
	if (e < d) d = e;
	return d;
}

stock void Track_Load()
{
	g_iTrackCount  = 0;
	g_fTrackLength = 0.0;
	g_iTrackHint   = -1;
	g_bTrackTruncated = false;

	char map[PLATFORM_MAX_PATH];
	GetCurrentMap(map, sizeof(map));

	char path[PLATFORM_MAX_PATH];
	BuildPath(Path_SM, path, sizeof(path), "data/csai/%s_track.txt", map);

	if (!FileExists(path))
	{
		PrintToServer("[CsAI] no track file for %s", map);
		return;
	}

	File f = OpenFile(path, "r");
	if (f == null)
		return;

	char line[256];
	char parts[8][32];

	while (!f.EndOfFile() && f.ReadLine(line, sizeof(line)))
	{
		TrimString(line);
		if (line[0] == '\0' || line[0] == '#')
			continue;
		if (g_iTrackCount >= MAX_TRACK)
		{
			g_bTrackTruncated = true;
			break;
		}

		int n = ExplodeString(line, " ", parts, sizeof(parts), sizeof(parts[]));
		if (n < 5)
			continue;

		int i = g_iTrackCount;
		g_fTrackX[i] = StringToFloat(parts[1]);
		g_fTrackY[i] = StringToFloat(parts[2]);
		g_fTrackZ[i] = StringToFloat(parts[3]);
		g_fTrackS[i] = StringToFloat(parts[4]);
		g_iTrackCount++;
	}
	delete f;

	if (g_iTrackCount > 0)
		g_fTrackLength = g_fTrackS[g_iTrackCount - 1];

	PrintToServer("[CsAI] track: %d points, %.0f units", g_iTrackCount, g_fTrackLength);
	if (g_bTrackTruncated)
	{
		// Loud, because the consequence is a finish line in the middle of the map.
		PrintToServer("[CsAI] ####################################################");
		PrintToServer("[CsAI] TRACK TRUNCATED at %d points - this map is longer than", MAX_TRACK);
		PrintToServer("[CsAI] the build allows. Episodes will report FINISHED partway");
		PrintToServer("[CsAI] through. Raise MAX_TRACK and rebuild before training.");
		PrintToServer("[CsAI] ####################################################");
	}
}

static stock float Sq(float v) { return v * v; }

static stock float DistSqToPoint(const float pos[3], int i)
{
	return Sq(pos[0] - g_fTrackX[i]) + Sq(pos[1] - g_fTrackY[i]) + Sq(pos[2] - g_fTrackZ[i]);
}

stock int Track_Nearest(const float pos[3], int hint)
{
	if (g_iTrackCount == 0)
		return -1;

	int lo, hi;
	if (hint < 0)
	{
		lo = 0;
		hi = g_iTrackCount - 1;
	}
	else
	{
		lo = hint - 16;
		hi = hint + 64;          // asymmetric: forward motion is the common case
		if (lo < 0) lo = 0;
		if (hi > g_iTrackCount - 1) hi = g_iTrackCount - 1;
	}

	int best = lo;
	float bestd = DistSqToPoint(pos, lo);
	for (int i = lo + 1; i <= hi; i++)
	{
		float d = DistSqToPoint(pos, i);
		if (d < bestd)
		{
			bestd = d;
			best = i;
		}
	}
	return best;
}

stock float Track_Project(const float pos[3], int idx, float closest[3], float &outDist)
{
	if (idx < 0 || g_iTrackCount == 0)
	{
		outDist = 0.0;
		return 0.0;
	}

	float bestS = g_fTrackS[idx];
	float bestD = 1.0e18;
	closest[0] = g_fTrackX[idx];
	closest[1] = g_fTrackY[idx];
	closest[2] = g_fTrackZ[idx];

	for (int k = -1; k <= 0; k++)
	{
		int a = idx + k;
		int b = a + 1;
		if (a < 0 || b > g_iTrackCount - 1)
			continue;

		float ax = g_fTrackX[a], ay = g_fTrackY[a], az = g_fTrackZ[a];
		float bx = g_fTrackX[b], by = g_fTrackY[b], bz = g_fTrackZ[b];
		float ex = bx - ax, ey = by - ay, ez = bz - az;

		float len2 = ex * ex + ey * ey + ez * ez;
		if (len2 < 0.0001)
			continue;

		float t = ((pos[0] - ax) * ex + (pos[1] - ay) * ey + (pos[2] - az) * ez) / len2;
		if (t < 0.0) t = 0.0;
		if (t > 1.0) t = 1.0;

		float px = ax + ex * t, py = ay + ey * t, pz = az + ez * t;
		float d  = Sq(pos[0] - px) + Sq(pos[1] - py) + Sq(pos[2] - pz);

		if (d < bestD)
		{
			bestD = d;
			bestS = g_fTrackS[a] + (g_fTrackS[b] - g_fTrackS[a]) * t;
			closest[0] = px; closest[1] = py; closest[2] = pz;
		}
	}

	outDist = SquareRoot(bestD);
	return bestS;
}

/** Heading of the centerline at index i, in degrees. */
stock float Track_TangentYaw(int i)
{
	int a = i;
	int b = i + 1;
	if (b > g_iTrackCount - 1)
	{
		a = g_iTrackCount - 2;
		b = g_iTrackCount - 1;
	}
	if (a < 0)
		return 0.0;
	return RadToDeg(ArcTangent2(g_fTrackY[b] - g_fTrackY[a], g_fTrackX[b] - g_fTrackX[a]));
}

// The line the policy sees can be shifted by a slow wave per episode, so it
// learns to steer by the ramps rather than by the exact line it was shown:
// sideways  g_fLineOff[0] + g_fLineOff[1] * sin(2 pi s / g_fLineOff[2] + g_fLineOff[3])
// up/down   g_fLineOff[4] + g_fLineOff[5] * sin(2 pi s / g_fLineOff[6] + g_fLineOff[7])
// Progress and rewards always use the real line.
#define LINE_PARAMS 8
float g_fLineOff[LINE_PARAMS];

stock void Track_ClearLineOffset()
{
	for (int i = 0; i < LINE_PARAMS; i++)
		g_fLineOff[i] = 0.0;
	g_fLineOff[2] = 1.0;
	g_fLineOff[6] = 1.0;
}

stock void Track_LineOffset(float s, float &side, float &up)
{
	side = g_fLineOff[0];
	up   = g_fLineOff[4];
	if (g_fLineOff[2] > 0.0)
		side += g_fLineOff[1] * Sine(6.2831853 * s / g_fLineOff[2] + g_fLineOff[3]);
	if (g_fLineOff[6] > 0.0)
		up += g_fLineOff[5] * Sine(6.2831853 * s / g_fLineOff[6] + g_fLineOff[7]);
}

stock void Track_BuildObs(const float pos[3], const float vel[3], int idx, float obs[OBS_DIM])
{
	for (int i = 0; i < OBS_DIM; i++)
		obs[i] = 0.0;

	if (idx < 0 || g_iTrackCount == 0)
		return;

	float closest[3], dist;
	float s = Track_Project(pos, idx, closest, dist);

	float speed  = SquareRoot(vel[0] * vel[0] + vel[1] * vel[1]);
	float velYaw = (speed > 1.0) ? RadToDeg(ArcTangent2(vel[1], vel[0])) : Track_TangentYaw(idx);
	float tanYaw = Track_TangentYaw(idx);

	float dYaw = DegToRad(velYaw - tanYaw);

	// signed lateral offset: cross product of tangent with the offset vector
	float tr = DegToRad(tanYaw);
	float tx = Cosine(tr), ty = Sine(tr);
	float ox = pos[0] - closest[0], oy = pos[1] - closest[1];
	float lateral = tx * oy - ty * ox;
	float side, up;
	Track_LineOffset(s, side, up);

	obs[0] = speed / 1000.0;
	obs[1] = vel[2] / 1000.0;
	obs[2] = Cosine(dYaw);
	obs[3] = Sine(dYaw);
	obs[4] = (lateral - side) / 256.0;
	obs[5] = (pos[2] - closest[2] - up) / 256.0;
	obs[6] = (g_fTrackLength > 0.0) ? (s / g_fTrackLength) : 0.0;

	// lookahead points, rotated into the velocity frame
	float vr = DegToRad(velYaw);
	float cy = Cosine(vr), sy = Sine(vr);

	for (int k = 0; k < LOOKAHEAD; k++)
	{
		int j = idx + g_iLookOffsets[k];
		if (j > g_iTrackCount - 1)
			j = g_iTrackCount - 1;

		float js, ju;
		Track_LineOffset(g_fTrackS[j], js, ju);
		float jr = DegToRad(Track_TangentYaw(j));
		float rx = g_fTrackX[j] - Sine(jr) * js - pos[0];
		float ry = g_fTrackY[j] + Cosine(jr) * js - pos[1];
		float rz = g_fTrackZ[j] + ju - pos[2];

		float scale = float(g_iLookOffsets[k]) * 64.0;   // nominal arc distance

		int o = 7 + k * 3;
		obs[o + 0] = ( cy * rx + sy * ry) / scale;   // forward
		obs[o + 1] = (-sy * rx + cy * ry) / scale;   // left
		obs[o + 2] = rz / scale;
	}
}

stock void Track_BuildProbe(int client, const float pos[3], const float vel[3], float out[PROBE_DIM])
{
	for (int i = 0; i < PROBE_DIM; i++)
		out[i] = 0.0;

	float speed = SquareRoot(vel[0] * vel[0] + vel[1] * vel[1]);
	float velYaw = (speed > 1.0) ? RadToDeg(ArcTangent2(vel[1], vel[0])) : 0.0;
	float vr = DegToRad(velYaw);
	float cy = Cosine(vr), sy = Sine(vr);

	// ray directions in the velocity frame: (forward, left, up)
	float dirs[PROBE_RAYS][3];
	dirs[0][0] =  0.0; dirs[0][1] =  0.0; dirs[0][2] = -1.0;   // straight down
	dirs[1][0] =  0.7; dirs[1][1] =  0.0; dirs[1][2] = -0.7;   // forward-down
	dirs[2][0] = -0.7; dirs[2][1] =  0.0; dirs[2][2] = -0.7;   // back-down
	dirs[3][0] =  0.0; dirs[3][1] =  0.7; dirs[3][2] = -0.7;   // left-down
	dirs[4][0] =  0.0; dirs[4][1] = -0.7; dirs[4][2] = -0.7;   // right-down
	dirs[5][0] =  0.0; dirs[5][1] =  1.0; dirs[5][2] =  0.0;   // left, level
	dirs[6][0] =  0.0; dirs[6][1] = -1.0; dirs[6][2] =  0.0;   // right, level
	dirs[7][0] =  1.0; dirs[7][1] =  0.0; dirs[7][2] =  0.0;   // straight ahead

	float bestFrac = 2.0;
	float bestNormal[3];
	bestNormal[0] = 0.0; bestNormal[1] = 0.0; bestNormal[2] = 0.0;

	for (int i = 0; i < PROBE_RAYS; i++)
	{
		float maxDist = (i < PROBE_DOWN_RAYS) ? 512.0 : 1024.0;

		// rotate the velocity-frame direction into world space
		float wx = cy * dirs[i][0] - sy * dirs[i][1];
		float wy = sy * dirs[i][0] + cy * dirs[i][1];
		float wz = dirs[i][2];

		float start[3], end[3];
		start[0] = pos[0]; start[1] = pos[1]; start[2] = pos[2] + 16.0;
		end[0] = start[0] + wx * maxDist;
		end[1] = start[1] + wy * maxDist;
		end[2] = start[2] + wz * maxDist;

		Handle tr = TR_TraceRayFilterEx(start, end, MASK_PLAYERSOLID, RayType_EndPoint,
		                                TraceFilterNotSelf, client);
		float frac = 1.0;
		if (tr != null && TR_DidHit(tr))
		{
			float hit[3];
			TR_GetEndPosition(hit, tr);
			float dx = hit[0] - start[0], dy = hit[1] - start[1], dz = hit[2] - start[2];
			frac = SquareRoot(dx * dx + dy * dy + dz * dz) / maxDist;

			// Normal still comes from the down-facing rays only, so this value
			// keeps the exact meaning it had before the level rays were added.
			if (i < PROBE_DOWN_RAYS && frac < bestFrac)
			{
				bestFrac = frac;
				TR_GetPlaneNormal(tr, bestNormal);
			}
		}
		delete tr;
		out[i] = frac;                 // 1.0 = nothing within range
	}

	// nearest surface normal, rotated into the velocity frame
	out[PROBE_RAYS + 0] =  cy * bestNormal[0] + sy * bestNormal[1];
	out[PROBE_RAYS + 1] = -sy * bestNormal[0] + cy * bestNormal[1];
	out[PROBE_RAYS + 2] =  bestNormal[2];
}
