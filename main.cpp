#include "src/debug/log/Logger.hpp"
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/SharedDefs.hpp>
#include <hyprutils/os/FileDescriptor.hpp>
#include <src/desktop/state/FocusState.hpp>
#define private public
#include <hyprland/src/xwayland/XWayland.hpp>
#undef private
#include "globals.hpp"
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/event/EventBus.hpp>

#include <cstring>
#include <unistd.h>
#include <xcb/randr.h>
#include <xcb/xcb.h>

// Methods
// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

namespace XwaylandPrimaryPlugin {
    CHyprSignalListener prerenderHook;

    void                setXWaylandPrimary() {
        if (!g_pXWayland || !g_pXWayland->m_wm || !g_pXWayland->m_wm->m_connection || !g_pXWayland->m_wm->m_screen) {
            Log::logger->log(Log::DEBUG, "XWaylandPrimary: No XWayland client");
            return;
        }
        static auto* const PRIMARYNAME = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:xwaylandprimary:display")->getDataStaticPtr();
        static auto* const FOLLOWFOCUS = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:xwaylandprimary:followfocused")->getDataStaticPtr();
        auto               dofollow    = **FOLLOWFOCUS;

        auto               PMONITOR = g_pCompositor->getMonitorFromName(std::string{*PRIMARYNAME});

        if (dofollow && Desktop::focusState()->monitor()) {
            PMONITOR = Desktop::focusState()->monitor();
        }

        if (!PMONITOR) {
            Log::logger->log(Log::DEBUG, "XWaylandPrimary: Could not find monitor {}", std::string{*PRIMARYNAME});
            return;
        }

        xcb_connection_t*                       XCBCONN = g_pXWayland->m_wm->getConnection();
        xcb_screen_t*                           screen  = g_pXWayland->m_wm->m_screen;

        xcb_randr_get_screen_resources_cookie_t res_cookie = xcb_randr_get_screen_resources(XCBCONN, screen->root);
        xcb_randr_get_screen_resources_reply_t* res_reply  = xcb_randr_get_screen_resources_reply(XCBCONN, res_cookie, 0);
        xcb_timestamp_t                         timestamp  = res_reply->config_timestamp;

        int                                     output_cnt = 0;
        xcb_randr_output_t*                     x_outputs;

        output_cnt = xcb_randr_get_screen_resources_outputs_length(res_reply);
        x_outputs  = xcb_randr_get_screen_resources_outputs(res_reply);

        for (int i = 0; i < output_cnt; i++) {
            xcb_randr_get_output_info_reply_t* output = xcb_randr_get_output_info_reply(XCBCONN, xcb_randr_get_output_info(XCBCONN, x_outputs[i], timestamp), NULL);
            if (output == NULL) {
                continue;
            }
            uint8_t* output_name = xcb_randr_get_output_info_name(output);
            int      len         = xcb_randr_get_output_info_name_length(output);
            Log::logger->log(Log::DEBUG, "XWaylandPrimary: RANDR OUTPUT {}", (char*)output_name);
            if (!strncmp((char*)output_name, PMONITOR->m_name.c_str(), len)) {
                Log::logger->log(Log::DEBUG, "XWaylandPrimary: setting primary monitor {}", (char*)output_name);
                xcb_void_cookie_t p_cookie = xcb_randr_set_output_primary_checked(XCBCONN, screen->root, x_outputs[i]);
                xcb_request_check(XCBCONN, p_cookie);
                if (prerenderHook) {
                    prerenderHook = nullptr;
                }
                free(output);
                break;
            }
            free(output);
        }
        if (res_reply)
            free(res_reply);
        return;
    }

    void XWaylandready(wl_listener* listener, void* data) {
        setXWaylandPrimary();
    }

    wl_listener           readyListener      = {.notify = XWaylandready};
    inline CFunctionHook* pXWaylandReadyHook = nullptr;

    typedef int (*origXWaylandReady)(void* thisptr, int fd, uint32_t mask);

    int hkXWaylandReady(void* thisptr, int fd, uint32_t mask) {
        int retval = (*(origXWaylandReady)pXWaylandReadyHook->m_original)(thisptr, fd, mask);
        setXWaylandPrimary();
        return retval;
    }

    void monitorEvent() {
        for (auto& m : g_pCompositor->m_monitors) {
            if (!m->m_output)
                continue;
            Log::logger->log(Log::DEBUG, "XWaylandPrimary: MONITOR {} X {} Y {} WIDTH {} HEIGHT {}", m->m_name, m->m_position.x, m->m_position.y, m->m_size.x, m->m_size.y);
        }
        if (g_pXWayland->m_wm && g_pXWayland->m_wm->m_connection) {
            // Xwayland may not have created the new output yet, so delay via a periodic
            // hook until it does.
            if (prerenderHook) {
                // If there's an existing prerender hook, cancel it.
                prerenderHook = nullptr;
            }
            prerenderHook = Event::bus()->m_events.render.pre.listen([] { XwaylandPrimaryPlugin::setXWaylandPrimary(); });
        }
    }
} // namespace XwaylandPrimaryPlugin

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    HyprlandAPI::addConfigValue(PHANDLE, "plugin:xwaylandprimary:display", Hyprlang::STRING{STRVAL_EMPTY});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:xwaylandprimary:followfocused", Hyprlang::INT{0});
    static auto CONFIGRELOAD = Event::bus()->m_events.config.reloaded.listen([] { XwaylandPrimaryPlugin::setXWaylandPrimary(); });

    HyprlandAPI::reloadConfig();

    static const auto XWAYLANDREADYMETHODS = HyprlandAPI::findFunctionsByName(PHANDLE, "ready");
    for (auto& match : XWAYLANDREADYMETHODS) {
        if (match.demangled.contains("CXWaylandServer::ready")) {
            XwaylandPrimaryPlugin::pXWaylandReadyHook = HyprlandAPI::createFunctionHook(PHANDLE, match.address, (void*)&XwaylandPrimaryPlugin::hkXWaylandReady);
            XwaylandPrimaryPlugin::pXWaylandReadyHook->hook();
            break;
        }
    }

    static auto MACB = Event::bus()->m_events.monitor.added.listen([] { XwaylandPrimaryPlugin::monitorEvent(); });
    static auto MRCB = Event::bus()->m_events.monitor.removed.listen([] { XwaylandPrimaryPlugin::monitorEvent(); });
    static auto FMCB = Event::bus()->m_events.monitor.focused.listen([] { XwaylandPrimaryPlugin::monitorEvent(); });

    XwaylandPrimaryPlugin::setXWaylandPrimary();

    return {"XWayland Primary Display", "Set a configurable XWayland primary display", "Zakk", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    HyprlandAPI::invokeHyprctlCommand("seterror", "disable");
}
