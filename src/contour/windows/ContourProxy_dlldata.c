
#define PROXY_DELEGATION
#include <rpcproxy.h>

#ifdef __cplusplus
extern "C"
{
#endif

    EXTERN_PROXY_FILE(ITerminalHandoff)

    PROXYFILE_LIST_START
    /* list of proxy files */
    REFERENCE_PROXY_FILE(ITerminalHandoff),
        /* End of list */
        PROXYFILE_LIST_END

        DLLDATA_ROUTINES(aProxyFileList, GET_DLL_CLSID)

#ifdef __cplusplus
} /*extern "C" */
#endif
