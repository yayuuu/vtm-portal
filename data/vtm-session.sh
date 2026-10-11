# Source this file when starting a VTM session; do not execute it as a service.
export XDG_CURRENT_DESKTOP=vtm
export BROWSER=vtm-open
if command -v dbus-update-activation-environment >/dev/null 2>&1; then
    vtm_portal_systemd_option=
    if command -v systemctl >/dev/null 2>&1 && systemctl --user show-environment >/dev/null 2>&1; then
        vtm_portal_systemd_option=--systemd
    fi
    if [ -n "${VTM_DESKTOP_PIN:-}" ]; then
        dbus-update-activation-environment $vtm_portal_systemd_option XDG_CURRENT_DESKTOP BROWSER PATH VTM_DESKTOP_PIN
    else
        dbus-update-activation-environment $vtm_portal_systemd_option XDG_CURRENT_DESKTOP BROWSER PATH
    fi
    unset vtm_portal_systemd_option
fi
