/**
 * =============================================================================
 * CS2Fixes
 * Copyright (C) 2023-2024 Source2ZE
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "cs2fixes.h"
#include "khook.hpp"
#include "iserver.h"

#include "appframework/IAppSystem.h"
#include "common.h"
#include "detours.h"
#include "entities.h"
#include "entitylistener.h"
#include "icvar.h"
#include "interface.h"
#include "tier0/dbg.h"
#include "schemasystem/schemasystem.h"
#include "patches.h"
#include "plat.h"
#include "entitysystem.h"
#include "playermanager.h"
#include "gameconfig.h"
#include "entity/cgamerules.h"
#include "entity/ccsplayercontroller.h"
#include "serversideclient.h"
#include "te.pb.h"

#define VPROF_ENABLED
#include "tier0/vprof.h"

#include "tier0/memdbgon.h"

double g_flUniversalTime;
float g_flLastTickedTime;
bool g_bHasTicked;

void Message(const char *msg, ...)
{
	va_list args;
	va_start(args, msg);

	char buf[1024] = {};
	V_vsnprintf(buf, sizeof(buf) - 1, msg, args);

	ConColorMsg(Color(255, 0, 255, 255), "[CS2Fixes] %s", buf);

	va_end(args);
}

void Panic(const char *msg, ...)
{
	va_list args;
	va_start(args, msg);

	char buf[1024] = {};
	V_vsnprintf(buf, sizeof(buf) - 1, msg, args);

	Warning("[CS2Fixes] %s", buf);

	va_end(args);
}

class GameSessionConfiguration_t { };

CS2Fixes g_CS2Fixes;

CGameEntitySystem* g_pEntitySystem = nullptr;
CGlobalVars *gpGlobals = nullptr;
CPlayerManager *g_playerManager = nullptr;
IVEngineServer2 *g_pEngineServer2 = nullptr;
CGameConfig *g_GameConfig = nullptr;
CCSGameRules *g_pGameRules = nullptr;

// Metamod 2.0 build 1472 uses KHook (plugin API 18). SourceHook hooks from
// the original STFixes build cannot be loaded by this Metamod version.
static KHook::Return<void> OnClientActivePost(IServerGameClients*, CPlayerSlot slot,
	bool bLoadGame, const char* pszName, uint64 xuid)
{
	if (g_playerManager)
		g_CS2Fixes.Hook_ClientActive(slot, bLoadGame, pszName, xuid);
	return {KHook::Action::Ignore};
}

static KHook::Return<void> OnStartupServerPost(INetworkServerService*,
	const GameSessionConfiguration_t& config, ISource2WorldSession* pSession,
	const char* pszMapName)
{
	if (g_pEntityListener)
		g_CS2Fixes.Hook_StartupServer(config, pSession, pszMapName);
	return {KHook::Action::Ignore};
}

static KHook::Return<void> OnGravityPrecachePost(CBaseEntity* pThis,
	CEntityPrecacheContext* param)
{
	g_CS2Fixes.Hook_CTriggerGravityPrecache(pThis, param);
	return {KHook::Action::Ignore};
}

static KHook::Return<void> OnGravityEndTouchPost(CBaseEntity* pThis,
	CBaseEntity* pOther)
{
	g_CS2Fixes.Hook_CTriggerGravityEndTouch(pThis, pOther);
	return {KHook::Action::Ignore};
}

static KHook::Virtual<IServerGameClients, void, CPlayerSlot, bool, const char*, uint64>
	g_ClientActiveHook(&IServerGameClients::ClientActive, nullptr, OnClientActivePost);
static KHook::Virtual<INetworkServerService, void, const GameSessionConfiguration_t&,
	ISource2WorldSession*, const char*>
	g_StartupServerHook(&INetworkServerService::StartupServer, nullptr, OnStartupServerPost);
static KHook::Virtual<CBaseEntity, void, CEntityPrecacheContext*>
	g_GravityPrecacheHook(nullptr, OnGravityPrecachePost);
static KHook::Virtual<CBaseEntity, void, CBaseEntity*>
	g_GravityEndTouchHook(nullptr, OnGravityEndTouchPost);
// KHook::AddGlobal reads a vtable pointer from the first word of this carrier.
static void* g_TriggerGravityVTable = nullptr;

CGameEntitySystem* GameEntitySystem()
{
	static int offset = g_GameConfig->GetOffset("GameEntitySystem");
	return *reinterpret_cast<CGameEntitySystem**>((uintptr_t)(g_pGameResourceServiceServer) + offset);
}

// Will return null between map end & new map startup, null check if necessary!
INetworkGameServer* GetNetworkGameServer()
{
	return g_pNetworkServerService->GetIGameServer();
}

CGlobalVars* GetGlobals()
{
	return g_pEngineServer2->GetServerGlobals();
}

PLUGIN_EXPOSE(CS2Fixes, g_CS2Fixes);
bool CS2Fixes::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetEngineFactory, g_pEngineServer2, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pGameResourceServiceServer, IGameResourceService, GAMERESOURCESERVICESERVER_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetServerFactory, g_pSource2Server, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetServerFactory, g_pSource2ServerConfig, ISource2ServerConfig, SOURCE2SERVERCONFIG_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetServerFactory, g_pSource2GameEntities, ISource2GameEntities, SOURCE2GAMEENTITIES_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetServerFactory, g_pSource2GameClients, IServerGameClients, SOURCE2GAMECLIENTS_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pNetworkServerService, INetworkServerService, NETWORKSERVERSERVICE_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pNetworkMessages, INetworkMessages, NETWORKMESSAGES_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);

	// Required to get the IMetamodListener events
	g_SMAPI->AddListener(this, this);

	Message( "Starting plugin.\n" );

	CBufferStringGrowable<256> gamedirpath;
	g_pEngineServer2->GetGameDir(gamedirpath);

	std::string gamedirname = CGameConfig::GetDirectoryName(gamedirpath.Get());

	const char* gamedataPath = "addons/stfixes-metamod/gamedata/cs2fixes.games.txt";
	Message("Loading %s for game: %s\n", gamedataPath, gamedirname.c_str());

	g_GameConfig = new CGameConfig(gamedirname, gamedataPath);
	char conf_error[255] = "";
	if (!g_GameConfig->Init(g_pFullFileSystem, conf_error, sizeof(conf_error)))
	{
		snprintf(error, maxlen, "Could not read %s: %s", g_GameConfig->GetPath().c_str(), conf_error);
		Panic("%s\n", error);
		return false;
	}

	bool bRequiredInitLoaded = true;

	if (!addresses::Initialize(g_GameConfig))
		bRequiredInitLoaded = false;

	if (!InitPatches(g_GameConfig))
		bRequiredInitLoaded = false;

	if (!InitDetours(g_GameConfig))
		bRequiredInitLoaded = false;

	if (!bRequiredInitLoaded)
	{
		snprintf(error, maxlen, "One or more address lookups, patches or detours failed, please refer to startup logs for more information");
		return false;
	}

	if (g_GameConfig->GetOffset("CBaseEntity::Use") == -1)
	{
		snprintf(error, maxlen, "Failed to find CBaseEntity::Use\n");
		return false;
	}

	const auto pTriggerGravityVTable = modules::server->FindVirtualTable("CTriggerGravity");
	if (!pTriggerGravityVTable)
	{
		snprintf(error, maxlen, "Failed to find TriggerGravity vtable\n");
		return false;
	}

	int offset = g_GameConfig->GetOffset("CBaseEntity::Precache");
	if (offset == -1)
	{
		snprintf(error, maxlen, "Failed to find CBaseEntity::Precache\n");
		return false;
	}
	g_GravityPrecacheHook.Configure(offset);

	offset = g_GameConfig->GetOffset("CBaseEntity::EndTouch");
	if (offset == -1)
	{
		snprintf(error, maxlen, "Failed to find CBaseEntity::EndTouch\n");
		return false;
	}
	g_GravityEndTouchHook.Configure(offset);

	ConVar_Register();

	g_playerManager = new CPlayerManager();
	g_pEntityListener = new CEntityListener();
	g_TriggerGravityVTable = reinterpret_cast<void*>(pTriggerGravityVTable);
	g_ClientActiveHook.Add(g_pSource2GameClients);
	g_StartupServerHook.Add(g_pNetworkServerService);
	g_GravityPrecacheHook.AddGlobal(reinterpret_cast<CBaseEntity*>(&g_TriggerGravityVTable));
	g_GravityEndTouchHook.AddGlobal(reinterpret_cast<CBaseEntity*>(&g_TriggerGravityVTable));
	Message("All KHook hooks started!\n");

	// run our cfg
	g_pEngineServer2->ServerCommand("exec stfixes-metamod/cs2fixes");

	srand(time(0));

	if (late)
	{
		g_pEntitySystem = GameEntitySystem();
		g_pEntitySystem->AddListenerEntity(g_pEntityListener);
		gpGlobals = g_pEngineServer2->GetServerGlobals();
	}


	return true;
}

bool CS2Fixes::Unload(char *error, size_t maxlen)
{
	g_ClientActiveHook.Remove(g_pSource2GameClients);
	g_StartupServerHook.Remove(g_pNetworkServerService);
	g_GravityPrecacheHook.RemoveGlobal(reinterpret_cast<CBaseEntity*>(&g_TriggerGravityVTable));
	g_GravityEndTouchHook.RemoveGlobal(reinterpret_cast<CBaseEntity*>(&g_TriggerGravityVTable));

	ConVar_Unregister();

	FlushAllDetours();
	UndoPatches();

	if (g_playerManager)
		delete g_playerManager;

	if (g_GameConfig)
		delete g_GameConfig;

	if (g_pEntityListener)
	{
		g_pEntitySystem->RemoveListenerEntity(g_pEntityListener);
		delete g_pEntityListener;
	}

	return true;
}

void CS2Fixes::AllPluginsLoaded()
{
	/* This is where we'd do stuff that relies on the mod or other plugins 
	 * being initialized (for example, cvars added and events registered).
	 */

	Message( "AllPluginsLoaded\n" );
}

CUtlVector<CServerSideClient*>* GetClientList()
{
	if (!GetNetworkGameServer())
		return nullptr;

	static int offset = g_GameConfig->GetOffset("CNetworkGameServer_ClientList");
	return (CUtlVector<CServerSideClient*>*)(&GetNetworkGameServer()[offset]);
}

CServerSideClient *GetClientBySlot(CPlayerSlot slot)
{
	CUtlVector<CServerSideClient *> *pClients = GetClientList();

	if (!pClients)
		return nullptr;

	return pClients->Element(slot.Get());
}

void CS2Fixes::Hook_ClientActive( CPlayerSlot slot, bool bLoadGame, const char *pszName, uint64 xuid )
{
	Message( "Hook_ClientActive(%d, %d, \"%s\", %lli)\n", slot, bLoadGame, pszName, xuid );
	g_playerManager->OnClientConnected(slot, xuid);
}

void CS2Fixes::Hook_StartupServer(const GameSessionConfiguration_t& config, ISource2WorldSession *pSession, const char *pszMapName)
{
	g_pEntitySystem = GameEntitySystem();
	g_pEntitySystem->AddListenerEntity(g_pEntityListener);
	gpGlobals = g_pEngineServer2->GetServerGlobals();
}

void CS2Fixes::Hook_CTriggerGravityPrecache(CBaseEntity* pThis, CEntityPrecacheContext* param)
{
	const auto kv = param->m_pKeyValues;
	CTriggerGravityHandler::OnPrecache(pThis, kv);
}

void CS2Fixes::Hook_CTriggerGravityEndTouch(CBaseEntity* pThis, CBaseEntity* pOther)
{
	CTriggerGravityHandler::OnEndTouch(pThis, pOther);
}

void CS2Fixes::OnLevelInit(char const* pMapName,
						   char const* pMapEntities,
						   char const* pOldLevel,
						   char const* pLandmarkName,
						   bool loadGame,
						   bool background)
{
	Message("OnLevelInit(%s)\n", pMapName);

	// run our cfg
	g_pEngineServer2->ServerCommand("exec stfixes-metamod/cs2fixes");

	// Run map cfg (if present)
	char cmd[MAX_PATH];
	V_snprintf(cmd, sizeof(cmd), "exec stfixes-metamod/maps/%s", pMapName);
	g_pEngineServer2->ServerCommand(cmd);

	EntityHandler_OnLevelInit();
}

bool CS2Fixes::Pause(char *error, size_t maxlen)
{
	return true;
}

bool CS2Fixes::Unpause(char *error, size_t maxlen)
{
	return true;
}

const char *CS2Fixes::GetLicense()
{
	return "GPL v3 License";
}

const char *CS2Fixes::GetVersion()
{
#ifndef CS2FIXES_VERSION
#define CS2FIXES_VERSION "1.4-dev"
#endif

	return CS2FIXES_VERSION; // defined by the build script
}

const char *CS2Fixes::GetDate()
{
	return __DATE__;
}

const char *CS2Fixes::GetLogTag()
{
	return "STFixes-metamod";
}

const char *CS2Fixes::GetAuthor()
{
	return "xen, Poggu, and the Source2ZE community (reduced by interesting with misc fixes reimplemented by rcnoob)";
}

const char *CS2Fixes::GetDescription()
{
	return "CS2Fixes culled down with misc surf fixes remaining";
}

const char *CS2Fixes::GetName()
{
	return "STFixes-metamod";
}

const char *CS2Fixes::GetURL()
{
	return "https://github.com/SharpTimer/STFixes-metamod";
}
