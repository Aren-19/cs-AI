// Caps a player's combined (XYZ) speed as they leave the start zone, the way
// KSF surf servers do. Timer start itself is left to shavit: with the Normal
// style's "startinair" 1 and "nozaxisspeed" 0, the timer starts on leaving the
// zone rather than on the jump.

#include <sourcemod>
#include <sdktools>
#include <shavit/core>
#include <shavit/zones>

#pragma semicolon 1
#pragma newdecls required

public Plugin myinfo =
{
	name        = "Start zone speed cap",
	author      = "numan",
	description = "Caps combined speed on leaving the start zone",
	version     = "1.0.0",
	url         = ""
};

ConVar g_cvCap;

public void OnPluginStart()
{
	g_cvCap = CreateConVar("startzone_speedcap", "475.0",
		"Largest combined (XYZ) speed a player keeps when leaving the start zone. 0 = off.",
		0, true, 0.0);
	AutoExecConfig(true, "plugin.startzone-speedcap");
}

public void Shavit_OnLeaveZone(int client, int type, int track, int id, int entity, int data)
{
	if (type != Zone_Start || !IsClientInGame(client) || !IsPlayerAlive(client))
		return;

	float cap = g_cvCap.FloatValue;
	if (cap <= 0.0)
		return;

	float vel[3];
	GetEntPropVector(client, Prop_Data, "m_vecAbsVelocity", vel);
	float speed = GetVectorLength(vel);
	if (speed <= cap)
		return;

	ScaleVector(vel, cap / speed);
	TeleportEntity(client, NULL_VECTOR, NULL_VECTOR, vel);
}
