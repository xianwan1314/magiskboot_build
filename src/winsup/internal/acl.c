#include <assert.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <aclapi.h>
#include <Lmcons.h>

#ifndef NDEBUG
#include <stdio.h>

#include "assert.h"

#define LOG_TAG                     "fs_internal"
#define LOG_ERR(...)                log_err(LOG_TAG, __VA_ARGS__);
#endif

static char userBuff[UNLEN + 1];

static HANDLE hToken = INVALID_HANDLE_VALUE;

int __ensure_path_access(const char *path, DWORD access) {
    char *pathBuff = strdup(path);

    if (!pathBuff) {
#ifndef NDEBUG
        perror("strdup");
#endif
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);

        return -1;
    }

    SECURITY_DESCRIPTOR *sd = NULL;

    ACL *oldDacl = NULL;  // inside sd, don't free

    ACL *newDacl = NULL;

    DWORD winerr;

    int ret = -1;

    // owner and group info are required for the AccessCheck below
    if ((winerr = GetNamedSecurityInfo(path, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, NULL, NULL, &oldDacl, NULL, (void **)&sd))) {
        SetLastError(winerr);

#ifndef NDEBUG
        LOG_ERR("GetNamedSecurityInfo failed: %s", win_strerror(winerr))
#endif

        goto exit;
    }

    EXPLICIT_ACCESS ea = {};

    BuildExplicitAccessWithName(&ea, userBuff, access, GRANT_ACCESS, SUB_CONTAINERS_AND_OBJECTS_INHERIT);

    // next, check if our process already have the requested permission

    GENERIC_MAPPING _mapping = {};  // unused

    // unused
    PRIVILEGE_SET _privs = {};
    DWORD _priv_size = sizeof(_privs);

    DWORD _granted = 0;  // unused

    BOOL status = FALSE;

    if (!AccessCheck(sd, hToken, access, &_mapping, &_privs, &_priv_size, &_granted, &status)) {
#ifndef NDEBUG
        LOG_ERR("AccessCheck failed: %s", win_strerror(GetLastError()))
#endif
        goto exit;
    }

    if (status)
        goto success;  // already has the permission, skip

    // merge with the old DACL

    if ((winerr = SetEntriesInAcl(1, &ea, oldDacl, &newDacl))) {
        SetLastError(winerr);

#ifndef NDEBUG
        LOG_ERR("SetEntriedInAcl failed: %s", win_strerror(winerr))
#endif

        goto exit;
    }

    if ((winerr = SetNamedSecurityInfo(pathBuff, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, newDacl, NULL))) {
        SetLastError(winerr);

#ifndef NDEBUG
        LOG_ERR("SetNamedSecurityInfo failed: %s", win_strerror(winerr))
#endif

        goto exit;
    }

success:
    ret = 0;

exit:
    if (pathBuff)
        free(pathBuff);

    if (sd)
        LocalFree(sd);  // also frees the memory region contains oldDacl

    if (newDacl)
        LocalFree(newDacl);

    return ret;
}

__attribute__((constructor)) static void __init_creds(void) {
    DWORD dw = sizeof(userBuff);

    if (!GetUserName(userBuff, &dw)) {
        LOG_ERR("GetUserName failed: %s", win_strerror(GetLastError()))

        assert(0);
    }

    // Ref: https://blog.aaronballman.com/2011/08/how-to-check-access-rights/

    HANDLE hProcessToken = INVALID_HANDLE_VALUE;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_IMPERSONATE | STANDARD_RIGHTS_READ, &hProcessToken)) {
        LOG_ERR("OpenProcessToken failed: %s", win_strerror(GetLastError()))

        assert(0);
    }

    BOOL succeed = DuplicateToken(hProcessToken, SecurityImpersonation, &hToken);

    CloseHandle(hProcessToken);

    if (!succeed) {
        LOG_ERR("DuplicateToken failed: %s", win_strerror(GetLastError()))

        assert(0);
    }
}

__attribute__((destructor)) static void __destroy_creds(void) {
    if (hToken != INVALID_HANDLE_VALUE)
        CloseHandle(hToken);
}
