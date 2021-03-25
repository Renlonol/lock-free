#ifndef _SHAREDRINGBUFFER_H_
#define _SHAREDRINGBUFFER_H_

#include <stdint.h>
#include <limits.h>



typedef void* em_event_t;
 #define EM_EVENT_UNDEF (NULL)

#define AASYSCOM_MAX_RB_QUEUE_NAME_LENGTH 20
#define AASYSCOM_RB_TYPE unsigned short
#define AASYSCOM_MAX_RB_EVENT USHRT_MAX /*USHRT_MAX can only be UCHAR or USHRT*/

typedef enum
{
    FALSE,
    TRUE
} Boolean;

typedef enum {
   SYSCOM_RB_STATUS_INUSE,    /** Ring buffer is created and can be used for messaging */
   SYSCOM_RB_STATUS_SHUTDOWN, /** Ring buffer is shutdown (i.e. owner is terminated, all mmaps to this ring buffer needs to be removed) */
   SYSCOM_RB_STATUS_LOCKED    /** Ring buffer is unused (no owner at the moment) */
} TAaSysComRingBufferStatus;

typedef struct TAaSysComRingBuffer {
  __attribute__((__aligned__(8))) volatile unsigned int marker;
  __attribute__((__aligned__(8))) volatile AASYSCOM_RB_TYPE start;
  __attribute__((__aligned__(8))) volatile AASYSCOM_RB_TYPE end;
  __attribute__((__aligned__(8))) volatile em_event_t buffer[(int)(AASYSCOM_MAX_RB_EVENT+1)];
  __attribute__((__aligned__(8))) volatile TAaSysComRingBufferStatus lock;
} TAaSysComRingBuffer;

/// Mapping info between euId and a ring-buffer used to send messages to
// a SMP process. EU IDs and CPIDs are in different mappings because one EU may
// have multiple CPIDs.
typedef struct TAaSysComRingBufferList {
  uint32_t                euId;  //processId
  unsigned short int      marker;
  TAaSysComRingBuffer    *ringBuffer;  //RingBuffer used to communicate with SMP processes
} TAaSysComRingBufferList;


#define QUEUE_SHARED_MEM_NAME_FORMAT "AaSysComQueue-%03x"

TAaSysComRingBuffer *AaSysComCreateRingBuffer(uint32_t euId, Boolean leaveMapped);
int AaSysComPushToRingBuffer(TAaSysComRingBuffer *const ringBuffer, em_event_t event);
Boolean AaSysComReleaseRingBuffer(uint32_t euId);
TAaSysComRingBuffer *AaSysComGetRingBufferForLutIndex(int lutIndex);
TAaSysComRingBuffer *AaSysComGetRingBufferForEuid(uint32_t euId);
Boolean AaSysComPushToLocalRingBuffer(em_event_t event);
Boolean AaSysComIsLocalRingBufferInitialized(void);
void AaSysComCreateLocalRingBuffer(void);
em_event_t AaSysComPopFromLocalRingBuffer(void);
void AaSysComRingBufferEuShutdown(void);
void AaSysComRingBufferEeShutdown(void);



#endif /* _SHAREDRINGBUFFER_H_ */
