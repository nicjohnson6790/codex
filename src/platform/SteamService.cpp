#include "platform/SteamService.hpp"

#if defined(TERRAIN_SANDBOX_ENABLE_STEAM)
#include <steam/steam_api.h>
#endif

void SteamService::initialize(bool requested, const std::filesystem::path& inputManifestPath)
{
    m_requested = requested;
    if (!m_requested)
    {
        m_initialized = false;
        return;
    }

#if defined(TERRAIN_SANDBOX_ENABLE_STEAM)
    if (!SteamAPI_Init())
    {
        m_initialized = false;
        return;
    }

    m_initialized = true;

    if (ISteamUtils* steamUtils = SteamUtils())
    {
        m_appId = steamUtils->GetAppID();
    }

    if (ISteamUser* steamUser = SteamUser(); steamUser != nullptr && steamUser->BLoggedOn())
    {
        m_userId = steamUser->GetSteamID().ConvertToUint64();
    }

    if (ISteamFriends* steamFriends = SteamFriends())
    {
        const char* personaName = steamFriends->GetPersonaName();
        if (personaName != nullptr)
        {
            m_personaName = personaName;
        }
    }

    initializeSteamInput(inputManifestPath);
#else
    m_initialized = false;
#endif
}

void SteamService::pumpCallbacks()
{
#if defined(TERRAIN_SANDBOX_ENABLE_STEAM)
    if (m_initialized)
    {
        SteamAPI_RunCallbacks();
        if (m_steamInputInitialized)
        {
            SteamInput()->RunFrame();
            refreshSteamInputControllers();
        }
    }
#endif
}

void SteamService::shutdown()
{
#if defined(TERRAIN_SANDBOX_ENABLE_STEAM)
    if (m_initialized)
    {
        if (m_steamInputInitialized)
        {
            SteamInput()->Shutdown();
        }
        SteamAPI_Shutdown();
    }
#endif

    m_initialized = false;
    m_steamInputInitialized = false;
    m_steamInputManifestLoaded = false;
    m_appId = 0;
    m_userId = 0;
    m_steamInputControllerCount = 0;
    m_activeInputHandle = 0;
    m_terrainActionSet = 0;
    m_moveAction = 0;
    m_lookAction = 0;
    m_ascendAction = 0;
    m_descendAction = 0;
    m_rollLeftAction = 0;
    m_rollRightAction = 0;
    m_sprintAction = 0;
    m_alignUpAction = 0;
    m_lastSteamInputStateActive = false;
    m_lastMoveX = 0.0f;
    m_lastMoveY = 0.0f;
    m_lastLookX = 0.0f;
    m_lastLookY = 0.0f;
    m_personaName.clear();
}

bool SteamService::compiledIn() const
{
#if defined(TERRAIN_SANDBOX_ENABLE_STEAM)
    return true;
#else
    return false;
#endif
}

bool SteamService::steamInputActionsReady() const
{
    return m_terrainActionSet != 0 &&
        m_moveAction != 0 &&
        m_lookAction != 0 &&
        m_ascendAction != 0 &&
        m_descendAction != 0 &&
        m_rollLeftAction != 0 &&
        m_rollRightAction != 0 &&
        m_sprintAction != 0 &&
        m_alignUpAction != 0;
}

GamepadState SteamService::pollGamepadState()
{
#if defined(TERRAIN_SANDBOX_ENABLE_STEAM)
    if (!m_initialized || !m_steamInputInitialized || m_activeInputHandle == 0)
    {
        m_lastSteamInputStateActive = false;
        m_lastMoveX = 0.0f;
        m_lastMoveY = 0.0f;
        m_lastLookX = 0.0f;
        m_lastLookY = 0.0f;
        return {};
    }

    SteamInput()->ActivateActionSet(m_activeInputHandle, m_terrainActionSet);

    const InputAnalogActionData_t move = SteamInput()->GetAnalogActionData(
        m_activeInputHandle,
        m_moveAction);
    const InputAnalogActionData_t look = SteamInput()->GetAnalogActionData(
        m_activeInputHandle,
        m_lookAction);
    m_lastMoveX = move.bActive ? move.x : 0.0f;
    m_lastMoveY = move.bActive ? move.y : 0.0f;
    m_lastLookX = look.bActive ? look.x : 0.0f;
    m_lastLookY = look.bActive ? look.y : 0.0f;

    const bool ascend = digitalAction(m_ascendAction);
    const bool descend = digitalAction(m_descendAction);
    const bool rollLeft = digitalAction(m_rollLeftAction);
    const bool rollRight = digitalAction(m_rollRightAction);
    const bool sprint = digitalAction(m_sprintAction);
    const bool alignUp = digitalAction(m_alignUpAction);
    m_lastSteamInputStateActive = move.bActive ||
        look.bActive ||
        ascend ||
        descend ||
        rollLeft ||
        rollRight ||
        sprint ||
        alignUp;

    return {
        .leftX = m_lastMoveX,
        .leftY = -m_lastMoveY,
        .rightX = m_lastLookX,
        .rightY = -m_lastLookY,
        .leftTrigger = descend ? 1.0f : 0.0f,
        .rightTrigger = ascend ? 1.0f : 0.0f,
        .leftShoulder = rollLeft,
        .rightShoulder = rollRight || sprint,
        .rightStickPressed = alignUp,
        .hasGamepad = true,
    };
#else
    return {};
#endif
}

void SteamService::initializeSteamInput(const std::filesystem::path& inputManifestPath)
{
#if defined(TERRAIN_SANDBOX_ENABLE_STEAM)
    if (!m_initialized || SteamInput() == nullptr)
    {
        return;
    }

    m_steamInputManifestLoaded = SteamInput()->SetInputActionManifestFilePath(
        inputManifestPath.string().c_str());

    m_steamInputInitialized = SteamInput()->Init(true);
    if (!m_steamInputInitialized)
    {
        return;
    }

    m_terrainActionSet = SteamInput()->GetActionSetHandle("terrain_controls");
    m_moveAction = SteamInput()->GetAnalogActionHandle("move");
    m_lookAction = SteamInput()->GetAnalogActionHandle("look");
    m_ascendAction = SteamInput()->GetDigitalActionHandle("ascend");
    m_descendAction = SteamInput()->GetDigitalActionHandle("descend");
    m_rollLeftAction = SteamInput()->GetDigitalActionHandle("roll_left");
    m_rollRightAction = SteamInput()->GetDigitalActionHandle("roll_right");
    m_sprintAction = SteamInput()->GetDigitalActionHandle("sprint");
    m_alignUpAction = SteamInput()->GetDigitalActionHandle("align_up");

    SteamInput()->RunFrame();
    refreshSteamInputControllers();
#endif
}

void SteamService::refreshSteamInputControllers()
{
#if defined(TERRAIN_SANDBOX_ENABLE_STEAM)
    if (!m_steamInputInitialized)
    {
        return;
    }

    InputHandle_t inputHandles[STEAM_INPUT_MAX_COUNT]{};
    m_steamInputControllerCount = SteamInput()->GetConnectedControllers(inputHandles);
    m_activeInputHandle = m_steamInputControllerCount > 0 ? inputHandles[0] : 0;
    if (m_activeInputHandle != 0 && m_terrainActionSet != 0)
    {
        SteamInput()->ActivateActionSet(m_activeInputHandle, m_terrainActionSet);
    }
#endif
}

bool SteamService::digitalAction(std::uint64_t actionHandle) const
{
#if defined(TERRAIN_SANDBOX_ENABLE_STEAM)
    if (!m_steamInputInitialized || m_activeInputHandle == 0 || actionHandle == 0)
    {
        return false;
    }

    const InputDigitalActionData_t data = SteamInput()->GetDigitalActionData(
        m_activeInputHandle,
        actionHandle);
    return data.bActive && data.bState;
#else
    return false;
#endif
}
