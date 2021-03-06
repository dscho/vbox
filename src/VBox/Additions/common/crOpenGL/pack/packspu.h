/* Copyright (c) 2001, Stanford University
 * All rights reserved.
 *
 * See the file LICENSE.txt for information on redistributing this software.
 */

#ifndef CR_PACKSPU_H
#define CR_PACKSPU_H

#ifdef WINDOWS
#define PACKSPU_APIENTRY __stdcall
#else
#define PACKSPU_APIENTRY
#endif

#include "cr_glstate.h"
#include "cr_netserver.h"
#include "cr_pack.h"
#include "cr_spu.h"
#include "cr_threads.h"
#include "state/cr_client.h"
#ifdef VBOX_WITH_CRPACKSPU_DUMPER
# include "cr_dump.h"
#endif

extern uint32_t g_u32VBoxHostCaps;

typedef struct thread_info_t ThreadInfo;
typedef struct context_info_t ContextInfo;

struct thread_info_t {
    unsigned long id;
    CRNetServer netServer;
    CRPackBuffer buffer;
    CRPackBuffer normBuffer;
    CRPackBuffer BeginEndBuffer;
    GLenum BeginEndMode;
    int BeginEndState;
    ContextInfo *currentContext;
    CRPackContext *packer;
    int writeback;
    GLboolean bInjectThread;
    GLboolean inUse;
};

struct context_info_t {
    CRContext *clientState;  /* used to store client-side GL state */
    GLint serverCtx;         /* context ID returned by server */
    GLboolean  fAutoFlush;
    ThreadInfo *currentThread;
    GLubyte glVersion[100];     /* GL_VERSION string */
    GLubyte pszRealVendor[100];
    GLubyte pszRealVersion[100];
    GLubyte pszRealRenderer[100];
};

typedef struct {
    int id;
    int swap;

    /* config options */
    int emit_GATHER_POST_SWAPBUFFERS;
    int swapbuffer_sync;

    int ReadPixels;

    char *name;
    int buffer_size;

    int numThreads; /*number of used threads in the next array, doesn't need to be cont*/
    ThreadInfo thread[MAX_THREADS];
    int idxThreadInUse; /*index of any used thread*/

#if defined(WINDOWS) && defined(VBOX_WITH_WDDM)
    bool bRunningUnderWDDM;
#endif

#ifdef VBOX_WITH_CRPACKSPU_DUMPER
    SPUDispatchTable self;

    CR_RECORDER Recorder;
    CR_DBGPRINT_DUMPER Dumper;
#endif

    int numContexts;
    ContextInfo context[CR_MAX_CONTEXTS];
} PackSPU;

extern PackSPU pack_spu;

#define THREAD_OFFSET_MAGIC 2000

#ifdef CHROMIUM_THREADSAFE
extern CRmutex _PackMutex;
extern CRtsd _PackTSD;
#define GET_THREAD_VAL()  (crGetTSD(&_PackTSD))
#define GET_THREAD_IDX(_id) ((_id) - THREAD_OFFSET_MAGIC)
#define GET_THREAD_VAL_ID(_id) (&(pack_spu.thread[GET_THREAD_IDX(_id)]))
#else
#define GET_THREAD_VAL()  (&(pack_spu.thread[0]))
#endif
#define GET_THREAD(T)  ThreadInfo *T = GET_THREAD_VAL()
#define GET_THREAD_ID(T, _id) ThreadInfo *T = GET_THREAD_VAL_ID(_id)



#define GET_CONTEXT(C)                      \
  GET_THREAD(thread);                       \
  ContextInfo *C = thread->currentContext

#define CRPACKSPU_WRITEBACK_WAIT(_thread, _writeback)  CR_WRITEBACK_WAIT((_thread)->netServer.conn, _writeback)
#if defined(WINDOWS) && defined(VBOX_WITH_WDDM) && defined(VBOX_WITH_CRHGSMI) && defined(IN_GUEST)
# define CRPACKSPU_IS_WDDM_CRHGSMI() (pack_spu.bRunningUnderWDDM)
#else
# define CRPACKSPU_IS_WDDM_CRHGSMI() (GL_FALSE)
#endif

extern void packspuCreateFunctions( void );
extern void packspuSetVBoxConfiguration( const SPU *child_spu );
extern void packspuConnectToServer( CRNetServer *server
#if defined(VBOX_WITH_CRHGSMI) && defined(IN_GUEST)
                , struct VBOXUHGSMI *pHgsmi
#endif
        );
extern void packspuFlush( void *arg );
extern void packspuHuge( CROpcode opcode, void *buf );

extern GLboolean packspuSyncOnFlushes();

extern ThreadInfo *packspuNewThread(
#if defined(VBOX_WITH_CRHGSMI) && defined(IN_GUEST)
                struct VBOXUHGSMI *pHgsmi
#endif
        );

extern ThreadInfo *packspuNewCtxThread( struct VBOXUHGSMI *pHgsmi );

#endif /* CR_PACKSPU_H */
