#include "Plugin.h"

template<size_t i> struct _PADDING_HELPER : _PADDING_HELPER<i - 1>
{
	static_assert(i != 0, "0 size struct not supported in C++");
	std::byte : 8;
};
template<> struct _PADDING_HELPER<1>
{
	std::byte : 8;
};
template<> struct _PADDING_HELPER<size_t(-1)>
{
	// To catch infinite recursion
};

#define PADDING(size) _PADDING_HELPER<size> EXPAND_CONCAT(CE_PADDING, __COUNTER__)

#include <shared/baseentity_shared.h>
#include <server/baseentity.h>
#include <server/player.h>
#include <cdll_int.h>
#include <eiface.h>
#include <interface.h>
#include <server_class.h>
#include <toolframework/ienginetool.h>

#include <map>
#include <optional>

IVEngineServer* engine;
CSharedEdictChangeInfo *g_pSharedChangeInfo;

IChangeInfoAccessor *CBaseEdict::GetChangeAccessor()
{
	return engine->GetChangeAccessor((const edict_t *)this);
}

template<class T>
class VariablePusher final
{
public:
	VariablePusher() = delete;
	VariablePusher(const VariablePusher<T>& other) = delete;
	VariablePusher(VariablePusher<T>&& other)
	{
		m_Variable = std::move(other.m_Variable);
		other.m_Variable = nullptr;
		m_OldValue = std::move(other.m_OldValue);
	}
	VariablePusher(T& variable, const T& newValue) : m_Variable(&variable)
	{
		m_OldValue = std::move(*m_Variable);
		*m_Variable = std::move(newValue);
	}
	VariablePusher(T& variable, T&& newValue) : m_Variable(&variable)
	{
		m_OldValue = std::move(*m_Variable);
		*m_Variable = std::move(newValue);
	}
	~VariablePusher()
	{
		if (m_Variable)
			*m_Variable = std::move(m_OldValue);
	}

	T& GetOldValue() { return m_OldValue; }
	const T& GetOldValue() const { return m_OldValue; }

private:
	T * const m_Variable;
	T m_OldValue;
};

class TF32BitAnglesPlugin final : public Plugin
{
	static SendProp* FindSendProp(SendTable* table, const char* name)
	{
		for (int i = 0; i < table->m_nProps; i++)
		{
			auto prop = &table->m_pProps[i];
			if (!strcmp(prop->GetName(), name))
				return prop;

			if (prop->GetType() == DPT_DataTable)
			{
				if (auto found = FindSendProp(prop->GetDataTable(), name))
					return found;
			}
		}

		return nullptr;
	}

	SendProp* m_KartBoost;
	SendProp* m_KartHealth;
	SendProp* m_EyeAngles0;
	SendProp* m_EyeAngles1;

	std::optional<VariablePusher<SendVarProxyFn>> m_KartBoostProxy;
	std::optional<VariablePusher<SendVarProxyFn>> m_KartHealthProxy;
	std::optional<VariablePusher<SendVarProxyFn>> m_EyeAngles0Proxy;
	std::optional<VariablePusher<SendVarProxyFn>> m_EyeAngles1Proxy;

	static void SendProxy_KartBoost(const SendProp* prop, const void* pStruct, const void* pData, DVariant* out, int element, int objectID);
	static void SendProxy_KartHealth(const SendProp* prop, const void* pStruct, const void* pData, DVariant* out, int element, int objectID);
	static void SendProxy_EyeAngles0(const SendProp* prop, const void* pStruct, const void* pData, DVariant* out, int element, int objectID);
	static void SendProxy_EyeAngles1(const SendProp* prop, const void* pStruct, const void* pData, DVariant* out, int element, int objectID);

	std::map<const void*, CBaseEntity*> m_EyeAngle0ToPlayer;
	std::map<const void*, CBaseEntity*> m_EyeAngle1ToPlayer;

public:
	bool Load(CreateInterfaceFn interfaceFactory, CreateInterfaceFn gameServerFactory) override
	{
		auto pEngineTool = (IEngineTool*)interfaceFactory(VENGINETOOL_INTERFACE_VERSION, nullptr);
		if (!pEngineTool)
			return false;

		engine = (IVEngineServer*)interfaceFactory(INTERFACEVERSION_VENGINESERVER, nullptr);
		if (!engine)
			return false;

		g_pSharedChangeInfo = engine->GetSharedEdictChangeInfo();
		if (!g_pSharedChangeInfo)
			return false;

		auto pServerDLL = (IServerGameDLL*)gameServerFactory(INTERFACEVERSION_SERVERGAMEDLL, nullptr);
		if (!pServerDLL)
			return false;

		for (auto sc = pServerDLL->GetAllServerClasses(); sc; sc = sc->m_pNext)
		{
			const char* name = sc->GetName();
			if (strcmp(name, "CTFPlayer"))
				continue;

			m_EyeAngles0 = FindSendProp(sc->m_pTable, "m_angEyeAngles[0]");
			m_EyeAngles1 = FindSendProp(sc->m_pTable, "m_angEyeAngles[1]");
			m_KartBoost = FindSendProp(sc->m_pTable, "m_flKartNextAvailableBoost");
			m_KartHealth = FindSendProp(sc->m_pTable, "m_iKartHealth");

			if (!m_EyeAngles0 || !m_EyeAngles1 || !m_KartBoost || !m_KartHealth)
				return false;

			m_KartBoostProxy.emplace(m_KartBoost->m_ProxyFn, SendProxy_KartBoost);
			m_KartHealthProxy.emplace(m_KartHealth->m_ProxyFn, SendProxy_KartHealth);
			m_EyeAngles0Proxy.emplace(m_EyeAngles0->m_ProxyFn, SendProxy_EyeAngles0);
			m_EyeAngles1Proxy.emplace(m_EyeAngles1->m_ProxyFn, SendProxy_EyeAngles1);

			return true;
		}

		return false;
	}
	void ClientActive(edict_t* entity) override
	{
		auto baseEnt = entity->GetUnknown()->GetBaseEntity();
		auto prop0Offset = m_EyeAngles0->GetOffset();
		auto prop1Offset = m_EyeAngles1->GetOffset();
		m_EyeAngle0ToPlayer[(std::byte*)baseEnt + prop0Offset] = baseEnt;
		m_EyeAngle1ToPlayer[(std::byte*)baseEnt + prop1Offset] = baseEnt;
	}
	void ClientDisconnect(edict_t* entity) override
	{
		auto baseEnt = entity->GetUnknown()->GetBaseEntity();
		for (auto iter = m_EyeAngle0ToPlayer.begin(); iter != m_EyeAngle0ToPlayer.end(); ++iter)
		{
			if (iter->second == baseEnt)
			{
				m_EyeAngle0ToPlayer.erase(iter);
				break;
			}
		}
		for (auto iter = m_EyeAngle1ToPlayer.begin(); iter != m_EyeAngle1ToPlayer.end(); ++iter)
		{
			if (iter->second == baseEnt)
			{
				m_EyeAngle1ToPlayer.erase(iter);
				break;
			}
		}
	}

	const char* GetPluginDescription() override { return "TF32BitAngles 1.0"; }
};

static TF32BitAnglesPlugin s_AnglesPlugin;
EXPOSE_SINGLE_INTERFACE_GLOBALVAR(TF32BitAnglesPlugin, IServerPluginCallbacks, INTERFACEVERSION_ISERVERPLUGINCALLBACKS, s_AnglesPlugin);

void TF32BitAnglesPlugin::SendProxy_KartBoost(const SendProp* prop, const void* pStruct, const void* pData, DVariant* out, int element, int objectID)
{
	out->m_Float = *(float*)((std::byte*)pStruct + s_AnglesPlugin.m_EyeAngles0->GetOffset());
}
void TF32BitAnglesPlugin::SendProxy_KartHealth(const SendProp* prop, const void* pStruct, const void* pData, DVariant* out, int element, int objectID)
{
	out->m_Float = *(float*)((std::byte*)pStruct + s_AnglesPlugin.m_EyeAngles1->GetOffset());
}

void TF32BitAnglesPlugin::SendProxy_EyeAngles0(const SendProp* prop, const void* pStruct, const void* pData, DVariant* out, int element, int objectID)
{
	if (auto found = s_AnglesPlugin.m_EyeAngle0ToPlayer.find(pData); found != s_AnglesPlugin.m_EyeAngle0ToPlayer.end())
		found->second->NetworkStateChanged(((std::byte*)found->second) + s_AnglesPlugin.m_KartBoost->GetOffset());

	s_AnglesPlugin.m_EyeAngles0Proxy->GetOldValue()(prop, pStruct, pData, out, element, objectID);
}
void TF32BitAnglesPlugin::SendProxy_EyeAngles1(const SendProp* prop, const void* pStruct, const void* pData, DVariant* out, int element, int objectID)
{
	if (auto found = s_AnglesPlugin.m_EyeAngle1ToPlayer.find(pData); found != s_AnglesPlugin.m_EyeAngle1ToPlayer.end())
		found->second->NetworkStateChanged(((std::byte*)found->second) + s_AnglesPlugin.m_KartHealth->GetOffset());

	s_AnglesPlugin.m_EyeAngles1Proxy->GetOldValue()(prop, pStruct, pData, out, element, objectID);
}