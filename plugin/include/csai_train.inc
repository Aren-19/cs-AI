/**
 * csai_train - the actor side of the training loop.
 *
 * Actor/learner split over files, because this server has no socket extension
 * and per-tick IPC would throttle throughput regardless:
 *
 *   plugin  ->  data/csai/out/batch_NNNN.bin   (trajectories)
 *   plugin  ->  data/csai/out/batch_NNNN.done  (marker: batch complete)
 *   python  ->  data/csai/weights.txt          (new policy, with a generation)
 *
 * The plugin polls for a weights generation newer than the one it is running.
 * In async mode it does not wait at all and just keeps collecting with whatever
 * policy it has - useful for throughput measurement and for checking the data
 * path before any learning exists.
 */

#if defined _csai_train_included
	#endinput
#endif
#define _csai_train_included

bool g_bTrainSync      = true;     // wait for a new policy between batches
bool g_bTrainWaiting   = false;
int  g_iTrainBatches   = 0;        // batches remaining; <0 = unlimited
/**
 * Which actor process this is, when several run in parallel.
 *
 * One srcds instance is single-threaded and saturates at ~5400 ticks/s, so the
 * only way past that on a 12-core machine is more instances. They share the
 * weights file (read-only) and differ only in where their batches land, so the
 * batch name carries the actor id - otherwise every actor would write
 * batch_0000 over the top of the others.
 */
int  g_iActorId        = 0;

float g_fEvalSavedMix  = 0.0;
int  g_iEvalSavedBatch = 64;   // restored after an eval, which borrows it
int  g_iTrainDone      = 0;
bool g_bTrainQuitAfter = false;
float g_fTrainStart    = 0.0;   // start of the current batch
float g_fTrainStartAll = 0.0;   // start of the whole run
int  g_iTrainStepsTotal = 0;

stock void Train_WeightsPath(char[] buf, int maxlen)
{
	BuildPath(Path_SM, buf, maxlen, "data/csai/weights.txt");
}

stock void Train_WriteDoneMarker()
{
	if (g_bBatchBroken)
	{
		PrintToServer("[CsAI] batch %d has no output file - not announcing it", g_iBatchIndex);
		g_bBatchBroken = false;
		return;
	}
	char path[PLATFORM_MAX_PATH];
	BuildPath(Path_SM, path, sizeof(path), "data/csai/out/a%d_batch_%04d.done", g_iActorId, g_iBatchIndex);

	File f = OpenFile(path, "w");
	if (f == null)
		return;

	// A tiny summary so the learner can log progress without parsing the binary.
	// The learner rebuilds every observation from its own copy of the track. If
	// the two ever differ - a regenerated centerline, a half-restarted actor -
	// the importance ratios are computed against a distribution nothing sampled
	// and training quietly degrades with no error anywhere. So the batch states
	// what it was produced against and the learner refuses to mix them.
	f.WriteLine("track_length %.1f", g_fTrackLength);
	f.WriteLine("episodes %d", g_iBatchEpisodes);
	f.WriteLine("gen %d", g_iPolicyGen);
	f.WriteLine("mean_return %.4f", (g_iBatchEpisodes > 0) ? (g_fStatReturnSum / float(g_iBatchEpisodes)) : 0.0);
	f.WriteLine("mean_progress %.4f", (g_iBatchEpisodes > 0) ? (g_fStatProgSum / float(g_iBatchEpisodes)) : 0.0);
	f.WriteLine("fell %d", g_iStatFell);
	f.WriteLine("finished %d", g_iStatFinished);
	f.WriteLine("timeout %d", g_iStatTimeout);
	delete f;
}

stock void Train_ReportBatch()
{
	float elapsed = GetEngineTime() - g_fTrainStart;
	float meanRet  = (g_iBatchEpisodes > 0) ? (g_fStatReturnSum / float(g_iBatchEpisodes)) : 0.0;
	float meanProg = (g_iBatchEpisodes > 0) ? (g_fStatProgSum   / float(g_iBatchEpisodes)) : 0.0;

	PrintToServer("[CsAI] batch %d gen %d | eps %d (mid %d) | return %.2f | progress %.1f%% | fell %d fin %d to %d | %.1fs",
	              g_iBatchIndex, g_iPolicyGen, g_iBatchEpisodes, g_iStatMidStart,
	              meanRet, meanProg * 100.0,
	              g_iStatFell, g_iStatFinished, g_iStatTimeout, elapsed);
}

stock void Train_Begin(int batches, bool sync, bool quitAfter)
{
	if (g_iBot == -1 || !IsClientInGame(g_iBot))
	{
		PrintToServer("[CsAI] train: no bot");
		return;
	}
	if (g_iTrackCount == 0)
	{
		PrintToServer("[CsAI] train: no track loaded for this map");
		return;
	}
	if (g_iStateCount == 0)
	{
		PrintToServer("[CsAI] train: no start states loaded for this map");
		// Quit rather than sit idle: an armed headless run that returns here
		// never exits, so the Python side waits out its whole timeout and
		// reports a timeout instead of the actual reason.
		if (g_bQuitAfter)
			ServerCommand("quit");
		return;
	}
	// Prefer a policy the learner has already produced; random init is only the
	// cold-start fallback.
	char wpath[PLATFORM_MAX_PATH];
	Train_WeightsPath(wpath, sizeof(wpath));
	if (Pol_Load(wpath))
		PrintToServer("[CsAI] starting from policy generation %d", g_iPolicyGen);
	else if (!g_bPolicyLoaded)
	{
		Pol_RandomInit();
		PrintToServer("[CsAI] no weights file, random init (gen 0)");
	}

	g_iTrainBatches   = batches;
	g_bTrainSync      = sync;
	g_bEval           = false;   // never begin training in evaluation mode
	g_iEvalLeft       = 0;
	g_bTrainQuitAfter = quitAfter;
	g_iTrainDone      = 0;
	g_bTrainWaiting   = false;
	g_iTrainStepsTotal = 0;
	g_fTrainStart     = GetEngineTime();
	g_fTrainStartAll  = g_fTrainStart;
	g_bDriving        = false;       // the scripted controller must not also run
	g_bTraining       = true;

	PrintToServer("[CsAI] training: %d batches x %d episodes, sync=%s, states=%d, obs=%d, actions=%d",
	              batches, g_iBatchTarget, sync ? "yes" : "no", g_iStateCount, OBS_DIM, POL_ACTIONS);
	PrintToServer("[CsAI] mixed starts: mix=%.2f window=%.2f..%.2f of %d states",
	              g_fStateMix, g_fStateLo, g_fStateHi, g_iStateCount);

	Ep_OpenBatch();
	Ep_Begin();
}

stock void Train_Stop()
{
	// Cleared here too. It used to be reset only at the natural end of an eval,
	// so stopping one part-way (or changing map during one) left g_bEval set -
	// and the next csai_train routed its first finished episode into
	// Eval_EpisodeDone, printed "eval complete", and stopped. A training run
	// that silently did one episode.
	g_bEval = false;
	g_iEvalLeft = 0;
	g_bTraining = false;
	delete g_hBatchFile;

	float elapsed = GetEngineTime() - g_fTrainStartAll;
	PrintToServer("[CsAI] training stopped after %d batches, %d steps, %.1fs (%.0f steps/s)",
	              g_iTrainDone, g_iTrainStepsTotal, elapsed,
	              (elapsed > 0.0) ? (float(g_iTrainStepsTotal) / elapsed) : 0.0);

	if (g_bTrainQuitAfter)
		ServerCommand("quit");
}

/** Poll for a policy newer than the one we are running. */
stock bool Train_TryLoadNewWeights()
{
	char path[PLATFORM_MAX_PATH];
	Train_WeightsPath(path, sizeof(path));
	if (!FileExists(path))
		return false;

	int before = g_iPolicyGen;
	if (!Pol_Load(path))
		return false;

	if (g_iPolicyGen <= before)
		return false;

	PrintToServer("[CsAI] loaded policy generation %d", g_iPolicyGen);
	return true;
}

stock void Train_EndOfBatch()
{
	delete g_hBatchFile;
	Train_WriteDoneMarker();
	Train_ReportBatch();

	g_iTrainDone++;
	g_iBatchIndex++;

	if (g_iTrainBatches > 0 && g_iTrainDone >= g_iTrainBatches)
	{
		Train_Stop();
		return;
	}

	if (g_bTrainSync)
	{
		g_bTrainWaiting = true;      // Train_Poll resumes us when weights land
		g_fTrainStart = GetEngineTime();
		return;
	}

	g_fTrainStart = GetEngineTime();
	Ep_OpenBatch();
	Ep_Begin();
}

/**
 * Called from OnPlayerRunCmd for the bot while training. Returns Plugin_Changed
 * when it has written inputs.
 */
stock bool Train_Tick(int client, int &buttons, float movement[3], float angles[3])
{
	if (g_bTrainWaiting)
	{
		// idle: hold still rather than flailing while the learner works
		movement[0] = 0.0;
		movement[1] = 0.0;
		movement[2] = 0.0;
		buttons = 0;
		return true;
	}

	if (!Ep_Tick(client, buttons, movement, angles))
	{
		g_iTrainStepsTotal += g_iEpSteps;

		if (g_bEval)
			Eval_EpisodeDone();
		else if (g_iBatchEpisodes >= g_iBatchTarget)
			Train_EndOfBatch();
		else
			Ep_Begin();
	}
	return true;
}

/** Timer-driven: checks for new weights while the actor idles between batches. */
stock void Train_Poll()
{
	if (!g_bTraining || !g_bTrainWaiting)
		return;

	if (Train_TryLoadNewWeights())
	{
		g_bTrainWaiting = false;
		g_fTrainStart = GetEngineTime();
		Ep_OpenBatch();
		Ep_Begin();
	}
}


// ------------------------------------------------------------ evaluation ----
//
// Training reports progress *gained* from random checkpoints, which is the right
// signal for learning but says nothing about whether the agent can run the map.
// Evaluation answers that: greedy (argmax) actions, always from state 0, and the
// only number that counts is whether it reaches the end and in what time.

int   g_iEvalLeft    = 0;
int   g_iEvalDone    = 0;
int   g_iEvalFinish  = 0;
float g_fEvalBestFrac = 0.0;
float g_fEvalBestTime = 0.0;
int   g_iEvalSavedStates = 0;

stock void Eval_Begin(int runs)
{
	if (g_iBot == -1 || !IsClientInGame(g_iBot) || g_iTrackCount == 0 || g_iStateCount == 0)
	{
		PrintToServer("[CsAI] eval: not ready");
		return;
	}

	char wpath[PLATFORM_MAX_PATH];
	Train_WeightsPath(wpath, sizeof(wpath));
	if (!Pol_Load(wpath) && !g_bPolicyLoaded)
	{
		// With no weights every logit is zero, argmax picks action 0 forever, and
		// the run still reports a perfectly plausible "0/N finished, best 3.2%"
		// for a policy that does not exist.
		PrintToServer("[CsAI] eval: no usable weights at %s - refusing to run", wpath);
		if (g_bQuitAfter)
			ServerCommand("quit");
		return;
	}

	g_bEval        = true;
	g_bTraining    = true;      // reuse the episode machinery
	g_bTrainSync   = false;
	g_bTrainWaiting = false;
	g_iTrainBatches = -1;
	g_iEvalLeft    = runs;
	g_iEvalDone    = 0;
	g_iEvalFinish  = 0;
	g_fEvalBestFrac = 0.0;
	g_fEvalBestTime = 0.0;

	// Must hold for the WHOLE eval, not just the first episode: Ep_Begin is
	// called again from Eval_EpisodeDone, and restoring this early made runs 2..N
	// start from random checkpoints. That is why a supposedly deterministic
	// greedy eval produced 3.8% / 27.1% / 34.8% / 45.7% on identical settings.
	g_iEvalSavedStates = g_iTrainStates;
	g_iTrainStates = 1;         // always state 0
	g_fEvalSavedMix = g_fStateMix;
	g_iEvalSavedBatch = g_iBatchTarget;
	g_fStateMix = 0.0;          // never start an eval run mid-map
	g_iBatchTarget = runs + 1;  // never roll a batch mid-eval

	PrintToServer("[CsAI] eval: %d greedy runs from state 0, policy gen %d", runs, g_iPolicyGen);
	Ep_OpenBatch();
	Rec_Start();
	Ep_Begin();
}

stock void Eval_EpisodeDone()
{
	g_iEvalDone++;
	float frac = (g_fTrackLength > 0.0) ? (g_fEpBestS / g_fTrackLength) : 0.0;
	float secs = float(g_iEpTick) * GetTickInterval();

	// Keep the run that got furthest - that is the one worth watching.
	if (frac > g_fEvalBestFrac)
	{
		g_fEvalBestFrac = frac;
		char tag[64];
		Format(tag, sizeof(tag), "gen%d", g_iPolicyGen);
		Rec_Stop();
		Rec_Save(tag, secs);
	}
	Rec_Start();

	// only a completed run has a meaningful time; the duration of a run that fell
	// early is not a "best time"
	if (g_iEpOutcome == EP_FINISHED)
	{
		g_iEvalFinish++;
		if (g_fEvalBestTime <= 0.0 || secs < g_fEvalBestTime)
			g_fEvalBestTime = secs;
	}

	PrintToServer("[CsAI]  eval run %d/%d: %s at %.1f%% in %.2fs",
	              g_iEvalDone, g_iEvalDone + g_iEvalLeft - 1,
	              (g_iEpOutcome == EP_FINISHED) ? "FINISHED" :
	              ((g_iEpOutcome == EP_FELL) ? "fell" : "stopped"),
	              frac * 100.0, secs);

	g_iEvalLeft--;
	if (g_iEvalLeft <= 0)
	{
		if (g_iEvalFinish > 0)
			// g_fRefTime, not a literal: this said "human 39.10s" on every map,
			// and surf_demise's own reference is 39.045.
			PrintToServer("[CsAI] eval complete: %d/%d finished, best %.1f%% of track, best time %.2fs (human %.2fs)",
			              g_iEvalFinish, g_iEvalDone, g_fEvalBestFrac * 100.0, g_fEvalBestTime, g_fRefTime);
		else
			PrintToServer("[CsAI] eval complete: 0/%d finished, best %.1f%% of track",
			              g_iEvalDone, g_fEvalBestFrac * 100.0);
		g_bEval = false;
		g_bTraining = false;
		g_iTrainStates = g_iEvalSavedStates;
		g_fStateMix = g_fEvalSavedMix;
		g_iBatchTarget = g_iEvalSavedBatch;
		delete g_hBatchFile;
		if (g_bTrainQuitAfter)
			ServerCommand("quit");
	}
	else
	{
		Ep_Begin();
	}
}
