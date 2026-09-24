#include <sourcemod>
#include <sdktools>
#include <cstrike>
#include <commandline>

#include "csai_bot.inc"
#include "csai_track.inc"
#include "csai_policy.inc"
#include "csai_replay.inc"
#include "csai_prestrafe.inc"
#include "csai_demo.inc"
#include "csai_record.inc"
#include "csai_episode.inc"
#include "csai_train.inc"

#pragma semicolon 1
#pragma newdecls required

#define CSAI_VERSION  "0.1.0"
#define AIR_SPEED_CAP 30.0      // Source clamps wishspd to this in AirMove()
#define REC_FIELDS    17

public Plugin myinfo =
{
    name        = "CsAI Harness",
    author      = "numan",
    description = "Bot control, telemetry and throughput benchmark for surf AI training",
    version     = CSAI_VERSION,
    url         = ""
};

// ---------------------------------------------------------------- state ----
bool      g_bDriving       = false;
int       g_iTicksLeft     = 0;
int       g_iRecTick       = 0;
float     g_fStartEngine   = 0.0;
ArrayList g_hTraj          = null;
char      g_sRunTag[64];

bool      g_bHaveSave      = false;
float     g_fSaveOrigin[3];
float     g_fSaveAngles[3];
float     g_fSaveVel[3];

int       g_iStrafeSide    = 1;     // +1 = sidemove right (D), -1 = left (A)
bool      g_bRecord        = true;

ConVar    g_cvAirAccel;

int       g_iAutoTicks     = 3000;
bool      g_bQuitAfter     = false;
float     g_fAutoTimescale = 1.0;

int       g_iLastMoveType  = -1;
int       g_iLastFlags     = 0;

ConVar    g_cvBenchTicks;
ConVar    g_cvBenchScale;
ConVar    g_cvBenchQuit;
ConVar    g_cvBenchDelay;
float     g_fBenchHeight   = 4000.0;
int       g_iBenchState    = 0;
int       g_iTrainBatchesCL = 0;
bool      g_bTrainSyncCL    = true;
int       g_iObsDumpCL      = 0;
int       g_iEvalCL         = 0;
int       g_iDemoCL         = 0;
bool      g_bBenchArmed    = false;

ConVar    g_cvTimescale;
ConVar    g_cvFpsMax;
ConVar    g_cvCheats;

Handle    g_hPollTimer      = null;

// ----------------------------------------------------------------- init ----
public void OnPluginStart()
{
    g_hTraj = new ArrayList(REC_FIELDS);
    g_cvAirAccel  = FindConVar("sv_airaccelerate");
    g_cvTimescale = FindConVar("host_timescale");
    g_cvFpsMax    = FindConVar("fps_max");
    g_cvCheats    = FindConVar("sv_cheats");

    ConVar hib = FindConVar("sv_hibernate_when_empty");
    if (hib != null)
    {
        hib.SetInt(0, false, false);
        PrintToServer("[CsAI] sv_hibernate_when_empty -> 0");
    }

    g_cvBenchTicks = CreateConVar("csai_bench_ticks", "0",
        "Ticks to simulate on map start. 0 = benchmark disabled.");
    g_cvBenchScale = CreateConVar("csai_bench_timescale", "1.0",
        "host_timescale to apply for the scripted benchmark run.");
    g_cvBenchQuit  = CreateConVar("csai_bench_quit", "0",
        "Quit the server once the scripted benchmark finishes.");
    g_cvBenchDelay = CreateConVar("csai_bench_delay", "6.0",
        "Seconds to wait after configs execute before the benchmark starts.");

    RegServerCmd("csai_spawn",  Cmd_Spawn,  "Create the AI fake client");
    RegServerCmd("csai_kick",   Cmd_Kick,   "Remove the AI fake client");
    RegServerCmd("csai_run",    Cmd_Run,    "csai_run <ticks> - drive from the current position");
    RegServerCmd("csai_air",    Cmd_Air,    "csai_air <ticks> [height] - drop test, proves the strafe math");
    RegServerCmd("csai_stop",   Cmd_Stop,   "Stop driving and dump the trajectory");
    RegServerCmd("csai_save",   Cmd_Save,   "Save the bot's full movement state");
    RegServerCmd("csai_load",   Cmd_Load,   "Restore the saved movement state");
    RegServerCmd("csai_side",   Cmd_Side,   "csai_side <1|-1> - strafe hand");
    RegServerCmd("csai_record", Cmd_Record, "csai_record <0|1> - per-tick logging on/off");
    RegServerCmd("csai_status", Cmd_Status, "Print harness status");
    RegServerCmd("csai_auto",   Cmd_Auto,   "csai_auto <ticks> <delay> <quit> <timescale> - scripted headless run");
    RegServerCmd("csai_timescale", Cmd_Timescale, "csai_timescale <n> - uncap fps and set host_timescale");
    RegServerCmd("csai_state",  Cmd_State,  "csai_state <idx> - teleport the bot to a replay start state");
    RegServerCmd("csai_states", Cmd_States, "Reload the start-states file for the current map");
    RegServerCmd("csai_train",  Cmd_Train,  "csai_train <batches> <sync 0|1> <quit 0|1> - start training");
    RegServerCmd("csai_train_stop", Cmd_TrainStop, "Stop training");
    RegServerCmd("csai_democapture", Cmd_DemoCapture, "Replay the human run and capture behaviour-cloning data");

    // Typed in chat as !csai_save / !csai_drop / !csai_runs
    RegConsoleCmd("sm_csai_save", Cmd_RecSave, "Save the run you just did");
    RegConsoleCmd("sm_csai_drop", Cmd_RecDrop, "Throw away the current recording and start over");
    RegConsoleCmd("sm_csai_runs", Cmd_RecRuns, "How many recorded runs this map has");
    RegServerCmd("csai_prestrafe", Cmd_Prestrafe, "csai_prestrafe <0|1> - replay the recorded prestrafe before each run");
    RegServerCmd("csai_scripted", Cmd_Scripted, "csai_scripted <0|1> - hand-coded controller (interface test)");
    RegServerCmd("csai_eval",   Cmd_Eval,   "csai_eval <runs> - greedy evaluation from state 0");
    RegServerCmd("csai_obsdump", Cmd_ObsDump, "csai_obsdump <n> - dump n observation rows for the parity check");
    RegServerCmd("csai_cfg",    Cmd_Cfg,    "csai_cfg <key> <value> - tune batch/frameskip/deviation/states");

    g_hPollTimer = CreateTimer(0.25, Timer_TrainPoll, _, TIMER_REPEAT | TIMER_FLAG_NO_MAPCHANGE);

    HookEvent("player_spawn", Event_PlayerSpawn, EventHookMode_Post);

    PrintToServer("[CsAI] harness %s loaded", CSAI_VERSION);
}

public void Event_PlayerSpawn(Event event, const char[] name, bool dontBroadcast)
{
    int client = GetClientOfUserId(event.GetInt("userid"));
    if (client != g_iBot)
        return;

    float o[3];
    GetClientAbsOrigin(client, o);
    PrintToServer("[CsAI] EVENT player_spawn t=%.3f origin=(%.0f %.0f %.0f) driving=%s",
                  GetGameTime(), o[0], o[1], o[2], g_bDriving ? "yes" : "no");
}

public void OnMapEnd()
{
    g_hPollTimer = null;
}

public void OnMapStart()
{
    if (g_bTraining)
    {
        // Should be unreachable now that map rotation is disabled, but a silent
        // stall here cost a whole run once - so say so loudly.
        PrintToServer("[CsAI] FATAL: map changed while training; aborting run");
        g_bTraining = false;
        delete g_hBatchFile;
        if (g_bTrainQuitAfter)
            ServerCommand("quit");
    }

    // The map change that just happened killed the poll timer. Nothing else
    // recreates it, so without this the plugin is inert from here on.
    if (g_hPollTimer == null)
        g_hPollTimer = CreateTimer(0.25, Timer_TrainPoll, _, TIMER_REPEAT | TIMER_FLAG_NO_MAPCHANGE);

    Track_Load();
    Zone_Load();
    LoadStates();

    Pre_Load();          // single-file fallback, if no recordings exist
    Demo_Load();
    Pre_BuildFromDemos();   // prefer the recorded prestrafes
    Demo_LoadIndex(0);      // leave run 0 loaded for demo capture
    ArmBenchmark();
}

public void OnConfigsExecuted()
{
    ArmBenchmark();
}

void ArmBenchmark()
{
    if (g_bBenchArmed)
        return;

    // command line wins, cvar is the fallback
    int   ticks = GetCommandLineParamInt("+csai_bench_ticks", g_cvBenchTicks.IntValue);
    float scale = GetCommandLineParamFloat("+csai_bench_timescale", g_cvBenchScale.FloatValue);
    int   quit  = GetCommandLineParamInt("+csai_bench_quit", g_cvBenchQuit.IntValue);
    float delay = GetCommandLineParamFloat("+csai_bench_delay", g_cvBenchDelay.FloatValue);
    g_fBenchHeight = GetCommandLineParamFloat("+csai_bench_height", 1500.0);
    g_iBenchState  = GetCommandLineParamInt("+csai_bench_state", 0);

    // training can be armed from the command line the same way the benchmark is
    g_iTrainBatchesCL = GetCommandLineParamInt("+csai_train_batches", 0);
    g_bTrainSyncCL    = (GetCommandLineParamInt("+csai_train_sync", 1) != 0);
    g_iObsDumpCL      = GetCommandLineParamInt("+csai_obsdump", 0);
    g_iEvalCL         = GetCommandLineParamInt("+csai_eval", 0);
    g_iDemoCL         = GetCommandLineParamInt("+csai_democapture", 0);
    g_bScripted       = (GetCommandLineParamInt("+csai_scripted", 0) != 0);
    g_bPrestrafeEnabled = (GetCommandLineParamInt("+csai_prestrafe", 1) != 0);
    g_iBatchTarget    = GetCommandLineParamInt("+csai_batch", g_iBatchTarget);
    g_iFrameSkip      = GetCommandLineParamInt("+csai_frameskip", g_iFrameSkip);
    g_iTrainStates    = GetCommandLineParamInt("+csai_states", g_iTrainStates);
    g_fStateMix       = GetCommandLineParamFloat("+csai_statemix", g_fStateMix);
    g_iForceSide      = GetCommandLineParamInt("+csai_forceside", g_iForceSide);
    g_iForceTrim      = GetCommandLineParamInt("+csai_forcetrim", g_iForceTrim);
    if (g_iForceSide != 0 || g_iForceTrim >= 0)
        PrintToServer("[CsAI] FORCE side=%d trim=%d", g_iForceSide, g_iForceTrim);
    g_fStateLo        = GetCommandLineParamFloat("+csai_statelo", g_fStateLo);
    g_fStateHi        = GetCommandLineParamFloat("+csai_statehi", g_fStateHi);
    g_iEpTickBudget   = GetCommandLineParamInt("+csai_budget", g_iEpTickBudget);
    g_fMaxDeviation   = GetCommandLineParamFloat("+csai_deviation", g_fMaxDeviation);
    g_bEvalGreedy     = (GetCommandLineParamInt("+csai_evalgreedy", 1) != 0);
    g_iActorId        = GetCommandLineParamInt("+csai_actor", 0);
    g_fSwitchCost     = GetCommandLineParamFloat("+csai_switchcost", g_fSwitchCost);
    g_fTimeCost       = GetCommandLineParamFloat("+csai_timecost", g_fTimeCost);
    g_fFinishBonus    = GetCommandLineParamFloat("+csai_finishbonus", g_fFinishBonus);
    g_fFinishFloor    = GetCommandLineParamFloat("+csai_finishfloor", g_fFinishFloor);
    g_fTimePower      = GetCommandLineParamFloat("+csai_timepower", g_fTimePower);
    GetCommandLineParam("+csai_slot", g_sSlot, sizeof(g_sSlot), "");
    GetCommandLineParam("+csai_partner", g_sPartner, sizeof(g_sPartner), "");
    g_fLearnedMix     = GetCommandLineParamFloat("+csai_learnedmix", g_fLearnedMix);
    g_bEvalLearned    = (GetCommandLineParamInt("+csai_evallearned", 1) != 0);
    g_iWindupMax      = GetCommandLineParamInt("+csai_windupmax", g_iWindupMax);
    g_fJumpInset      = GetCommandLineParamFloat("+csai_jumpinset", g_fJumpInset);
    g_fStartSpeedCap  = GetCommandLineParamFloat("+csai_startcap", g_fStartSpeedCap);
    g_iWindupTicks    = GetCommandLineParamInt("+csai_windup", g_iWindupTicks);
    g_fWindupYawCap   = GetCommandLineParamFloat("+csai_windupyaw", g_fWindupYawCap);
    g_iWindupHold     = GetCommandLineParamInt("+csai_winduphold", g_iWindupHold);
    g_fTrimCost       = GetCommandLineParamFloat("+csai_trimcost", g_fTrimCost);
    g_fDeviationCost  = GetCommandLineParamFloat("+csai_devcost", g_fDeviationCost);
    g_iPitchMode      = GetCommandLineParamInt("+csai_pitch", g_iPitchMode);
    g_fPitchFixed     = GetCommandLineParamFloat("+csai_pitchfixed", g_fPitchFixed);
    int seed          = GetCommandLineParamInt("+csai_seed", 0);
    if (seed != 0)
        Pol_Seed(seed);
    if (g_iFrameSkip < 1)
        g_iFrameSkip = 1;

    PrintToServer("[CsAI] reward: finish %.1f x (reference / time) ^ %.1f, never below %.1f",
                  g_fFinishBonus, g_fTimePower, g_fFinishFloor);
    if (g_fRefTime > 0.0)
        PrintToServer("[CsAI] reward: a finish pays %.1f at the reference %.3fs, %.1f at 40.0s, %.1f at 42.0s",
                      g_fFinishBonus, g_fRefTime,
                      g_fFinishBonus * Pow(g_fRefTime / 40.0, g_fTimePower),
                      g_fFinishBonus * Pow(g_fRefTime / 42.0, g_fTimePower));
    PrintToServer("[CsAI] reward: timecost %.3f per decision, devcost %.2f, switchcost %.2f, trimcost %.2f",
                  g_fTimeCost, g_fDeviationCost, g_fSwitchCost, g_fTrimCost);
    if (g_iWindupTicks > 0)
        PrintToServer("[CsAI] wind-up training: up to %d ground ticks, view capped at %.1f deg/tick, key held %d ticks, run flown by '%s'",
                      g_iWindupMax, g_fWindupYawCap, g_iWindupHold, g_sPartner);
    else
        PrintToServer("[CsAI] opening: learned wind-up from '%s' on %.0f%% of runs, recorded on the rest",
                      g_sPartner, g_fLearnedMix * 100.0);

    char cmdline[512];
    GetCommandLine(cmdline, sizeof(cmdline));
    PrintToServer("[CsAI] OnConfigsExecuted: ticks=%d scale=%.2f quit=%d delay=%.1f",
                  ticks, scale, quit, delay);
    PrintToServer("[CsAI] cmdline: %s", cmdline);

    if (ticks <= 0 && g_iTrainBatchesCL <= 0 && g_iEvalCL <= 0 && g_iDemoCL <= 0)
    {
        PrintToServer("[CsAI] nothing armed (no bench ticks, no train batches, no eval)");
        return;
    }

    g_bBenchArmed    = true;
    g_iAutoTicks     = ticks;
    g_fAutoTimescale = (scale > 0.0) ? scale : 1.0;
    g_bQuitAfter     = (quit != 0);
    if (delay < 1.0)
        delay = 1.0;

    PrintToServer("[CsAI] benchmark armed: %d ticks, timescale %.2f, starting in %.1fs",
                  ticks, g_fAutoTimescale, delay);

    SetupRound();                      // early: mp_restartgame must settle first
    CreateTimer(delay, Timer_AutoSpawn);
}

public void OnClientDisconnect(int client)
{
    if (client == g_iBot)
    {
        g_iBot     = -1;
        g_bDriving = false;
    }
}

// ------------------------------------------------------------- the loop ----
public Action OnPlayerRunCmd(int client, int &buttons, int &impulse, float vel[3],
                             float angles[3], int &weapon, int &subtype, int &cmdnum,
                             int &tickcount, int &seed, int mouse[2])
{
    if (client != g_iBot && !IsFakeClient(client) && IsPlayerAlive(client))
    {
        Rec_Human_Track(client);
        Rec_Human_Tick(client, buttons, angles);
        return Plugin_Continue;
    }

    if (g_bDemoActive && client == g_iBot && IsPlayerAlive(client))
    {
        if (!Demo_Tick(client, buttons, vel, angles))
        {
            if (g_bQuitAfter)
                ServerCommand("quit");
        }
        return Plugin_Changed;
    }

    if (g_bTraining && client == g_iBot && IsPlayerAlive(client))
    {
        Train_Tick(client, buttons, vel, angles);
        return Plugin_Changed;
    }

    if (!g_bDriving || client != g_iBot || !IsPlayerAlive(client))
        return Plugin_Continue;

    float origin[3], velocity[3], eyes[3], normal[3];
    GetClientAbsOrigin(client, origin);
    GetEntPropVector(client, Prop_Data, "m_vecVelocity", velocity);
    GetClientEyeAngles(client, eyes);
    g_iLastFlags    = GetEntityFlags(client);
    g_iLastMoveType = view_as<int>(GetEntityMoveType(client));
    bool onGround   = (g_iLastFlags & FL_ONGROUND) != 0;
    GetGroundNormal(client, origin, normal);

    if (g_bRecord)
        RecordTick(origin, velocity, eyes, onGround, normal);
    else
        g_iRecTick++;

    // ---- controller: speed-optimal air strafe ----
    float viewYaw = ComputeStrafeYaw(client, velocity);

    angles[0] = 0.0;
    angles[1] = viewYaw;
    angles[2] = 0.0;

    vel[0] = 0.0;                                 // forwardmove
    vel[1] = 400.0 * float(g_iStrafeSide);        // sidemove
    vel[2] = 0.0;                                 // upmove

    buttons = (g_iStrafeSide > 0) ? IN_MOVERIGHT : IN_MOVELEFT;

    // keep the entity's own view in sync (demos / spectators / eye-angle reads)
    float setAng[3];
    setAng[0] = 0.0; setAng[1] = viewYaw; setAng[2] = 0.0;
    TeleportEntity(client, NULL_VECTOR, setAng, NULL_VECTOR);

    if (--g_iTicksLeft <= 0)
        StopRun();

    return Plugin_Changed;
}

float ComputeStrafeYaw(int client, const float velocity[3])
{
    float speed  = SquareRoot(velocity[0] * velocity[0] + velocity[1] * velocity[1]);
    float velYaw = (speed > 0.1) ? RadToDeg(ArcTangent2(velocity[1], velocity[0])) : 0.0;

    float maxSpeed = GetEntPropFloat(client, Prop_Data, "m_flMaxspeed");
    if (maxSpeed <= 0.0)
        maxSpeed = 250.0;

    float airaccel  = (g_cvAirAccel != null) ? g_cvAirAccel.FloatValue : 10.0;
    float frametime = GetTickInterval();

    float wishspeed = maxSpeed;
    float wishspd   = (wishspeed > AIR_SPEED_CAP) ? AIR_SPEED_CAP : wishspeed;
    float accelCap  = airaccel * wishspeed * frametime;

    float theta;
    if (speed < 1.0)
    {
        theta = 0.0;
    }
    else
    {
        float c = (wishspd - accelCap) / speed;
        if (c > 1.0) c = 1.0;
        if (c < 0.0) c = 0.0;          // uncapped regime: perpendicular is optimal
        theta = RadToDeg(ArcCosine(c));
    }

    // wish direction sits theta degrees off the velocity, on the strafing side
    float wishYaw = velYaw - float(g_iStrafeSide) * theta;

    // sidemove>0 => wishdir is the view's right vector  (yaw - 90)
    // sidemove<0 => wishdir is the view's left vector   (yaw + 90)
    return NormalizeYaw((g_iStrafeSide > 0) ? (wishYaw + 90.0) : (wishYaw - 90.0));
}

float NormalizeYaw(float yaw)
{
    while (yaw >  180.0) yaw -= 360.0;
    while (yaw < -180.0) yaw += 360.0;
    return yaw;
}

void GetGroundNormal(int client, const float origin[3], float normal[3])
{
    float start[3], end[3];
    start[0] = origin[0]; start[1] = origin[1]; start[2] = origin[2] + 8.0;
    end[0]   = origin[0]; end[1]   = origin[1]; end[2]   = origin[2] - 72.0;

    Handle tr = TR_TraceRayFilterEx(start, end, MASK_PLAYERSOLID, RayType_EndPoint,
                                    TraceFilterNotSelf, client);
    if (tr != null && TR_DidHit(tr))
        TR_GetPlaneNormal(tr, normal);
    else
        normal[0] = normal[1] = normal[2] = 0.0;

    delete tr;
}

public bool TraceFilterNotSelf(int entity, int mask, any data)
{
    return entity != data;
}

void RecordTick(const float o[3], const float v[3], const float a[3], bool ground, const float n[3])
{
    float row[REC_FIELDS];
    row[0]  = float(g_iRecTick);
    row[1]  = o[0]; row[2]  = o[1]; row[3]  = o[2];
    row[4]  = v[0]; row[5]  = v[1]; row[6]  = v[2];
    row[7]  = SquareRoot(v[0] * v[0] + v[1] * v[1]);
    row[8]  = a[0]; row[9]  = a[1];
    row[10] = ground ? 1.0 : 0.0;
    row[11] = n[0]; row[12] = n[1]; row[13] = n[2];
    row[14] = GetGameTime();
    row[15] = float(g_iLastMoveType);
    row[16] = float(g_iLastFlags);
    g_hTraj.PushArray(row, REC_FIELDS);
    g_iRecTick++;
}

// --------------------------------------------------------- run lifecycle ----
void StartRun(int ticks, const char[] tag)
{
    if (g_iBot == -1 || !IsClientInGame(g_iBot))
    {
        PrintToServer("[CsAI] no bot - run csai_spawn first");
        return;
    }

    g_hTraj.Clear();
    g_iRecTick     = 0;
    g_iTicksLeft   = ticks;
    g_bDriving     = true;
    g_fStartEngine = GetEngineTime();
    strcopy(g_sRunTag, sizeof(g_sRunTag), tag);

    PrintToServer("[CsAI] run %s started: %d ticks, side=%d, airaccel=%.0f",
                  tag, ticks, g_iStrafeSide,
                  (g_cvAirAccel != null) ? g_cvAirAccel.FloatValue : 0.0);
    PrintToServer("[CsAI]  at run time: sv_cheats=%d host_timescale=%.2f fps_max=%d",
                  (g_cvCheats    != null) ? g_cvCheats.IntValue      : -1,
                  (g_cvTimescale != null) ? g_cvTimescale.FloatValue : -1.0,
                  (g_cvFpsMax    != null) ? g_cvFpsMax.IntValue      : -1);
}

void StopRun()
{
    if (!g_bDriving)
        return;

    g_bDriving = false;

    float realElapsed = GetEngineTime() - g_fStartEngine;
    float interval    = GetTickInterval();
    float gameSeconds = float(g_iRecTick) * interval;

    float tps     = (realElapsed > 0.0) ? (float(g_iRecTick) / realElapsed) : 0.0;
    float speedup = (realElapsed > 0.0) ? (gameSeconds / realElapsed) : 0.0;

    PrintToServer("[CsAI] ---------------- run %s complete ----------------", g_sRunTag);
    PrintToServer("[CsAI]  ticks simulated : %d", g_iRecTick);
    PrintToServer("[CsAI]  game time       : %.2f s", gameSeconds);
    PrintToServer("[CsAI]  real time       : %.2f s", realElapsed);
    PrintToServer("[CsAI]  throughput      : %.0f ticks/s", tps);
    PrintToServer("[CsAI]  speedup         : %.2fx realtime", speedup);

    ReportSpeedProfile();

    if (g_bRecord)
        DumpTrajectory();

    if (g_bQuitAfter)
    {
        PrintToServer("[CsAI] auto: shutting down");
        ServerCommand("quit");
    }
}

void ReportSpeedProfile()
{
    int n = g_hTraj.Length;
    if (n == 0)
        return;

    float row[REC_FIELDS];
    float best = 0.0;
    for (int i = 0; i < n; i++)
    {
        g_hTraj.GetArray(i, row, REC_FIELDS);
        if (row[7] > best)
            best = row[7];
    }

    g_hTraj.GetArray(0, row, REC_FIELDS);
    float first = row[7];
    g_hTraj.GetArray(n - 1, row, REC_FIELDS);
    float last = row[7];

    PrintToServer("[CsAI]  h-speed         : start %.1f -> end %.1f (peak %.1f) u/s",
                  first, last, best);
    PrintToServer("[CsAI]  movetype/flags  : %d / 0x%X", g_iLastMoveType, g_iLastFlags);
}

void DumpTrajectory()
{
    char dir[PLATFORM_MAX_PATH];
    BuildPath(Path_SM, dir, sizeof(dir), "logs/csai");
    if (!DirExists(dir))
        CreateDirectory(dir, 511);

    char path[PLATFORM_MAX_PATH];
    BuildPath(Path_SM, path, sizeof(path), "logs/csai/%s.csv", g_sRunTag);

    File f = OpenFile(path, "w");
    if (f == null)
    {
        PrintToServer("[CsAI]  WARNING: could not open %s for writing", path);
        return;
    }

    f.WriteLine("tick,x,y,z,vx,vy,vz,hspeed,pitch,yaw,onground,nx,ny,nz,gametime,movetype,flags");

    float row[REC_FIELDS];
    int n = g_hTraj.Length;
    for (int i = 0; i < n; i++)
    {
        g_hTraj.GetArray(i, row, REC_FIELDS);
        f.WriteLine("%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.2f,%.2f,%d,%.4f,%.4f,%.4f,%.3f,%d,%d",
                    RoundToNearest(row[0]),
                    row[1], row[2], row[3],
                    row[4], row[5], row[6], row[7],
                    row[8], row[9],
                    RoundToNearest(row[10]),
                    row[11], row[12], row[13],
                    row[14],
                    RoundToNearest(row[15]),
                    RoundToNearest(row[16]));
    }
    delete f;

    PrintToServer("[CsAI]  trajectory      : %s (%d rows)", path, n);
}

// ------------------------------------------------------------- commands ----
public Action Cmd_Spawn(int args)
{
    if (g_iBot != -1 && IsClientInGame(g_iBot))
    {
        PrintToServer("[CsAI] bot already present (client %d)", g_iBot);
        return Plugin_Handled;
    }

    int bot = CreateFakeClient("CsAI");
    if (bot == 0)
    {
        PrintToServer("[CsAI] CreateFakeClient failed - is there a free slot?");
        return Plugin_Handled;
    }

    g_iBot = bot;
    CS_SwitchTeam(bot, CS_TEAM_CT);
    CS_RespawnPlayer(bot);

    PrintToServer("[CsAI] bot spawned as client %d", bot);
    return Plugin_Handled;
}

public Action Cmd_Kick(int args)
{
    if (g_iBot != -1 && IsClientInGame(g_iBot))
        KickClient(g_iBot, "CsAI harness");
    g_iBot     = -1;
    g_bDriving = false;
    PrintToServer("[CsAI] bot removed");
    return Plugin_Handled;
}

public Action Cmd_Run(int args)
{
    char buf[32];
    int ticks = 1000;
    if (args >= 1)
    {
        GetCmdArg(1, buf, sizeof(buf));
        ticks = StringToInt(buf);
    }
    if (ticks <= 0)
        ticks = 1000;

    StartRun(ticks, "run");
    return Plugin_Handled;
}

public Action Cmd_Air(int args)
{
    if (g_iBot == -1 || !IsClientInGame(g_iBot))
    {
        PrintToServer("[CsAI] no bot - run csai_spawn first");
        return Plugin_Handled;
    }

    char buf[32];
    int   ticks  = 400;
    float height = 2000.0;

    if (args >= 1) { GetCmdArg(1, buf, sizeof(buf)); ticks  = StringToInt(buf); }
    if (args >= 2) { GetCmdArg(2, buf, sizeof(buf)); height = StringToFloat(buf); }
    if (ticks <= 0) ticks = 400;

    // Lift straight up from where the bot stands and give it a small nudge so the
    // whole run is airborne. If the strafe math is right, h-speed climbs on its own.
    float origin[3], angles[3], velocity[3];
    GetClientAbsOrigin(g_iBot, origin);
    origin[2] += height;

    angles[0] = 0.0; angles[1] = 0.0; angles[2] = 0.0;
    velocity[0] = 30.0; velocity[1] = 0.0; velocity[2] = 0.0;

    TeleportEntity(g_iBot, origin, angles, velocity);

    PrintToServer("[CsAI] air test: lifted +%.0f units", height);
    StartRun(ticks, "airtest");
    return Plugin_Handled;
}

float LiftBot(int client, float requested)
{
    float origin[3], up[3];
    GetClientAbsOrigin(client, origin);

    up[0] = origin[0];
    up[1] = origin[1];
    up[2] = origin[2] + requested + 128.0;

    Handle tr = TR_TraceRayFilterEx(origin, up, MASK_PLAYERSOLID, RayType_EndPoint,
                                    TraceFilterNotSelf, client);
    float headroom = requested;
    if (tr != null && TR_DidHit(tr))
    {
        float hit[3];
        TR_GetEndPosition(hit, tr);
        headroom = (hit[2] - origin[2]) - 96.0;    // keep clear of the ceiling
        if (headroom < 0.0)
            headroom = 0.0;
    }
    delete tr;

    float angles[3], velocity[3];
    angles[0] = 0.0; angles[1] = 0.0; angles[2] = 0.0;
    velocity[0] = 30.0; velocity[1] = 0.0; velocity[2] = 0.0;

    origin[2] += headroom;
    TeleportEntity(client, origin, angles, velocity);

    PrintToServer("[CsAI] lift: requested %.0f, headroom %.0f -> z=%.0f",
                  requested, headroom, origin[2]);
    return headroom;
}

void UnfreezeBot()
{
    if (g_iBot == -1 || !IsClientInGame(g_iBot))
        return;

    int flags = GetEntityFlags(g_iBot);
    if (flags & FL_FROZEN)
    {
        SetEntityFlags(g_iBot, flags & ~FL_FROZEN);
        PrintToServer("[CsAI] cleared FL_FROZEN");
    }
    SetEntityMoveType(g_iBot, MOVETYPE_WALK);
    SetEntPropFloat(g_iBot, Prop_Data, "m_flLaggedMovementValue", 1.0);
}

public Action Cmd_Stop(int args)
{
    if (!g_bDriving)
    {
        PrintToServer("[CsAI] not running");
        return Plugin_Handled;
    }
    StopRun();
    return Plugin_Handled;
}

public Action Cmd_Save(int args)
{
    if (g_iBot == -1 || !IsClientInGame(g_iBot))
    {
        PrintToServer("[CsAI] no bot");
        return Plugin_Handled;
    }

    GetClientAbsOrigin(g_iBot, g_fSaveOrigin);
    GetClientEyeAngles(g_iBot, g_fSaveAngles);
    GetEntPropVector(g_iBot, Prop_Data, "m_vecVelocity", g_fSaveVel);
    g_bHaveSave = true;

    PrintToServer("[CsAI] saved: pos(%.1f %.1f %.1f) vel(%.1f %.1f %.1f)",
                  g_fSaveOrigin[0], g_fSaveOrigin[1], g_fSaveOrigin[2],
                  g_fSaveVel[0], g_fSaveVel[1], g_fSaveVel[2]);
    return Plugin_Handled;
}

public Action Cmd_Load(int args)
{
    if (!g_bHaveSave)
    {
        PrintToServer("[CsAI] nothing saved");
        return Plugin_Handled;
    }
    if (g_iBot == -1 || !IsClientInGame(g_iBot))
    {
        PrintToServer("[CsAI] no bot");
        return Plugin_Handled;
    }

    TeleportEntity(g_iBot, g_fSaveOrigin, g_fSaveAngles, g_fSaveVel);
    PrintToServer("[CsAI] restored saved state");
    return Plugin_Handled;
}

public Action Cmd_Side(int args)
{
    char buf[32];
    if (args >= 1)
    {
        GetCmdArg(1, buf, sizeof(buf));
        g_iStrafeSide = (StringToInt(buf) < 0) ? -1 : 1;
    }
    PrintToServer("[CsAI] strafe side = %d", g_iStrafeSide);
    return Plugin_Handled;
}

public Action Cmd_Record(int args)
{
    char buf[32];
    if (args >= 1)
    {
        GetCmdArg(1, buf, sizeof(buf));
        g_bRecord = (StringToInt(buf) != 0);
    }
    PrintToServer("[CsAI] per-tick recording = %s", g_bRecord ? "on" : "off");
    return Plugin_Handled;
}

public Action Cmd_Status(int args)
{
    PrintToServer("[CsAI] version %s", CSAI_VERSION);
    PrintToServer("[CsAI]  bot client   : %d%s", g_iBot,
                  (g_iBot != -1 && IsClientInGame(g_iBot)) ? " (in game)" : "");
    PrintToServer("[CsAI]  driving      : %s", g_bDriving ? "yes" : "no");
    PrintToServer("[CsAI]  ticks left   : %d", g_iTicksLeft);
    PrintToServer("[CsAI]  recording    : %s", g_bRecord ? "on" : "off");
    PrintToServer("[CsAI]  strafe side  : %d", g_iStrafeSide);
    PrintToServer("[CsAI]  tick interval: %.4f s (%.0f tick)",
                  GetTickInterval(), 1.0 / GetTickInterval());
    PrintToServer("[CsAI]  sv_airaccel  : %.0f",
                  (g_cvAirAccel != null) ? g_cvAirAccel.FloatValue : 0.0);
    return Plugin_Handled;
}

void ApplyTimescale(float scale)
{
    ServerCommand("shavit_core_disable_sv_cheats 0");
    ServerCommand("sv_cheats 1");
    if (g_cvFpsMax != null)
        g_cvFpsMax.SetInt(0, false, false);      // uncapped: let the loop run flat out
    if (g_cvTimescale != null)
        g_cvTimescale.SetFloat(scale, false, false);

    PrintToServer("[CsAI] timescale=%.2f fps_max=%d sv_cheats=%d",
                  (g_cvTimescale != null) ? g_cvTimescale.FloatValue : -1.0,
                  (g_cvFpsMax    != null) ? g_cvFpsMax.IntValue      : -1,
                  (g_cvCheats    != null) ? g_cvCheats.IntValue      : -1);
}

public Action Cmd_Timescale(int args)
{
    char buf[32];
    float scale = 1.0;
    if (args >= 1) { GetCmdArg(1, buf, sizeof(buf)); scale = StringToFloat(buf); }
    if (scale <= 0.0) scale = 1.0;
    ApplyTimescale(scale);
    return Plugin_Handled;
}

public Action Cmd_Auto(int args)
{
    char buf[32];
    float delay = 5.0;
    g_iAutoTicks = 3000;
    g_bQuitAfter = false;

    if (args >= 1) { GetCmdArg(1, buf, sizeof(buf)); g_iAutoTicks = StringToInt(buf); }
    if (args >= 2) { GetCmdArg(2, buf, sizeof(buf)); delay        = StringToFloat(buf); }
    if (args >= 3) { GetCmdArg(3, buf, sizeof(buf)); g_bQuitAfter = (StringToInt(buf) != 0); }
    if (args >= 4) { GetCmdArg(4, buf, sizeof(buf)); g_fAutoTimescale = StringToFloat(buf); }
    if (g_fAutoTimescale <= 0.0) g_fAutoTimescale = 1.0;

    if (g_iAutoTicks <= 0) g_iAutoTicks = 3000;
    if (delay < 1.0)       delay        = 1.0;

    PrintToServer("[CsAI] auto: %d ticks in %.1fs (quit after: %s)",
                  g_iAutoTicks, delay, g_bQuitAfter ? "yes" : "no");

    CreateTimer(delay, Timer_AutoSpawn);
    return Plugin_Handled;
}

void SetupRound()
{
    ServerCommand("sv_cheats 1");

    ServerCommand("sm plugins unload shavit-mapchooser");
    ServerCommand("sm plugins unload shavit-timelimit");
    ServerCommand("mp_freezetime 0");
    ServerCommand("mp_ignore_round_win_conditions 1");
    ServerCommand("mp_roundtime 60");
    ServerCommand("mp_timelimit 0");
    ServerCommand("mp_autoteambalance 0");
    ServerCommand("mp_limitteams 0");
    ServerCommand("mp_autokick 0");
    ServerCommand("bot_quota 0");
    ServerCommand("sv_alltalk 1");
    ServerCommand("mp_restartgame 1");
    PrintToServer("[CsAI] round management disabled");
}

public Action Timer_AutoSpawn(Handle timer)
{
    ServerCommand("csai_spawn");
    CreateTimer(1.0, Timer_AutoPrep);
    return Plugin_Stop;
}

public Action Timer_AutoPrep(Handle timer)
{
    // sv_cheats must be 1 *before* host_timescale means anything, and both go
    // through the console queue, so they need a frame to land before the run.
    ApplyTimescale(g_fAutoTimescale);
    CreateTimer(1.0, Timer_AutoRun);
    return Plugin_Stop;
}

public Action Timer_AutoRun(Handle timer)
{
    if (g_iBot == -1 || !IsClientInGame(g_iBot))
    {
        PrintToServer("[CsAI] auto: bot never became available, aborting");
        if (g_bQuitAfter)
            ServerCommand("quit");
        return Plugin_Stop;
    }

    UnfreezeBot();

    if (g_iDemoCL > 0)
    {
        UnfreezeBot();
        Demo_Begin(g_iBot);
        return Plugin_Stop;
    }

    if (g_iEvalCL > 0)
    {
        g_bTrainQuitAfter = g_bQuitAfter;
        if (g_iObsDumpCL > 0)
            ServerCommand("csai_obsdump %d", g_iObsDumpCL);
        Eval_Begin(g_iEvalCL);
        return Plugin_Stop;
    }

    if (g_iTrainBatchesCL > 0)
    {
        if (g_iObsDumpCL > 0)
            ServerCommand("csai_obsdump %d", g_iObsDumpCL);
        Train_Begin(g_iTrainBatchesCL, g_bTrainSyncCL, g_bQuitAfter);
        return Plugin_Stop;
    }

    if (g_iStateCount > 0)
        ApplyState(g_iBot, g_iBenchState);
    else
        LiftBot(g_iBot, g_fBenchHeight);   // fallback: no replay for this map
    StartRun(g_iAutoTicks, "bench");
    return Plugin_Stop;
}

// ------------------------------------------------------------- training ----
public Action Timer_TrainPoll(Handle timer)
{
    Train_Poll();
    return Plugin_Continue;
}

public Action Cmd_Train(int args)
{
    char buf[32];
    int  batches = 1;
    bool sync    = true;
    bool quitAfter = false;

    if (args >= 1) { GetCmdArg(1, buf, sizeof(buf)); batches   = StringToInt(buf); }
    if (args >= 2) { GetCmdArg(2, buf, sizeof(buf)); sync      = (StringToInt(buf) != 0); }
    if (args >= 3) { GetCmdArg(3, buf, sizeof(buf)); quitAfter = (StringToInt(buf) != 0); }

    Train_Begin(batches, sync, quitAfter);
    return Plugin_Handled;
}

public Action Cmd_TrainStop(int args)
{
    if (g_bTraining)
        Train_Stop();
    else
        PrintToServer("[CsAI] not training");
    return Plugin_Handled;
}

public Action Cmd_Cfg(int args)
{
    char key[32], val[32];
    if (args < 2)
    {
        PrintToServer("[CsAI] cfg: batch=%d frameskip=%d deviation=%.0f states=%d budget=%d switchcost=%.2f seed=%d",
                      g_iBatchTarget, g_iFrameSkip, g_fMaxDeviation,
                      g_iTrainStates, g_iEpTickBudget, g_fSwitchCost, g_iRngState);
        return Plugin_Handled;
    }
    GetCmdArg(1, key, sizeof(key));
    GetCmdArg(2, val, sizeof(val));

    if      (StrEqual(key, "batch"))      g_iBatchTarget  = StringToInt(val);
    else if (StrEqual(key, "frameskip"))  g_iFrameSkip    = (StringToInt(val) < 1) ? 1 : StringToInt(val);
    else if (StrEqual(key, "deviation"))  g_fMaxDeviation = StringToFloat(val);
    else if (StrEqual(key, "devcost"))    g_fDeviationCost = StringToFloat(val);
    else if (StrEqual(key, "pitch"))      g_iPitchMode = StringToInt(val);
    else if (StrEqual(key, "pitchfixed")) g_fPitchFixed = StringToFloat(val);
    else if (StrEqual(key, "switchcost")) g_fSwitchCost = StringToFloat(val);
    else if (StrEqual(key, "timecost"))   g_fTimeCost   = StringToFloat(val);
    else if (StrEqual(key, "finishbonus")) g_fFinishBonus = StringToFloat(val);
    else if (StrEqual(key, "finishfloor")) g_fFinishFloor = StringToFloat(val);
    else if (StrEqual(key, "timepower"))  g_fTimePower   = StringToFloat(val);
    else if (StrEqual(key, "learnedmix")) g_fLearnedMix  = StringToFloat(val);
    else if (StrEqual(key, "windup"))     g_iWindupTicks = StringToInt(val);
    else if (StrEqual(key, "windupyaw"))  g_fWindupYawCap = StringToFloat(val);
    else if (StrEqual(key, "winduphold")) g_iWindupHold  = StringToInt(val);
    else if (StrEqual(key, "trimcost"))   g_fTrimCost   = StringToFloat(val);
    else if (StrEqual(key, "states"))     g_iTrainStates  = StringToInt(val);
    else if (StrEqual(key, "statemix"))   g_fStateMix     = StringToFloat(val);
    else if (StrEqual(key, "statelo"))    g_fStateLo      = StringToFloat(val);
    else if (StrEqual(key, "statehi"))    g_fStateHi      = StringToFloat(val);
    else if (StrEqual(key, "budget"))     g_iEpTickBudget = StringToInt(val);
    else if (StrEqual(key, "seed"))       Pol_Seed(StringToInt(val));
    else PrintToServer("[CsAI] unknown cfg key '%s'", key);

    return Plugin_Handled;
}

public Action Cmd_ObsDump(int args)
{
    char buf[32];
    int n = 200;
    if (args >= 1) { GetCmdArg(1, buf, sizeof(buf)); n = StringToInt(buf); }
    if (n <= 0) n = 200;

    char dir[PLATFORM_MAX_PATH];
    BuildPath(Path_SM, dir, sizeof(dir), "data/csai/out");
    if (!DirExists(dir))
        CreateDirectory(dir, 511);

    char path[PLATFORM_MAX_PATH];
    BuildPath(Path_SM, path, sizeof(path), "data/csai/out/obsdump.txt");

    delete g_hObsDumpFile;
    g_hObsDumpFile = OpenFile(path, "w");
    if (g_hObsDumpFile == null)
    {
        PrintToServer("[CsAI] could not open %s", path);
        return Plugin_Handled;
    }
    g_iObsDump = n;
    PrintToServer("[CsAI] dumping %d obs rows to %s", n, path);
    return Plugin_Handled;
}

public Action Cmd_Eval(int args)
{
    char buf[32];
    int runs = 5;
    if (args >= 1) { GetCmdArg(1, buf, sizeof(buf)); runs = StringToInt(buf); }
    if (runs <= 0) runs = 5;
    Eval_Begin(runs);
    return Plugin_Handled;
}

public Action Cmd_Scripted(int args)
{
    char buf[32];
    if (args >= 1) { GetCmdArg(1, buf, sizeof(buf)); g_bScripted = (StringToInt(buf) != 0); }
    PrintToServer("[CsAI] scripted controller = %s", g_bScripted ? "on" : "off");
    return Plugin_Handled;
}

public Action Cmd_Prestrafe(int args)
{
    char buf[32];
    if (args >= 1) { GetCmdArg(1, buf, sizeof(buf)); g_bPrestrafeEnabled = (StringToInt(buf) != 0); }
    PrintToServer("[CsAI] prestrafe replay = %s (%d recorded ticks)",
                  g_bPrestrafeEnabled ? "on" : "off", g_iPreCount);
    return Plugin_Handled;
}

public Action Cmd_DemoCapture(int args)
{
    UnfreezeBot();
    Demo_Begin(g_iBot);
    return Plugin_Handled;
}

public Action Shavit_OnStart(int client, int track)
{
    if (client != g_iBot && !IsFakeClient(client))
    {
        Rec_Human_Track(client);
        // Everything still buffered is the prestrafe, bounded so a failed
        // attempt or a spell of idling does not ride along in the file.
        Rec_Human_TrimPre();
        g_iRecHumanPre = g_iRecHumanCount;
    }
    return Plugin_Continue;
}

public void Shavit_OnFinish(int client, int style, float time, int jumps, int strafes,
                            float sync, int track, float oldtime, float perfs,
                            float avgvel, float maxvel, int timestamp)
{
    if (client != g_iBot && !IsFakeClient(client))
        Rec_Human_Save(client);
}

public Action Cmd_RecSave(int client, int args)
{
    if (client > 0)
        Rec_Human_Track(client);
    Rec_Human_Save(client);
    return Plugin_Handled;
}

public Action Cmd_RecDrop(int client, int args)
{
    Rec_Human_Reset();
    if (client > 0 && IsClientInGame(client))
        PrintToChat(client, "[CsAI] recording restarted");
    return Plugin_Handled;
}

public Action Cmd_RecRuns(int client, int args)
{
    int n = Demo_CountFiles();
    if (client > 0 && IsClientInGame(client))
    {
        PrintToChat(client, "[CsAI] %d recorded run(s) for this map", n);
        PrintToChat(client, "[CsAI] recording now: %d ticks%s", g_iRecHumanCount,
                    (g_iRecHumanPre >= 0) ? " (timer running)" : " (waiting for timer)");
    }
    PrintToServer("[CsAI] %d recorded run(s), buffer %d ticks", n, g_iRecHumanCount);
    return Plugin_Handled;
}
