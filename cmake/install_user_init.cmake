# Post-install step at the install prefix (normally ~/.nuke).
#
# 1. Registers the plugin folder in the user's init.py (runs in ALL modes, so
#    the version-dispatching MeshSimplify/init.py also works headless and on
#    farms). Idempotent: guarded by a marker comment (NDK_NOTES 8.2 / 8.4).
# 2. Removes the pre-1.1 layout: a MeshSimplify.dll at the folder root would be
#    loaded by EVERY Nuke version ahead of the per-version binary, and the old
#    deploy.ps1 block in menu.py registered the folder a second time.

set(_marker_start "# --- MeshSimplify (auto-added by installer) ---")
set(_marker_end "# --- end MeshSimplify ---")
set(_block "\n${_marker_start}\nimport nuke\nnuke.pluginAddPath('./MeshSimplify')\n${_marker_end}\n")

set(_init "${CMAKE_INSTALL_PREFIX}/init.py")
if(EXISTS "${_init}")
    file(READ "${_init}" _content)
    string(FIND "${_content}" "${_marker_start}" _pos)
    if(_pos EQUAL -1)
        file(APPEND "${_init}" "${_block}")
        message(STATUS "MeshSimplify: appended registration to ${_init}")
    else()
        message(STATUS "MeshSimplify: already registered in ${_init}")
    endif()
else()
    file(WRITE "${_init}" "${_block}")
    message(STATUS "MeshSimplify: created ${_init} with registration")
endif()

# Old binaries outside the per-version folders: MeshSimplify/MeshSimplify.dll
# (deploy.ps1 -Install) and .nuke/MeshSimplify.dll (the v1.0 README's manual
# copy). .nuke itself is always on the plugin path, so either one is found
# before the per-version binary and every Nuke version tries to load it.
foreach(_old IN ITEMS MeshSimplify/MeshSimplify.dll MeshSimplify/MeshSimplify.so
                      MeshSimplify.dll MeshSimplify.so)
    set(_p "${CMAKE_INSTALL_PREFIX}/${_old}")
    if(EXISTS "${_p}")
        file(REMOVE "${_p}")
        message(STATUS "MeshSimplify: removed old binary ${_p}")
    endif()
endforeach()

# Old deploy.ps1 block in menu.py (exact text it wrote).
set(_menu "${CMAKE_INSTALL_PREFIX}/menu.py")
if(EXISTS "${_menu}")
    file(READ "${_menu}" _m)
    # Add-Content wrote it with CRLF on Windows; match either line ending.
    foreach(_nl IN ITEMS "\r\n" "\n")
        set(_old_block "# --- MeshSimplify plugin (auto-added by deploy.ps1) ---${_nl}import nuke${_nl}nuke.pluginAddPath('./MeshSimplify')${_nl}# --- end MeshSimplify plugin ---${_nl}")
        string(FIND "${_m}" "${_old_block}" _opos)
        if(NOT _opos EQUAL -1)
            string(REPLACE "${_old_block}" "" _m "${_m}")
            file(WRITE "${_menu}" "${_m}")
            message(STATUS "MeshSimplify: removed old deploy.ps1 registration from ${_menu}")
        endif()
    endforeach()
endif()
