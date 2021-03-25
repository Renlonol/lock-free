#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <sched.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <fcntl.h>   // for O_* constants
#include "sharedRingBuffer.h"

#define AASYSCOM_RB_DBG(fmt,...) printf("%s(): "fmt, __FUNCTION__, ##__VA_ARGS__)
#define AASYSCOM_RB_INF(fmt,...) printf("%s(): "fmt, __FUNCTION__, ##__VA_ARGS__)
#define AASYSCOM_RB_WRN(fmt,...) printf("%s(): "fmt, __FUNCTION__, ##__VA_ARGS__)
#define AASYSCOM_RB_ERR(fmt,...) printf("%s(): "fmt, __FUNCTION__, ##__VA_ARGS__)

#define AASYSCOM_MAX_RB_CHECK_COUNT 1000
#define AASYSCOM_RB_CHECK_SLEEPGAP 500

#define SHM_NAME_LENGTH 64
#define PAGE_SIZE sysconf(_SC_PAGESIZE)

// cat /proc/sys/kernel/pid_max
#define RTOS_MAX_PROCS 32768


#define ALIGN(x, s) (size_t)(((x) + (s - 1U)) & ~(s - 1U))
#define RING_BUFFER_SIZE ALIGN((uint32_t)sizeof(TAaSysComRingBuffer), PAGE_SIZE)
#define RING_BUFFER_LIST_SIZE ALIGN((uint32_t)sizeof(TAaSysComRingBufferList)*RTOS_MAX_PROCS, PAGE_SIZE)

#define ATOMIC_CMPX(_ptr, _expected, _new) __atomic_compare_exchange_n(_ptr, _expected, _new, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)

static __thread TAaSysComRingBufferList *g_aaSysComRingBufferList = NULL;
static __thread int g_aaSysComRingBufferListSize = 0;
static __thread TAaSysComRingBuffer *g_aaSysComRingBuffer = NULL;

static int MakeShmObjName(const char* const name, char* buffer, size_t size)
{
    static char* ceNamePtr = NULL;
    int length;


    length = snprintf(buffer, size, "CCSRT-%d-%s-%s", getuid(),
             (ceNamePtr != NULL) ? ceNamePtr : "0",
             name);

    if (length >= (int)(size))
    {
        buffer[size-1] = 0;
        AASYSCOM_RB_ERR("shm name buffer is too short for '%s' -> '%s'",
            name, buffer);
        return -1;
    }

    return 0;
}

static Boolean UnmapShmObj(void* ptr, const size_t size)
{
    if (munmap(ptr, size) == -1)
    {
        AASYSCOM_RB_ERR("unmapping shared memory at %p failed", ptr);
        return FALSE;
    }

    return TRUE;
}


Boolean AaSysComIsLocalRingBufferInitialized(void)
{
    return g_aaSysComRingBuffer != NULL;
}

/**
 ********************************************************************************
 *
 *   @brief  Private helper function to allocate and initialize the process ring-buffer
 *
 *   @param   euId euId of the ringBuffer owner
 *   @param   leaveMapped defines if creator is owner of ringbuffer. If registration
 *            is done on behalf of another thread, ringbuffer is unmapped in the
 *            end.
 *
 *   @return  ring-buffer pointer in case of  success
 *            NULL error
 *
 *   Initialize a shared memory storage used as a ring-buffer.
 *
 *******************************************************************************/
TAaSysComRingBuffer *AaSysComCreateRingBuffer(uint32_t euId, Boolean leaveMapped)
{
    const size_t RB_SIZE = ALIGN((uint32_t)sizeof(TAaSysComRingBuffer),PAGE_SIZE);
    int ringBufferFd = -1;
    TAaSysComRingBuffer *ringBuffer = NULL;
    char queueName[AASYSCOM_MAX_RB_QUEUE_NAME_LENGTH] = {'\0'};
    char shmName[SHM_NAME_LENGTH];
    Boolean init = TRUE;
    int i;

    snprintf(queueName, sizeof(queueName), QUEUE_SHARED_MEM_NAME_FORMAT, (unsigned int)euId);
    AASYSCOM_RB_DBG("Creating new RB for euId 0x%x", euId);

    if (MakeShmObjName(queueName, shmName, sizeof(shmName)) != 0)
    {
        return NULL;
    }

    ringBufferFd = shm_open(shmName, O_CREAT|O_RDWR|O_EXCL, S_IRWXU|S_IRWXG|S_IRWXO);
    if (ringBufferFd < 0)
    {
        if (EEXIST == errno)
        {
            init = FALSE;
            ringBufferFd = shm_open(shmName, O_RDWR, S_IRWXU|S_IRWXG|S_IRWXO);
            AASYSCOM_RB_DBG("euId 0x%x - Already created", euId);
        }
    }
    if (-1 == ringBufferFd)
    {
        AASYSCOM_RB_ERR("Unable to create RB for %s -%s", shmName, strerror(errno));
        return NULL;
    }
    if ( TRUE == init)
    {
        if (-1 == ftruncate(ringBufferFd, (long int)RB_SIZE))
        {
            AASYSCOM_RB_ERR("Unable to size shared memory [%s] -%s %d.",
                    shmName, strerror(errno), (int)RB_SIZE);
            (void)shm_unlink(shmName);
            (void)close(ringBufferFd);
            return NULL;
        }
    }

    ringBuffer = (TAaSysComRingBuffer *) mmap(NULL, RB_SIZE,
            PROT_READ|PROT_WRITE, MAP_SHARED, ringBufferFd, 0);

    if (-1 == close(ringBufferFd))
    {
        AASYSCOM_RB_ERR("Error closing FD for RB [%s] %s.", shmName, strerror(errno));
    }
    if((void *)-1 == ringBuffer)
    {
        AASYSCOM_RB_ERR("Unable to mmap shared memory [%s] -%s.", shmName, strerror(errno));
        if (TRUE == init)
        {
            (void) shm_unlink(shmName);
        }
        return NULL;
    }

    if (TRUE == init)
    {
        ringBuffer->lock = SYSCOM_RB_STATUS_INUSE;
        ringBuffer->marker = 0;
        for( i = 0; i < (int)AASYSCOM_MAX_RB_EVENT+1; i++ ) ringBuffer->buffer[i] = EM_EVENT_UNDEF;
        ringBuffer->start = (AASYSCOM_RB_TYPE)AASYSCOM_MAX_RB_EVENT;
        ringBuffer->end   = (AASYSCOM_RB_TYPE)0;
    }
    else
    {
        TAaSysComRingBufferStatus lock = ringBuffer->lock;
        if (SYSCOM_RB_STATUS_INUSE == lock)
        {
            AASYSCOM_RB_DBG("Recreating RB [%s] previous lock value %d.", shmName, lock);
        }
        else if (SYSCOM_RB_STATUS_SHUTDOWN == lock)
        {
            // Lock set to SHUTDOWN implies that the previous owner is still cleaning up shutdown procedure.
            // Try until it is marked SYSCOM_RB_STATUS_LOCKED by shutdown procedure completion.
            i = 0;
            do
            {
                (void)sched_yield();
                lock = __sync_val_compare_and_swap(&ringBuffer->lock,SYSCOM_RB_STATUS_LOCKED,SYSCOM_RB_STATUS_SHUTDOWN);
            } while (( lock != SYSCOM_RB_STATUS_LOCKED) && ( i++ < 100));
            AASYSCOM_RB_DBG("RB [%p/%x %s] was locked %d.", ringBuffer, ringBuffer->marker, shmName, lock);
        }
        ringBuffer->marker++;
    }

    //No need to add the RB in the memory dump
    (void)madvise((void *)ringBuffer,RB_SIZE,MADV_DONTDUMP);

    AASYSCOM_RB_DBG("Locking RB %p/%x lock=0", ringBuffer, ringBuffer->marker);

    __sync_synchronize();
    ringBuffer->lock = SYSCOM_RB_STATUS_INUSE;
    (void)msync(ringBuffer, sizeof(TAaSysComRingBuffer), MS_INVALIDATE|MS_ASYNC);

    if(!leaveMapped)
    {
        munmap(ringBuffer, RB_SIZE);
        ringBuffer = NULL;
    }

    return ringBuffer;
}

void AaSysComCreateLocalRingBuffer(void)
{
    g_aaSysComRingBuffer = AaSysComCreateRingBuffer(pthread_self(), TRUE);
    assert(g_aaSysComRingBuffer != NULL);
}

/**
 ********************************************************************************
 *
 *   @brief  Private helper function to push an address to the process ring-buffer
 *
 *   @param  ringBuffer - target ring-buffert
 *           event  - data to be stored on the ring buffer (physical address)
 *
 *   @return  0 < success
 *            -1 error
 *
 *
 *******************************************************************************/
int AaSysComPushToRingBuffer(TAaSysComRingBuffer *const ringBuffer, em_event_t event)
{
    AASYSCOM_RB_TYPE end;
    AASYSCOM_RB_TYPE start;

    for(;;)
    {
        start = ringBuffer->start;
        end = ringBuffer->end;
        __sync_synchronize();

        if ((AASYSCOM_RB_TYPE)(end + 1) == start) // NOTE! Type cast is essential, otherwise there are miscalculations
        {
            AASYSCOM_RB_ERR("%p/%x Unable to push event %p %u/%u. RingBuffer full",
                    ringBuffer, ringBuffer->marker, (void*)event, ringBuffer->start, ringBuffer->end);
            #if defined(PLATFORM_DEVICE_LIONFISH)
                em_own_event(event);
            #endif
            return -1;
        }

        // Try to push event to next free ring buffer slot
        em_event_t expected = EM_EVENT_UNDEF;
        bool success = ATOMIC_CMPX(&ringBuffer->buffer[end], &expected, event);

        if ( ! success ) // failure
        {
            //If we are here there's a contention with another process trying to write to the
            //same ringBuffer, increment ringbuffer end and try again
            (void)ATOMIC_CMPX(&ringBuffer->end, &end, (AASYSCOM_RB_TYPE)(end + 1));
        }
        else // success
        {
            //If we are here we were able to replace a free slot with our event
            //Check if start had moved since we last read it
            if (start == ringBuffer->start) // start did not move, write done; exit cleanly
            {
                break;
            }
            else // start moved, check if we need to do rewrite
            {
                //If start moved it is possible that this process was swapped out and by the time
                //we were able to add the value, start had already passed it
                if (end <= ringBuffer->end) // our write landed and no roll over of end
                {
                    if (end <= ringBuffer->start) // our write landed after current read
                    {
                        // try to release our entry; if already freed we can leave, otherwise retry writing
                        success = ATOMIC_CMPX(&ringBuffer->buffer[end], &event, EM_EVENT_UNDEF);

                        if ( ! success ) // our write got read so we can exit cleanly
                        {
                            break;
                        }
                    }
                    else // our write is still before current read; exit cleanly
                        break;
                } // if
                else // our write landed and end roll over
                {
                    // cannot be sure if also read is rolled over or still before our write
                    // so to be safe side, we try again so we try to release our
                    // entry; if already freed we can leave, otherwise retry writing
                    success = ATOMIC_CMPX(&ringBuffer->buffer[end], &event, EM_EVENT_UNDEF);

                    if ( ! success ) // our write got read so we can exit cleanly
                    {
                        break;
                    }
                } // else
            } // else
        } // else
    } // for

    // End incremented for our successful write, we use CAS in order to
    // prevent unneeded increment in case other write has already done
    // increment. The CAS prevent bubbles from forming into the ring buffer.
    (void)ATOMIC_CMPX(&ringBuffer->end, &end, (AASYSCOM_RB_TYPE)(end + 1));
    return 0;
}

Boolean AaSysComPushToLocalRingBuffer(em_event_t event)
{
    return AaSysComPushToRingBuffer(g_aaSysComRingBuffer,event) == 0;
}

/**
 ********************************************************************************
 *
 *   @brief  Private helper function to pop an address from the process ring-buffer
 *
 *   There is only one reader per ring-buffer.
 *
 *   @param  ringBuffer - source ring-buffer
 *
 *   @return  event
 *            EM_EVENT_UNDEF error
 *
 *
 *******************************************************************************/
static em_event_t popFromRingBuffer(TAaSysComRingBuffer *const ringBuffer) {
    AASYSCOM_RB_TYPE start = (AASYSCOM_RB_TYPE)(ringBuffer->start + 1); // NOTE! Type cast is essential, otherwise there are miscalculations

    while (start != ringBuffer->end)
    {
        em_event_t event = ringBuffer->buffer[start];
        __sync_synchronize();
        ringBuffer->start = start;

        bool success = ATOMIC_CMPX(&ringBuffer->buffer[start], &event, EM_EVENT_UNDEF); // NOTE! in case exchange fails, event will point to the current value

        if (success && event != EM_EVENT_UNDEF)
        {
            // Succeeded to pop the expected event from ring buffer
            return event;
        }
        else if (event == EM_EVENT_UNDEF)
        {
            // Empty slot in the ring buffer, advance to the next one
            start = (AASYSCOM_RB_TYPE)(ringBuffer->start + 1);
        }
        else
        {
            // Some other event replaced the one we extected, try pop it instead. Note that
            // if there are multiple senders pushing messages at the same time it is possible
            // that the event we initially got is not there anymore after we have updated start
            // index and got replaced by event from antoher sender. This was missing which caused
            // event to be left behind to the ring buffer event then they were processed.
        }
    }

    return EM_EVENT_UNDEF;
}

em_event_t AaSysComPopFromLocalRingBuffer(void)
{
    return popFromRingBuffer(g_aaSysComRingBuffer);
}

static TAaSysComRingBuffer *mapRingBufferOfEuId(uint32_t euId)
{
    int ringBufferFd = -1;
    TAaSysComRingBuffer *ringBuffer = NULL;
    char rbName[AASYSCOM_MAX_RB_QUEUE_NAME_LENGTH] = {'\0'};
    char shmName[SHM_NAME_LENGTH];

    snprintf(rbName, sizeof(rbName), QUEUE_SHARED_MEM_NAME_FORMAT, (unsigned int)euId);

    if (MakeShmObjName(rbName, shmName, sizeof(shmName)) != 0)
    {
        return NULL;
    }

    ringBufferFd = shm_open(shmName, O_RDWR, S_IRWXU|S_IRWXG|S_IRWXO);

    if (ringBufferFd < 0)
    {
        AASYSCOM_RB_ERR("Unable to open %s RB - %s", shmName, strerror(errno));
        return NULL;
    }
    else
    {
        AASYSCOM_RB_DBG("euId 0x%x - Already created which is OK", euId);
    }

    /// FIXME Need to get rid of this. Perhaps changing order calls would do it.
    //---PR147325 check the shared memory size before use it in syscom_RB_init
    struct stat fileStat;
    uint32_t i = AASYSCOM_MAX_RB_CHECK_COUNT;
    while(i--)
    {
        if (-1 == fstat(ringBufferFd, &fileStat))
        {
            AASYSCOM_RB_ERR("Unable to get the file information  for %s -%s",
                    shmName, strerror(errno));
            close(ringBufferFd);
            return NULL;
        }

        if (0 < fileStat.st_size)
        {
            break;
        }
        usleep(AASYSCOM_RB_CHECK_SLEEPGAP);
    }

    if (0 >= fileStat.st_size)
    {
        close(ringBufferFd);
        AASYSCOM_RB_ERR("%s RB still not ready after waited for %d us",
                shmName, AASYSCOM_MAX_RB_CHECK_COUNT*AASYSCOM_RB_CHECK_SLEEPGAP);
        return NULL;
    }
    //---PR147325 check the shared memory size before use it in syscom_RB_init

    ringBuffer = (TAaSysComRingBuffer *) mmap(NULL, RING_BUFFER_SIZE,
                                              PROT_READ|PROT_WRITE, MAP_SHARED, ringBufferFd, 0);
    if (-1 == close(ringBufferFd))
    {
        AASYSCOM_RB_ERR("Error closing FD for RB [%s] %s.", shmName, strerror(errno));
    }
    if((void *)-1 == ringBuffer)
    {
        AASYSCOM_RB_ERR("Unable to mmap shared memory [%s] -%s.", shmName, strerror(errno));
        return NULL;
    }

    //No need to add the RB in the memory dump
    (void)madvise((void *)ringBuffer, RING_BUFFER_SIZE, MADV_DONTDUMP);

    return ringBuffer;
}

static int initRingBufferList(void)
{
    g_aaSysComRingBufferList = (TAaSysComRingBufferList *)malloc(RING_BUFFER_LIST_SIZE);

    if (NULL == g_aaSysComRingBufferList)
    {
        AASYSCOM_RB_ERR("Unable to allocate RB-List of size %zd", RING_BUFFER_LIST_SIZE);
        g_aaSysComRingBufferList = NULL; //In case NULL is different from NULL
        return -1;
    }
    memset( g_aaSysComRingBufferList, 0, RING_BUFFER_LIST_SIZE);
    //No need to add the RB List in the memory dump
    (void)madvise((void *)g_aaSysComRingBufferList,RING_BUFFER_LIST_SIZE,MADV_DONTDUMP);

    return 0;
}

/**
 ********************************************************************************
 *
 *   @brief  Private helper function to get the process ring-buffer
 *
 *   @param  euId - euId associated with the ringBuffer
 *
 *   @return  ring-buffer pointer in case of  success
 *            NULL error
 *
 *   Search in the local aaSysComRingBufferList for the ring-buffer associated
 *   with the euId that registered for the input CPID.
 *   If there is no mapping, it tries to map the shared memory for the ring buffer.
 *
 *******************************************************************************/
TAaSysComRingBuffer *AaSysComGetRingBufferForEuid(uint32_t euId)
{
    int end   = g_aaSysComRingBufferListSize;
    int start = 0;
    int mid   = 0;

    if (euId == pthread_self())
        return g_aaSysComRingBuffer;

    if (0 < end){
        while (start <= end) {
            mid = (int) ((start+end) >>1);
            if (g_aaSysComRingBufferList[mid].euId == euId) {
                AASYSCOM_RB_DBG("Found RB for euId:0x%x %p",
                                euId, g_aaSysComRingBufferList[mid].ringBuffer);
                //Ensure that this is not an obsolete entry
                if ((g_aaSysComRingBufferList[mid].marker != g_aaSysComRingBufferList[mid].ringBuffer->marker) ||
                        (SYSCOM_RB_STATUS_INUSE != g_aaSysComRingBufferList[mid].ringBuffer->lock)) {
                    AASYSCOM_RB_DBG("Replacing obsoleted RB for euId:0x%x %p", euId, g_aaSysComRingBufferList[mid].ringBuffer);
                    for (start = mid; start <g_aaSysComRingBufferListSize-1; ++start) {
                        g_aaSysComRingBufferList[start] = g_aaSysComRingBufferList[start+1];
                    }
                    g_aaSysComRingBufferListSize -= 1;
                    break;
                } else {
                    return g_aaSysComRingBufferList[mid].ringBuffer;
                }
            } else if (euId > g_aaSysComRingBufferList[mid].euId) {
                start = mid + 1;
            } else {
                end = mid - 1;
            }
        }
    }

    // Unable to find a INUSE ringBuffer associated with the euId/CPID from thread local
    // mapping table. Try to find it from filesystem and map it.
    AASYSCOM_RB_DBG("Init a RB to connect to 0x%x ", euId);
    TAaSysComRingBuffer *ringBuffer = mapRingBufferOfEuId(euId);

    if (NULL != ringBuffer) {
        if (NULL == g_aaSysComRingBufferList) {
            if (0 == initRingBufferList()) {
                AASYSCOM_RB_DBG("Init RB-List for 0x%x %p", euId, g_aaSysComRingBufferList);
            } else {
                AASYSCOM_RB_ERR("Unable to initialize RB-List for euId:0x%x.", euId);
                UnmapShmObj(ringBuffer, RING_BUFFER_SIZE);
                return NULL;
            }
        }

        if (g_aaSysComRingBufferListSize < RTOS_MAX_PROCS) {
            for (start = g_aaSysComRingBufferListSize-1; start >= 0; --start) {
                if (g_aaSysComRingBufferList[start].euId < euId) break;
                g_aaSysComRingBufferList[start+1] = g_aaSysComRingBufferList[start];
            }
            AASYSCOM_RB_DBG("Adding RB %p for euId:0x%x at %d", ringBuffer, euId, start+1);
            g_aaSysComRingBufferList[start+1].euId       = euId;
            g_aaSysComRingBufferList[start+1].marker     = (short unsigned int)ringBuffer->marker;
            g_aaSysComRingBufferList[start+1].ringBuffer = ringBuffer;
            g_aaSysComRingBufferListSize += 1;
        }
        else
        {
            AASYSCOM_RB_ERR("Unable to cache RB %p for euId:0x%x into aaSysComRingBufferList (%d)",
                            ringBuffer, euId, g_aaSysComRingBufferListSize);
        }
    }

    return ringBuffer;
}

static Boolean emptyRingBuffer(TAaSysComRingBuffer *const ringBuffer)
{
    em_event_t event;

    do
    {
        event = popFromRingBuffer(ringBuffer);
        if (EM_EVENT_UNDEF != event) {
            free(event);
        }
    } while (EM_EVENT_UNDEF != event);

    return TRUE;
}

/**
 ********************************************************************************
 *
 *   @brief  Private helper function to shutdown ring buffer by emptying it and leave locked
 *
 *   @param  ringBuffer - ring buffer
 *
 *   @return  0 success
 *
 *******************************************************************************/
static void shutdownRingBuffer(TAaSysComRingBuffer *rb)
{
    if (NULL != rb)
    {
        AASYSCOM_RB_DBG("ringBuffer %p/%x lock=%d", rb, rb->marker, rb->lock);
        TAaSysComRingBufferStatus lock = __sync_val_compare_and_swap(&rb->lock,SYSCOM_RB_STATUS_INUSE,SYSCOM_RB_STATUS_SHUTDOWN);
        __sync_synchronize();

        if (SYSCOM_RB_STATUS_INUSE == lock)
        {
            AASYSCOM_RB_DBG("Emptying ringBuffer %p/%x lock=%d",
                            rb, rb->marker, rb->lock);
            (void) emptyRingBuffer(rb);
            (void) __sync_val_compare_and_swap(&rb->lock,SYSCOM_RB_STATUS_SHUTDOWN,SYSCOM_RB_STATUS_LOCKED);
        }
    }
}

static void unmapRingBufferMemory(TAaSysComRingBuffer* ringBufferPtr)
{
    if (munmap((void*)ringBufferPtr, RING_BUFFER_SIZE) == -1)
    {
        AASYSCOM_RB_ERR("munmap failed errno: %i", errno);
    }
}

static void releaseRingBufferMemory(uint32_t euId)
{
    char rbName[AASYSCOM_MAX_RB_QUEUE_NAME_LENGTH] = { '\0' };
    char shmName[SHM_NAME_LENGTH];

    snprintf(rbName, sizeof(rbName), QUEUE_SHARED_MEM_NAME_FORMAT, (unsigned int) euId);

    if (MakeShmObjName(rbName, shmName, sizeof(shmName)) != 0)
    {
        return;
    }

    AASYSCOM_RB_DBG("releasing ringbuffer shared memory %s of euId %#x", shmName, euId);

    if (shm_unlink(shmName) == -1)
    {
        AASYSCOM_RB_DBG("shm_unlink failed for '%s': %s", shmName, strerror(errno));
    }
}

static TAaSysComRingBuffer* removeRingBufferMappingOfEuId(uint32_t euId)
{
    TAaSysComRingBuffer* rb = NULL;
    int start;

    if (g_aaSysComRingBufferListSize < RTOS_MAX_PROCS && g_aaSysComRingBufferListSize != 0)
    {
        for (start = g_aaSysComRingBufferListSize - 1; start >= 0; --start)
        {
            if (g_aaSysComRingBufferList[start].euId == euId)
            {
                rb = g_aaSysComRingBufferList[start].ringBuffer;
                AASYSCOM_RB_DBG("Removing RB %p of EU %#x", rb, euId);

                for (; start < g_aaSysComRingBufferListSize - 1; ++start)
                {
                    g_aaSysComRingBufferList[start] = g_aaSysComRingBufferList[start + 1];
                }
                g_aaSysComRingBufferListSize -= 1;
                break;
            }
        }
    }
    return rb;
}

Boolean AaSysComReleaseRingBuffer(uint32_t euId)
{
    AASYSCOM_RB_DBG("Releasing RB for EU %#x", euId);
    const uint32_t currentEuId = pthread_self();
    TAaSysComRingBuffer *rb;

    if (currentEuId == euId)
    {
        // In case of own ringbuffer, it is stored to separate thread specific variable
        rb = g_aaSysComRingBuffer;
        g_aaSysComRingBuffer = NULL;
    }
    else
        rb = removeRingBufferMappingOfEuId(euId);

    if (rb != NULL)
    {
        shutdownRingBuffer(rb);
        unmapRingBufferMemory(rb);
    }

    if (currentEuId != euId)
        releaseRingBufferMemory(euId);

    return TRUE;
}

static void unInitRingBufferList(void)
{
    if (NULL != g_aaSysComRingBufferList)
    {
        int i;
        for (i = 0; i < g_aaSysComRingBufferListSize; i++)
        {
            (void)unmapRingBufferMemory(g_aaSysComRingBufferList[i].ringBuffer);
        }

        free(g_aaSysComRingBufferList);
    }

    g_aaSysComRingBufferList = NULL;
    g_aaSysComRingBufferListSize = 0;
}

void AaSysComRingBufferEuShutdown(void)
{
    unInitRingBufferList();
    AaSysComReleaseRingBuffer(pthread_self());
}

void AaSysComRingBufferEeShutdown(void)
{
    AaSysComRingBufferEuShutdown(); // Process is also kind of EU
}
