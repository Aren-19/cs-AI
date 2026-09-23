#if defined _csai_train_included
	#endinput
#endif
#define _csai_train_included

bool g_bTrainSync      = true;     // wait for a new policy between batches
bool g_bTrainWaiting   = false;
int  g_iTrainBatches   = 0;        // batches remaining; <0 = unlimited
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
	if (g_sSlot[0] == '\0')
		BuildPath(Path_SM, buf, maxlen, "data/csai/weights.txt");
	else
		BuildPath(Path_SM, buf, maxlen, "data/csai/weights_%s.txt", g_sSlot);
}

/** The partner slot's policy: the wind-up for the main run, the main run for a wind-up. */
stock void Train_LoadPartner()
{
	if (g_sPartner[0] == '\0')
		return;
	char path[PLATFORM_MAX_PATH];
	if (StrEqual(g_sPartner, "main"))
		BuildPath(Path_SM, path, sizeof(path), "data/csai/weights.txt");
	else
		BuildPath(Path_SM, path, sizeof(path), "data/csai/weights_%s.txt", g_sPartner);
	int before = g_iPartnerGen;
	if (Pol_Load(path, NET_PARTNER) && g_iPartnerGen != before)
		PrintToServer("[CsAI] partner '%s' at generation %d", g_sPartner, g_iPartnerGen);
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
	if (g_sSlot[0] == '\0')
		BuildPath(Path_SM, path, sizeof(path), "data/csai/out/a%d_batch_%04d.done", g_iActorId, g_iBatchIndex);
	else
		BuildPath(Path_SM, path, sizeof(path), "data/csai/out_%s/a%d_batch_%04d.done", g_sSlot, g_iActorId, g_iBatchIndex);

	File f = OpenFile(path, "w");
	if (f == null)
		return;

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

	Train_LoadPartner();
	if (g_iWindupTicks > 0 && !g_bPartnerLoaded)
	{
		PrintToServer("[CsAI] train: a wind-up needs the main policy to fly its run, and '%s' has no weights", g_sPartner);
		g_bTraining = false;
		if (g_bQuitAfter)
			ServerCommand("quit");
		return;
	}
	Ep_OpenBatch();
	Ep_Begin();
}

stock void Train_Stop()
{
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
	Train_LoadPartner();
	Ep_OpenBatch();
	Ep_Begin();
}

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
		Train_AfterEpisode();
	return true;
}

stock void Train_AfterEpisode()
{
	g_iTrainStepsTotal += g_iEpSteps;

	if (g_bEval)
		Eval_EpisodeDone();
	else if (g_iBatchEpisodes >= g_iBatchTarget)
		Train_EndOfBatch();
	else
		Ep_Begin();
}

/** A bot that dies or leaves mid-episode gets no more ticks, which would stall the actor. */
stock void Train_CheckBot()
{
	if (g_iBot == -1 || !IsClientInGame(g_iBot))
	{
		PrintToServer("[CsAI] bot left mid-run - stopping");
		Train_Stop();
		if (g_bBenchArmed)
			ServerCommand("quit");
		return;
	}
	if (IsPlayerAlive(g_iBot))
		return;

	PrintToServer("[CsAI] bot died mid-run - respawning");
	CS_RespawnPlayer(g_iBot);
	Ep_End(EP_FELL);
	Train_AfterEpisode();
}

/** Timer-driven: checks for new weights while the actor idles between batches. */
stock void Train_Poll()
{
	if (!g_bTraining)
		return;
	if (!g_bTrainWaiting)
	{
		Train_CheckBot();
		return;
	}

	if (Train_TryLoadNewWeights())
	{
		Train_LoadPartner();
		g_bTrainWaiting = false;
		g_fTrainStart = GetEngineTime();
		Ep_OpenBatch();
		Ep_Begin();
	}
}

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

	g_iEvalSavedStates = g_iTrainStates;
	g_iTrainStates = 1;         // always state 0
	g_fEvalSavedMix = g_fStateMix;
	g_iEvalSavedBatch = g_iBatchTarget;
	g_fStateMix = 0.0;          // never start an eval run mid-map
	g_iBatchTarget = runs + 1;  // never roll a batch mid-eval

	Train_LoadPartner();
	PrintToServer("[CsAI] eval: %d greedy runs from state 0, policy gen %d", runs, g_iPolicyGen);
	if (g_iWindupTicks == 0)
		PrintToServer("[CsAI] eval: opening with the %s wind-up",
		              (g_bPartnerLoaded && g_bEvalLearned) ? "learned" : "recorded");
	Ep_OpenBatch();
	Rec_Start();
	Ep_Begin();
}

stock void Eval_EpisodeDone()
{
	g_iEvalDone++;
	float frac = (g_fTrackLength > 0.0) ? (g_fEpBestS / g_fTrackLength) : 0.0;
	int ticks = g_iEpTick - g_iEpPreTicks;
	float secs = float((ticks > 0) ? ticks : 0) * GetTickInterval();
	bool finished = (g_iEpOutcomeRun == EP_FINISHED);

	// Keep the fastest finish, or failing that the run that got furthest.
	bool keep = finished ? (g_iEvalFinish == 0 || secs < g_fEvalBestTime)
	                     : (g_iEvalFinish == 0 && frac > g_fEvalBestFrac);
	if (frac > g_fEvalBestFrac)
		g_fEvalBestFrac = frac;
	if (keep)
	{
		char tag[64];
		Format(tag, sizeof(tag), "gen%d", g_iPolicyGen);
		Rec_Stop();
		Rec_Save(tag, secs);
	}
	Rec_Start();

	if (finished)
	{
		g_iEvalFinish++;
		if (g_fEvalBestTime <= 0.0 || secs < g_fEvalBestTime)
			g_fEvalBestTime = secs;
	}

	char what[16];
	switch (g_iEpOutcomeRun)
	{
		case EP_FINISHED: what = "FINISHED";
		case EP_FELL:     what = "fell";
		case EP_WINDUP:   what = "wind-up";
		case EP_TIMEOUT:  what = "timed out";
		default:          what = "stuck";
	}
	PrintToServer("[CsAI]  eval run %d/%d: %s at %.1f%% in %.2fs",
	              g_iEvalDone, g_iEvalDone + g_iEvalLeft - 1, what, frac * 100.0, secs);

	g_iEvalLeft--;
	if (g_iEvalLeft <= 0)
	{
		if (g_iEvalFinish > 0)
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
