/**
 * csai_track - centerline, progress, observation vector.
 *
 * The centerline comes from a human replay (tools/replay.py --track). Progress
 * along it is the dense reward signal: defined on every tick, unlike a sparse
 * "did you finish" bonus, which is unlearnable over a 2600-tick episode.
 *
 * The observation is deliberately built from (pos, vel) and the centerline ONLY -
 * no traces. Two reasons:
 *   1. Tracking the reference line IS surfing, because the line runs along the
 *      ramps. The agent does not need to see geometry it is already being told
 *      to follow.
 *   2. Everything here is recomputable in Python from logged (pos, vel), so
 *      trajectories log 8 floats per step instead of 30+, and the observation
 *      definition can change without re-collecting data.
 */

#if defined _csai_track_included
	#endinput
#endif
#define _csai_track_included

/**
 * Room for a long map, and a loud failure rather than a quiet one if exceeded.
 *
 * Measured: surf_demise 1474 points, surf_dune 890. At demise's ~92 units per
 * point, 2048 points was only about 188k units - roughly 40% of headroom. Going
 * over used to `break` silently, which set the track length to the arc length of
 * the last point that fit, and the finish test (s >= length - 96) then declared
 * EP_FINISHED partway through the map and paid the completion bonus.
 */
#define MAX_TRACK       16384

/**
 * Lookahead points are spaced LOGARITHMICALLY, not uniformly.
 *
 * Uniform spacing (5 points x 6 track points = 1920 units) gave the policy only
 * 0.53 s of preview at this map's median 3614 u/s, and 0.40 s at its 4789 u/s
 * peak - while episodes last ~11.7 s. That is the information ceiling that made
 * training plateau at ~6.7% of the track regardless of how much data it saw: you
 * cannot plan a surf line you cannot see.
 *
 * Track points are 64 units apart, so these offsets span 256 .. 16384 units,
 * i.e. roughly 0.07 s .. 4.5 s ahead at median speed. Near points keep the local
 * precision, far points supply the route context.
 *
 * Each point is normalised by its own nominal arc distance, so all seven stay
 * O(1) instead of the far ones dominating the input scale.
 */
#define LOOKAHEAD       7
#define PROBE_RAYS      8        // 5 down-facing, then left, right, forward
/**
 * Rays 0-4 look down, for the ramp underneath - which is the whole story in
 * ordinary surfing. Rays 5-7 look sideways and ahead, and exist because of the
 * drop at 72% of surf_demise: the surface there is a near-vertical wall beside
 * the bot rather than a ramp below it.
 *
 * Measured going into that drop: at 70.3% four rays were touching geometry
 * (0.05-0.12 of range), and half a second later four of the five read 1.00 -
 * nothing within 512 units - leaving one oblique ray at ~330 units as the only
 * description of the wall it had to ride.
 *
 * The new rays reach 1024 rather than 512, because 512 units is under two
 * decisions of warning at 4000 u/s. Rays 0-4 keep their old range and directions
 * exactly, and the surface normal is still taken from them alone, so every
 * observation value that existed before is unchanged and a trained policy can be
 * carried straight over.
 */
#define PROBE_DOWN_RAYS 5
#define PROBE_DIM       (PROBE_RAYS + 3)
// +2 for sin/cos of the currently held wish angle. With delta actions the
// absolute angle is internal state; if the policy cannot see it, it is acting
// blind on the one variable it directly controls.
/**
 * Previously the observation ended with cos/sin of the PREVIOUS wish angle.
 * That is the previous action, and behaviour cloning latched onto it: because a
 * human changes strafe key only ~1/s out of 33 decisions/s, "repeat the last
 * action" alone predicts their side 96.7% of the time. The clone scored 0.972
 * side accuracy on the human's trajectory and 0.1% of the map when driving,
 * switching sides 0.00 times per second - it had welded itself to one key,
 * because at eval that feature is its own output fed back.
 *
 * With the feature ablated, side accuracy is 0.917 (majority baseline 0.613),
 * so the geometry alone carries the technique. WISH_DIM 0 removes it; set it
 * back to 2 only with a reason.
 */
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

stock void Track_Load()
{
	g_iTrackCount  = 0;
	g_fTrackLength = 0.0;
	g_iTrackHint   = -1;

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

/**
 * Nearest centerline index. A full scan of ~1500 points every tick would cost
 * more than the physics at 5000 ticks/s, so search a window around the previous
 * answer. hint < 0 forces a full scan (after a reset or teleport).
 */
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

/**
 * Project onto the two segments adjacent to the nearest point, so progress is
 * smooth rather than quantised to the 64-unit point spacing. Returns arc length;
 * fills `closest` with the projected point and `outDist` with the distance to it.
 */
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

/**
 * Observation vector. Everything is expressed in a frame attached to the player's
 * horizontal velocity, so the policy sees the same numbers for the same relative
 * situation anywhere on the map.
 *
 *   0  horizontal speed / 1000
 *   1  vertical speed / 1000
 *   2  cos(velocity heading - track tangent)
 *   3  sin(same)
 *   4  signed lateral offset from the line / 256
 *   5  vertical offset from the line / 256
 *   6  progress fraction along the track
 *   7.. next LOOKAHEAD centerline points, player-local, / 512
 */
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

	obs[0] = speed / 1000.0;
	obs[1] = vel[2] / 1000.0;
	obs[2] = Cosine(dYaw);
	obs[3] = Sine(dYaw);
	obs[4] = lateral / 256.0;
	obs[5] = (pos[2] - closest[2]) / 256.0;
	obs[6] = (g_fTrackLength > 0.0) ? (s / g_fTrackLength) : 0.0;

	// lookahead points, rotated into the velocity frame
	float vr = DegToRad(velYaw);
	float cy = Cosine(vr), sy = Sine(vr);

	for (int k = 0; k < LOOKAHEAD; k++)
	{
		int j = idx + g_iLookOffsets[k];
		if (j > g_iTrackCount - 1)
			j = g_iTrackCount - 1;

		float rx = g_fTrackX[j] - pos[0];
		float ry = g_fTrackY[j] - pos[1];
		float rz = g_fTrackZ[j] - pos[2];

		float scale = float(g_iLookOffsets[k]) * 64.0;   // nominal arc distance

		int o = 7 + k * 3;
		obs[o + 0] = ( cy * rx + sy * ry) / scale;   // forward
		obs[o + 1] = (-sy * rx + cy * ry) / scale;   // left
		obs[o + 2] = rz / scale;
	}
}


/**
 * Surface probe - what the agent is actually riding.
 *
 * The centerline says where to GO. It says nothing about where the ramp IS, and
 * once the agent drifts off the line it has no way to find the surface again.
 * Measured on a failed run: at death the bot was 346 units to the side but 502
 * units BELOW the line and falling - it had lost the ramp and could not perceive
 * it. That is why training plateaued at ~7% of the track regardless of the
 * lookahead or discount horizon.
 *
 * Five rays cast down and diagonally in the velocity frame. Output is the
 * per-ray hit distance plus the normal of the nearest hit, expressed in the
 * velocity frame so it is orientation-invariant like the rest of the vector.
 *
 * These CANNOT be recomputed in Python - they need the engine's collision - so
 * they are logged per step in the trajectory instead.
 */
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
		// The level rays reach further: 512 units is under two decisions of
		// warning at surf speed, and these are the ones meant to see a drop
		// coming rather than the ramp already underfoot.
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
