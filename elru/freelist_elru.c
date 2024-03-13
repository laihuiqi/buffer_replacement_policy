/*-------------------------------------------------------------------------
 *
 * freelist.c
 *	  routines for managing the buffer pool's replacement strategy.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/freelist.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "pgstat.h"
#include "port/atomics.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/proc.h"

#define INT_ACCESS_ONCE(var)	((int)(*((volatile int *)&(var))))
#define TIMESTAMP_NIL -1

long long getCurrentTimeNanoseconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000 + ts.tv_nsec;
}


/*
 * A single buffer node structure, unit that forms doubly-'linked' list of buffers.
 */
 
typedef struct BufferNode
{
	int node_id;
	struct BufferNode *prev;
	struct BufferNode *next;
    long long last_accessed;
	long long second_last_accessed;


} BufferNode;

// A stack of buffer nodes, traces the buffer node addresses.
static BufferNode *elruStack = NULL;

/*
 * The shared freelist control information.
 */
typedef struct
{
	/* Spinlock: protects the values below */
	slock_t		buffer_strategy_lock;

	/*
	 * Clock sweep hand: index of next buffer to consider grabbing. Note that
	 * this isn't a concrete buffer - we only ever increase the value. So, to
	 * get an actual buffer, it needs to be used modulo NBuffers.
	 */
	pg_atomic_uint32 nextVictimBuffer;

	int			firstFreeBuffer;	/* Head of list of unused buffers */
	int			lastFreeBuffer; /* Tail of list of unused buffers */
	
	
	slock_t		stack_lock;
	slock_t 	lru_lock;
	// Pointer to the top and bottom nodes of the BufferNode stack.
	BufferNode *stackTop;
	BufferNode *stackBottom;

	/*
	 * NOTE: lastFreeBuffer is undefined when firstFreeBuffer is -1 (that is,
	 * when the list is empty)
	 */

	/*
	 * Statistics.  These counters should be wide enough that they can't
	 * overflow during a single bgwriter cycle.
	 */
	uint32		completePasses; /* Complete cycles of the clock sweep */
	pg_atomic_uint32 numBufferAllocs;	/* Buffers allocated since last reset */

	/*
	 * Bgworker process to be notified upon activity or -1 if none. See
	 * StrategyNotifyBgWriter.
	 */
	int			bgwprocno;
} BufferStrategyControl;

/* Pointers to shared state */
static BufferStrategyControl *StrategyControl = NULL;

/*
 * Private (non-shared) state for managing a ring of shared buffers to re-use.
 * This is currently the only kind of BufferAccessStrategy object, but someday
 * we might have more kinds.
 */
typedef struct BufferAccessStrategyData
{
	/* Overall strategy type */
	BufferAccessStrategyType btype;
	/* Number of elements in buffers[] array */
	int			nbuffers;

	/*
	 * Index of the "current" slot in the ring, ie, the one most recently
	 * returned by GetBufferFromRing.
	 */
	int			current;

	/*
	 * Array of buffer numbers.  InvalidBuffer (that is, zero) indicates we
	 * have not yet selected a buffer for this ring slot.  For allocation
	 * simplicity this is palloc'd together with the fixed fields of the
	 * struct.
	 */
	Buffer		buffers[FLEXIBLE_ARRAY_MEMBER];
}			BufferAccessStrategyData;


void StrategyAccessBuffer(int buf_id, bool delete); /* cs3223 */

/* Prototypes for internal functions */
static BufferDesc *GetBufferFromRing(BufferAccessStrategy strategy,
									 uint32 *buf_state);
static void AddBufferToRing(BufferAccessStrategy strategy,
							BufferDesc *buf);

/*
 * ClockSweepTick - Helper routine for StrategyGetBuffer()
 *
 * Move the clock hand one buffer ahead of its current position and return the
 * id of the buffer now under the hand.
 */
static inline uint32
ClockSweepTick(void)
{
	uint32		victim;

	/*
	 * Atomically move hand ahead one buffer - if there's several processes
	 * doing this, this can lead to buffers being returned slightly out of
	 * apparent order.
	 */
	victim =
		pg_atomic_fetch_add_u32(&StrategyControl->nextVictimBuffer, 1);

	if (victim >= NBuffers)
	{
		uint32		originalVictim = victim;

		/* always wrap what we look up in BufferDescriptors */
		victim = victim % NBuffers;

		/*
		 * If we're the one that just caused a wraparound, force
		 * completePasses to be incremented while holding the spinlock. We
		 * need the spinlock so StrategySyncStart() can return a consistent
		 * value consisting of nextVictimBuffer and completePasses.
		 */
		if (victim == 0)
		{
			uint32		expected;
			uint32		wrapped;
			bool		success = false;

			expected = originalVictim + 1;

			while (!success)
			{
				/*
				 * Acquire the spinlock while increasing completePasses. That
				 * allows other readers to read nextVictimBuffer and
				 * completePasses in a consistent manner which is required for
				 * StrategySyncStart().  In theory delaying the increment
				 * could lead to an overflow of nextVictimBuffers, but that's
				 * highly unlikely and wouldn't be particularly harmful.
				 */
				SpinLockAcquire(&StrategyControl->buffer_strategy_lock);

				wrapped = expected % NBuffers;

				success = pg_atomic_compare_exchange_u32(&StrategyControl->nextVictimBuffer,
														 &expected, wrapped);
				if (success)
					StrategyControl->completePasses++;
				SpinLockRelease(&StrategyControl->buffer_strategy_lock);
			}
		}
	}
	return victim;
}

/*
 * have_free_buffer -- a lockless check to see if there is a free buffer in
 *					   buffer pool.
 *
 * If the result is true that will become stale once free buffers are moved out
 * by other operations, so the caller who strictly want to use a free buffer
 * should not call this.
 */
bool
have_free_buffer(void)
{
	if (StrategyControl->firstFreeBuffer >= 0)
		return true;
	else
		return false;
}

bool isRemovedNode(const BufferNode* node) {
	return node->last_accessed == TIMESTAMP_NIL && node->second_last_accessed == TIMESTAMP_NIL;
}

// cs3223
int compareELRU(const BufferNode* a, const BufferNode* b) {
    if (isRemovedNode(a)) {
        return 1;
    }
    if (isRemovedNode(b)) {
        return -1;
    }

    if (a->second_last_accessed == TIMESTAMP_NIL && b->second_last_accessed == TIMESTAMP_NIL) {
        return a->last_accessed - b->last_accessed;
    } 
    
    if (a->second_last_accessed == TIMESTAMP_NIL) {
        return -1;
    } 
    
    if (b->second_last_accessed == TIMESTAMP_NIL) {
        return 1;
    }

    return a->second_last_accessed - b->second_last_accessed;
}

// cs3223

void processStackBottomToTop(BufferNode* node) {
    BufferNode* current = StrategyControl->stackBottom;

    // Edge case: If the stack is empty
    if (current == NULL) {
        StrategyControl->stackBottom = node;
        StrategyControl->stackTop = node;
        node->next = NULL;
        node->prev = NULL;
        return;
    }

    while (current != NULL) {
        // Edge case: If reached the end of the stack
        if (current->node_id == StrategyControl->stackTop->node_id) {
			if(compareELRU(node, current) > 0) {
				current->prev = node;
				node->next = current;
				node->prev = NULL;
				StrategyControl->stackTop = node;

			} else {
				if(current->next == NULL) {
					node->prev = current;
					current->next = node;
					StrategyControl->stackBottom = node;
				} else {
					current->next->prev = node;
					node->next = current->next;
					current->next = node;
					node->prev = current;
				}
			}
			return;
        }

        // Compare current and next using the modified comparator
        if (compareELRU(node, current) <= 0) {

			if(current->next == NULL) {
				StrategyControl->stackBottom = node;
				node->prev = current;
				node->next = NULL;
				current->next = node;
				break;
			}
			current->next->prev = node;
			node->next = current->next;
            current->next = node;
			node->prev = current;

            return;
        }

        current = current->prev;
    }
}

			
			// // stack is empty, node is also bottom
			// if (StrategyControl->stackTop == NULL)
			// {
			// 	StrategyControl->stackBottom = curr;
				
			// } else  
			// {
			// 	StrategyControl->stackTop->prev = curr;
			// 	curr->next = StrategyControl->stackTop;
			// }
			// // set node as top
			// StrategyControl->stackTop = curr;



// cs3223
// StrategyAccessBuffer 
// Called by bufmgr when a buffer page is accessed.
// Adjusts the position of buffer (identified by buf_id) in the LRU stack if delete is false;
// otherwise, delete buffer buf_id from the LRU stack.
void
StrategyAccessBuffer(int buf_id, bool delete)
{

	SpinLockAcquire(&StrategyControl->stack_lock);
	
	if (buf_id < 0 || buf_id >= NBuffers) // check the range of buf_id
	{
		SpinLockRelease(&StrategyControl->stack_lock);
		elog(ERROR, "Invalid buffer index");
	}
	
	BufferNode *curr = &elruStack[buf_id]; // focus on the node with node_id = buf_id

	
	if (false) // case 4: remove buffer(returned to freelist) from stack
    	{

		// not in stack or the only node in stack
    		if (curr->prev == NULL && curr->next == NULL)
		{	
			// not in stack
			if (StrategyControl->stackTop == NULL
				|| StrategyControl->stackTop->node_id != buf_id)
			{
				SpinLockRelease(&StrategyControl->stack_lock);
				return;
			}
			// only node in stack, stack is empty after this
			else if (StrategyControl->stackTop->node_id == buf_id
				&& StrategyControl->stackBottom->node_id == buf_id)
			{
				StrategyControl->stackTop = NULL;
    				StrategyControl->stackBottom = NULL;
			}
			
		}
		// node at top    	
        	else if (StrategyControl->stackTop->node_id == buf_id) 
        	{
            		StrategyControl->stackTop = curr->next;
            		curr->next->prev = NULL;
            		
        	} 
            	// node at bottom
        	else if (StrategyControl->stackBottom->node_id == buf_id) 
        	{	
            		StrategyControl->stackBottom = curr->prev;
            		curr->prev->next = NULL;
        	}
        	// node in the middle
        	else
        	{
        		curr->next->prev = curr->prev;
        		curr->prev->next = curr->next;
        		
        	}
        	// remove links on node
		curr->prev = NULL;
		curr->next = NULL;
		curr->last_accessed = TIMESTAMP_NIL;
		curr->second_last_accessed = TIMESTAMP_NIL;
        	
        SpinLockRelease(&StrategyControl->stack_lock);
        return;
	}
			long long prev_last_accessed = curr->last_accessed;
			curr -> last_accessed = getCurrentTimeNanoseconds();
			curr -> second_last_accessed = prev_last_accessed;

		
		if (curr->prev == NULL && curr->next == NULL
			&& (StrategyControl->stackTop == NULL
				|| StrategyControl->stackTop->node_id != buf_id))
		{	
			processStackBottomToTop(curr);
			
		}
		// c1/c3: node in stack, but not at top, move node to top
		else
		{	

			// node at bottom, set prev_node as new bottom
			if (StrategyControl->stackBottom->node_id == buf_id)
			{
				StrategyControl->stackBottom = curr->prev;
				curr->prev->next = NULL;
				
			} 
			else if(StrategyControl->stackTop->node_id == buf_id) {
				StrategyControl->stackTop = curr->next;
				StrategyControl->stackTop->prev = NULL;
			}
			 else {
				BufferNode * temp = curr;
				while(temp != NULL) {
					temp = temp->prev;
				}
				if(curr->next) curr->next->prev = curr->prev;
				if(curr->prev) curr->prev->next = curr->next;
			}
			
			// // set node as top
			// curr->next = StrategyControl->stackTop;
			// curr->prev = NULL;
			// StrategyControl->stackTop->prev = curr;
			// StrategyControl->stackTop = curr;

			processStackBottomToTop(curr);
			
		}
	
	SpinLockRelease(&StrategyControl->stack_lock);

}

/*
 * StrategyGetBuffer
 *
 *	Called by the bufmgr to get the next candidate buffer to use in
 *	BufferAlloc(). The only hard requirement BufferAlloc() has is that
 *	the selected buffer must not currently be pinned by anyone.
 *
 *	strategy is a BufferAccessStrategy object, or NULL for default strategy.
 *
 *	To ensure that no one else can pin the buffer before we do, we must
 *	return the buffer with the buffer header spinlock still held.
 */
BufferDesc *
StrategyGetBuffer(BufferAccessStrategy strategy, uint32 *buf_state, bool *from_ring)
{
	BufferDesc *buf;
	int			bgwprocno;
	int			trycounter;
	uint32		local_buf_state;	/* to avoid repeated (de-)referencing */

	*from_ring = false;

	/*
	 * If given a strategy object, see whether it can select a buffer. We
	 * assume strategy objects don't need buffer_strategy_lock.
	 */
	if (strategy != NULL)
	{
		buf = GetBufferFromRing(strategy, buf_state);
		if (buf != NULL)
		{
			*from_ring = true;
			StrategyAccessBuffer(buf->buf_id, false);
			return buf;
		}
	}

	/*
	 * If asked, we need to waken the bgwriter. Since we don't want to rely on
	 * a spinlock for this we force a read from shared memory once, and then
	 * set the latch based on that value. We need to go through that length
	 * because otherwise bgwprocno might be reset while/after we check because
	 * the compiler might just reread from memory.
	 *
	 * This can possibly set the latch of the wrong process if the bgwriter
	 * dies in the wrong moment. But since PGPROC->procLatch is never
	 * deallocated the worst consequence of that is that we set the latch of
	 * some arbitrary process.
	 */
	bgwprocno = INT_ACCESS_ONCE(StrategyControl->bgwprocno);
	if (bgwprocno != -1)
	{
		/* reset bgwprocno first, before setting the latch */
		StrategyControl->bgwprocno = -1;

		/*
		 * Not acquiring ProcArrayLock here which is slightly icky. It's
		 * actually fine because procLatch isn't ever freed, so we just can
		 * potentially set the wrong process' (or no process') latch.
		 */
		SetLatch(&ProcGlobal->allProcs[bgwprocno].procLatch);
	}

	/*
	 * We count buffer allocation requests so that the bgwriter can estimate
	 * the rate of buffer consumption.  Note that buffers recycled by a
	 * strategy object are intentionally not counted here.
	 */
	pg_atomic_fetch_add_u32(&StrategyControl->numBufferAllocs, 1);

	/*
	 * First check, without acquiring the lock, whether there's buffers in the
	 * freelist. Since we otherwise don't require the spinlock in every
	 * StrategyGetBuffer() invocation, it'd be sad to acquire it here -
	 * uselessly in most cases. That obviously leaves a race where a buffer is
	 * put on the freelist but we don't see the store yet - but that's pretty
	 * harmless, it'll just get used during the next buffer acquisition.
	 *
	 * If there's buffers on the freelist, acquire the spinlock to pop one
	 * buffer of the freelist. Then check whether that buffer is usable and
	 * repeat if not.
	 *
	 * Note that the freeNext fields are considered to be protected by the
	 * buffer_strategy_lock not the individual buffer spinlocks, so it's OK to
	 * manipulate them without holding the spinlock.
	 */
	if (StrategyControl->firstFreeBuffer >= 0)
	{
		while (true)
		{
			/* Acquire the spinlock to remove element from the freelist */
			SpinLockAcquire(&StrategyControl->buffer_strategy_lock);

			if (StrategyControl->firstFreeBuffer < 0)
			{
				SpinLockRelease(&StrategyControl->buffer_strategy_lock);
				break;
			}

			buf = GetBufferDescriptor(StrategyControl->firstFreeBuffer);
			Assert(buf->freeNext != FREENEXT_NOT_IN_LIST);
			
			/* Unconditionally remove buffer from freelist */
			StrategyControl->firstFreeBuffer = buf->freeNext;
			buf->freeNext = FREENEXT_NOT_IN_LIST;

			/*
			 * Release the lock so someone else can access the freelist while
			 * we check out this buffer.
			 */
			SpinLockRelease(&StrategyControl->buffer_strategy_lock);

			/*
			 * If the buffer is pinned or has a nonzero usage_count, we cannot
			 * use it; discard it and retry.  (This can only happen if VACUUM
			 * put a valid buffer in the freelist and then someone else used
			 * it before we got to it.  It's probably impossible altogether as
			 * of 8.3, but we'd better check anyway.)
			 *
			 * For lru implementation, usage_count is not important or ignored.
			 */
			local_buf_state = LockBufHdr(buf);
			if (BUF_STATE_GET_REFCOUNT(local_buf_state) == 0 && BUF_STATE_GET_USAGECOUNT(local_buf_state) == 0)
			{
				// SpinLockAcquire(&StrategyControl->lru_lock);
				// BufferNode* curr = &elruStack[buf->buf_id];
				// curr->last_accessed = TIMESTAMP_NIL;
				// curr->second_last_accessed = TIMESTAMP_NIL;
				// SpinLockRelease(&StrategyControl->lru_lock);

				StrategyAccessBuffer(buf->buf_id, false);
				*buf_state = local_buf_state;
				return buf;

			} 
			UnlockBufHdr(buf, local_buf_state);
		}
	}

	/* Nothing on the freelist, so run LRU */
	SpinLockAcquire(&StrategyControl->lru_lock);
	// Get victim buffer from the tail of list, which means the bottom of the stack.
	BufferNode *victim = StrategyControl->stackBottom;
	
	while(victim != NULL) {
		buf = GetBufferDescriptor(victim->node_id);

		/*
		 * If the buffer is pinned or has a nonzero usage_count, we cannot
		 * use it; discard it and retry.  (This can only happen if VACUUM
		 * put a valid buffer in the freelist and then someone else used
		 * it before we got to it.  It's probably impossible altogether as
		 * of 8.3, but we'd better check anyway.)
		 *
		 * For lru implementation, usage_count is not important or ignored.
		 */
		local_buf_state = LockBufHdr(buf);
		if (BUF_STATE_GET_REFCOUNT(local_buf_state) == 0)
		{

			victim->last_accessed = TIMESTAMP_NIL;
			victim->second_last_accessed = TIMESTAMP_NIL;

			StrategyAccessBuffer(buf->buf_id, false);
			*buf_state = local_buf_state;


			SpinLockRelease(&StrategyControl->lru_lock);
			return buf;
			
		}
		else if (StrategyControl->stackTop->node_id == buf->buf_id) 
		{
			UnlockBufHdr(buf, local_buf_state);
			SpinLockRelease(&StrategyControl->lru_lock);
			elog(ERROR, "No unpinned buffer");
		}
		
		UnlockBufHdr(buf, local_buf_state);
		
		victim = victim->prev;
        }
        
        SpinLockRelease(&StrategyControl->lru_lock);
}

/*
 * StrategyFreeBuffer: put a buffer on the freelist
 */
void
StrategyFreeBuffer(BufferDesc *buf)
{
	SpinLockAcquire(&StrategyControl->buffer_strategy_lock);

	/*
	 * It is possible that we are told to put something in the freelist that
	 * is already in it; don't screw up the list if so.
	 */
	if (buf->freeNext == FREENEXT_NOT_IN_LIST)
	{
		buf->freeNext = StrategyControl->firstFreeBuffer;
		if (buf->freeNext < 0)
			StrategyControl->lastFreeBuffer = buf->buf_id;
		StrategyControl->firstFreeBuffer = buf->buf_id;
		// c4: Buffer is returned to freelist
		StrategyAccessBuffer(buf->buf_id, true); 
	}

	SpinLockRelease(&StrategyControl->buffer_strategy_lock);
}

/*
 * StrategySyncStart -- tell BufferSync where to start syncing
 *
 * The result is the buffer index of the best buffer to sync first.
 * BufferSync() will proceed circularly around the buffer array from there.
 *
 * In addition, we return the completed-pass count (which is effectively
 * the higher-order bits of nextVictimBuffer) and the count of recent buffer
 * allocs if non-NULL pointers are passed.  The alloc count is reset after
 * being read.
 */
int
StrategySyncStart(uint32 *complete_passes, uint32 *num_buf_alloc)
{
	uint32		nextVictimBuffer;
	int			result;

	SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
	nextVictimBuffer = pg_atomic_read_u32(&StrategyControl->nextVictimBuffer);
	result = nextVictimBuffer % NBuffers;

	if (complete_passes)
	{
		*complete_passes = StrategyControl->completePasses;

		/*
		 * Additionally add the number of wraparounds that happened before
		 * completePasses could be incremented. C.f. ClockSweepTick().
		 */
		*complete_passes += nextVictimBuffer / NBuffers;
	}

	if (num_buf_alloc)
	{
		*num_buf_alloc = pg_atomic_exchange_u32(&StrategyControl->numBufferAllocs, 0);
	}
	SpinLockRelease(&StrategyControl->buffer_strategy_lock);
	return result;
}

/*
 * StrategyNotifyBgWriter -- set or clear allocation notification latch
 *
 * If bgwprocno isn't -1, the next invocation of StrategyGetBuffer will
 * set that latch.  Pass -1 to clear the pending notification before it
 * happens.  This feature is used by the bgwriter process to wake itself up
 * from hibernation, and is not meant for anybody else to use.
 */
void
StrategyNotifyBgWriter(int bgwprocno)
{
	/*
	 * We acquire buffer_strategy_lock just to ensure that the store appears
	 * atomic to StrategyGetBuffer.  The bgwriter should call this rather
	 * infrequently, so there's no performance penalty from being safe.
	 */
	SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
	StrategyControl->bgwprocno = bgwprocno;
	SpinLockRelease(&StrategyControl->buffer_strategy_lock);
}


/*
 * StrategyShmemSize
 *
 * estimate the size of shared memory used by the freelist-related structures.
 *
 * Note: for somewhat historical reasons, the buffer lookup hashtable size
 * is also determined here.
 */
Size
StrategyShmemSize(void)
{
	Size		size = 0;

	/* size of lookup hash table ... see comment in StrategyInitialize */
	size = add_size(size, BufTableShmemSize(NBuffers + NUM_BUFFER_PARTITIONS));

	/* size of the shared replacement strategy control block */
	size = add_size(size, MAXALIGN(sizeof(BufferStrategyControl)));

	/* size of the elruStack */
	size = add_size(size, MAXALIGN(mul_size(sizeof(BufferNode), NBuffers)));

	return size;
}

/*
 * StrategyInitialize -- initialize the buffer cache replacement
 *		strategy.
 *
 * Assumes: All of the buffers are already built into a linked list.
 *		Only called by postmaster and only during initialization.
 */
void
StrategyInitialize(bool init)
{
	bool		found;
	bool            stack_found;

	/*
	 * Initialize the shared buffer lookup hashtable.
	 *
	 * Since we can't tolerate running out of lookup table entries, we must be
	 * sure to specify an adequate table size here.  The maximum steady-state
	 * usage is of course NBuffers entries, but BufferAlloc() tries to insert
	 * a new entry before deleting the old.  In principle this could be
	 * happening in each partition concurrently, so we could need as many as
	 * NBuffers + NUM_BUFFER_PARTITIONS entries.
	 */
	InitBufTable(NBuffers + NUM_BUFFER_PARTITIONS);

	/*
	 * Get or create the shared strategy control block
	 */
	StrategyControl = (BufferStrategyControl *)
		ShmemInitStruct("Buffer Strategy Status",
						sizeof(BufferStrategyControl),
						&found);
	
	if (!found)
	{
		/*
		 * Only done once, usually in postmaster
		 */
		Assert(init);

		SpinLockInit(&StrategyControl->buffer_strategy_lock);

		/*
		 * Grab the whole linked list of free buffers for our strategy. We
		 * assume it was previously set up by InitBufferPool().
		 */
		StrategyControl->firstFreeBuffer = 0;
		StrategyControl->lastFreeBuffer = NBuffers - 1;
		
		// Init lock
		SpinLockInit(&StrategyControl->stack_lock);
		SpinLockInit(&StrategyControl->lru_lock);
		
		// The top and bottom pointers of the stack is NULL during initialization.
		StrategyControl->stackTop = NULL;
                StrategyControl->stackBottom = NULL;

		/* Initialize the clock sweep pointer */
		pg_atomic_init_u32(&StrategyControl->nextVictimBuffer, 0);

		/* Clear statistics */
		StrategyControl->completePasses = 0;
		pg_atomic_init_u32(&StrategyControl->numBufferAllocs, 0);

		/* No pending notification */
		StrategyControl->bgwprocno = -1;
                
                
	}
	else
		Assert(!init);
		
	// Initialize LRU stack with NBuffers nodes.
	elruStack = (BufferNode*)ShmemInitStruct(
		    "LRU stack", MAXALIGN(mul_size(sizeof(BufferNode), NBuffers)), &stack_found);
		
	if (!stack_found) {
		
		for (int i = 0; i < NBuffers; i++) {
                    BufferNode *new_node = &elruStack[i];
                    new_node->node_id = i;
                    new_node->prev = NULL;
                    new_node->next = NULL;
                    new_node->last_accessed = TIMESTAMP_NIL;
					new_node->second_last_accessed = TIMESTAMP_NIL;

                }
             	
        }
        else 
        	Assert(!init);
	
}


/* ----------------------------------------------------------------
 *				Backend-private buffer ring management
 * ----------------------------------------------------------------
 */


/*
 * GetAccessStrategy -- create a BufferAccessStrategy object
 *
 * The object is allocated in the current memory context.
 */
BufferAccessStrategy
GetAccessStrategy(BufferAccessStrategyType btype)
{
	int			ring_size_kb;

	/*
	 * Select ring size to use.  See buffer/README for rationales.
	 *
	 * Note: if you change the ring size for BAS_BULKREAD, see also
	 * SYNC_SCAN_REPORT_INTERVAL in access/heap/syncscan.c.
	 */
	switch (btype)
	{
		case BAS_NORMAL:
			/* if someone asks for NORMAL, just give 'em a "default" object */
			return NULL;

		case BAS_BULKREAD:
			ring_size_kb = 256;
			break;
		case BAS_BULKWRITE:
			ring_size_kb = 16 * 1024;
			break;
		case BAS_VACUUM:
			ring_size_kb = 256;
			break;

		default:
			elog(ERROR, "unrecognized buffer access strategy: %d",
				 (int) btype);
			return NULL;		/* keep compiler quiet */
	}

	return GetAccessStrategyWithSize(btype, ring_size_kb);
}

/*
 * GetAccessStrategyWithSize -- create a BufferAccessStrategy object with a
 *		number of buffers equivalent to the passed in size.
 *
 * If the given ring size is 0, no BufferAccessStrategy will be created and
 * the function will return NULL.  ring_size_kb must not be negative.
 */
BufferAccessStrategy
GetAccessStrategyWithSize(BufferAccessStrategyType btype, int ring_size_kb)
{
	int			ring_buffers;
	BufferAccessStrategy strategy;

	Assert(ring_size_kb >= 0);

	/* Figure out how many buffers ring_size_kb is */
	ring_buffers = ring_size_kb / (BLCKSZ / 1024);

	/* 0 means unlimited, so no BufferAccessStrategy required */
	if (ring_buffers == 0)
		return NULL;

	/* Cap to 1/8th of shared_buffers */
	ring_buffers = Min(NBuffers / 8, ring_buffers);

	/* NBuffers should never be less than 16, so this shouldn't happen */
	Assert(ring_buffers > 0);

	/* Allocate the object and initialize all elements to zeroes */
	strategy = (BufferAccessStrategy)
		palloc0(offsetof(BufferAccessStrategyData, buffers) +
				ring_buffers * sizeof(Buffer));

	/* Set fields that don't start out zero */
	strategy->btype = btype;
	strategy->nbuffers = ring_buffers;

	return strategy;
}

/*
 * GetAccessStrategyBufferCount -- an accessor for the number of buffers in
 *		the ring
 *
 * Returns 0 on NULL input to match behavior of GetAccessStrategyWithSize()
 * returning NULL with 0 size.
 */
int
GetAccessStrategyBufferCount(BufferAccessStrategy strategy)
{
	if (strategy == NULL)
		return 0;

	return strategy->nbuffers;
}

/*
 * FreeAccessStrategy -- release a BufferAccessStrategy object
 *
 * A simple pfree would do at the moment, but we would prefer that callers
 * don't assume that much about the representation of BufferAccessStrategy.
 */
void
FreeAccessStrategy(BufferAccessStrategy strategy)
{
	/* don't crash if called on a "default" strategy */
	if (strategy != NULL)
		pfree(strategy);
}

/*
 * GetBufferFromRing -- returns a buffer from the ring, or NULL if the
 *		ring is empty / not usable.
 *
 * The bufhdr spin lock is held on the returned buffer.
 */
static BufferDesc *
GetBufferFromRing(BufferAccessStrategy strategy, uint32 *buf_state)
{
	BufferDesc *buf;
	Buffer		bufnum;
	uint32		local_buf_state;	/* to avoid repeated (de-)referencing */


	/* Advance to next ring slot */
	if (++strategy->current >= strategy->nbuffers)
		strategy->current = 0;

	/*
	 * If the slot hasn't been filled yet, tell the caller to allocate a new
	 * buffer with the normal allocation strategy.  He will then fill this
	 * slot by calling AddBufferToRing with the new buffer.
	 */
	bufnum = strategy->buffers[strategy->current];
	if (bufnum == InvalidBuffer)
		return NULL;

	/*
	 * If the buffer is pinned we cannot use it under any circumstances.
	 *
	 * If usage_count is 0 or 1 then the buffer is fair game (we expect 1,
	 * since our own previous usage of the ring element would have left it
	 * there, but it might've been decremented by clock sweep since then). A
	 * higher usage_count indicates someone else has touched the buffer, so we
	 * shouldn't re-use it.
	 */
	buf = GetBufferDescriptor(bufnum - 1);
	local_buf_state = LockBufHdr(buf);
	if (BUF_STATE_GET_REFCOUNT(local_buf_state) == 0
		&& BUF_STATE_GET_USAGECOUNT(local_buf_state) <= 1)
	{
		*buf_state = local_buf_state;
		return buf;
	}
	UnlockBufHdr(buf, local_buf_state);

	/*
	 * Tell caller to allocate a new buffer with the normal allocation
	 * strategy.  He'll then replace this ring element via AddBufferToRing.
	 */
	return NULL;
}

/*
 * AddBufferToRing -- add a buffer to the buffer ring
 *
 * Caller must hold the buffer header spinlock on the buffer.  Since this
 * is called with the spinlock held, it had better be quite cheap.
 */
static void
AddBufferToRing(BufferAccessStrategy strategy, BufferDesc *buf)
{
	strategy->buffers[strategy->current] = BufferDescriptorGetBuffer(buf);
}

/*
 * Utility function returning the IOContext of a given BufferAccessStrategy's
 * strategy ring.
 */
IOContext
IOContextForStrategy(BufferAccessStrategy strategy)
{
	if (!strategy)
		return IOCONTEXT_NORMAL;

	switch (strategy->btype)
	{
		case BAS_NORMAL:

			/*
			 * Currently, GetAccessStrategy() returns NULL for
			 * BufferAccessStrategyType BAS_NORMAL, so this case is
			 * unreachable.
			 */
			pg_unreachable();
			return IOCONTEXT_NORMAL;
		case BAS_BULKREAD:
			return IOCONTEXT_BULKREAD;
		case BAS_BULKWRITE:
			return IOCONTEXT_BULKWRITE;
		case BAS_VACUUM:
			return IOCONTEXT_VACUUM;
	}

	elog(ERROR, "unrecognized BufferAccessStrategyType: %d", strategy->btype);
	pg_unreachable();
}

/*
 * StrategyRejectBuffer -- consider rejecting a dirty buffer
 *
 * When a nondefault strategy is used, the buffer manager calls this function
 * when it turns out that the buffer selected by StrategyGetBuffer needs to
 * be written out and doing so would require flushing WAL too.  This gives us
 * a chance to choose a different victim.
 *
 * Returns true if buffer manager should ask for a new victim, and false
 * if this buffer should be written and re-used.
 */
bool
StrategyRejectBuffer(BufferAccessStrategy strategy, BufferDesc *buf, bool from_ring)
{
	/* We only do this in bulkread mode */
	if (strategy->btype != BAS_BULKREAD)
		return false;

	/* Don't muck with behavior of normal buffer-replacement strategy */
	if (!from_ring ||
		strategy->buffers[strategy->current] != BufferDescriptorGetBuffer(buf))
		return false;

	/*
	 * Remove the dirty buffer from the ring; necessary to prevent infinite
	 * loop if all ring members are dirty.
	 */
	strategy->buffers[strategy->current] = InvalidBuffer;

	return true;
}