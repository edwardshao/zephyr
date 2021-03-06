/*
 * Copyright (c) 1997-2010, 2013-2014 Wind River Systems, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <microkernel.h>
#include <micro_private.h>
#include <toolchain.h>
#include <sections.h>

/* Auto-Defrag settings */

#define AD_NONE 0
#define AD_BEFORE_SEARCH4BIGGERBLOCK 1
#define AD_AFTER_SEARCH4BIGGERBLOCK 2

#define AUTODEFRAG AD_AFTER_SEARCH4BIGGERBLOCK

/**
 *
 * @brief Initialize kernel memory pool subsystem
 *
 * Perform any initialization of memory pool that wasn't done at build time.
 *
 * @return N/A
 */
void _k_mem_pool_init(void)
{
	struct pool_struct *P;
	int i;

	/* perform initialization for each memory pool */

	for (i = 0, P = _k_mem_pool_list; i < _k_mem_pool_count; i++, P++) {

		/*
		 * mark block set for largest block size
		 * as owning all of the memory pool buffer space
		 */

		int remaining = P->nr_of_maxblocks;
		int t = 0;
		char *memptr = P->bufblock;

		while (remaining >= 4) {
			P->block_set[0].quad_block[t].mem_blocks = memptr;
			P->block_set[0].quad_block[t].mem_status = 0xF;
			t++;
			remaining = remaining - 4;
			memptr +=
				OCTET_TO_SIZEOFUNIT(P->block_set[0].block_size)
				* 4;
		}

		if (remaining != 0) {
			P->block_set[0].quad_block[t].mem_blocks = memptr;
			P->block_set[0].quad_block[t].mem_status =
				0xF >> (4 - remaining);
			/* non-existent blocks are marked as unavailable */
		}

		/*
		 * note: all other block sets own no blocks, since their
		 * first quad-block has a NULL memory pointer
		 */
	}
}

/**
 *
 * @brief Determines which block set corresponds to the specified data size
 *
 * Finds the block set with the smallest blocks that can hold the specified
 * amount of data.
 *
 * @return block set index
 */
static int compute_block_set_index(struct pool_struct *P, int data_size)
{
	int block_size = P->minblock_size;
	int offset = P->nr_of_block_sets - 1;

	while (data_size > block_size) {
		block_size = block_size << 2;
		offset--;
	}

	return offset;
}

/**
 *
 * @brief Return an allocated block to its block set
 *
 * @param ptr pointer to start of block
 * @param P memory pool descriptor
 * @param index block set identifier
 *
 * @return N/A
 */
static void free_existing_block(char *ptr, struct pool_struct *P, int index)
{
	struct pool_quad_block *quad_block = P->block_set[index].quad_block;
	char *block_ptr;
	int i, j;

	/*
	 * search block set's quad-blocks until the block is located,
	 * then mark it as unused
	 *
	 * note: block *must* exist, so no need to do array bounds checking
	 */

	for (i = 0; ; i++) {
		__ASSERT((i < P->block_set[index].nr_of_entries) &&
			 (quad_block[i].mem_blocks != NULL),
			 "Attempt to free unallocated memory pool block\n");

		block_ptr = quad_block[i].mem_blocks;
		for (j = 0; j < 4; j++) {
			if (ptr == block_ptr) {
				quad_block[i].mem_status |= (1 << j);
				return;
			}
			block_ptr += OCTET_TO_SIZEOFUNIT(
				P->block_set[index].block_size);
		}
	}
}


/**
 *
 * @brief Defragment the specified memory pool block sets
 *
 * Reassembles any quad-blocks that are entirely unused into larger blocks
 * (to the extent permitted).
 *
 * @param P memory pool descriptor
 * @param ifraglevel_start index of smallest block set to defragment
 * @param ifraglevel_stop index of largest block set to defragment
 *
 * @return N/A
 */
static void defrag(struct pool_struct *P,
		   int ifraglevel_start, int ifraglevel_stop)
{
	int i, j, k;
	struct pool_quad_block *quad_block;

	/* process block sets from smallest to largest permitted sizes */

	for (j = ifraglevel_start; j > ifraglevel_stop; j--) {

		quad_block = P->block_set[j].quad_block;
		i = 0;

		do {
			/* block set is done if no more quad-blocks exist */

			if (quad_block[i].mem_blocks == NULL) {
				break;
			}

			/* reassemble current quad-block, if possible */

			if (quad_block[i].mem_status == 0xF) {

				/*
				 * mark the corresponding block in next larger
				 * block set as free
				 */

				free_existing_block(
					quad_block[i].mem_blocks, P, j - 1);

				/*
				 * delete the quad-block from this block set
				 * by replacing it with the last quad-block
				 *
				 * (algorithm works even when the deleted
				 * quad-block is the last quad_block)
				 */

				k = i;
				while (((k+1) != P->block_set[j].nr_of_entries)
				       &&
				       (quad_block[k+1].mem_blocks != NULL)) {
					k++;
				}

				quad_block[i].mem_blocks =
					quad_block[k].mem_blocks;
				quad_block[i].mem_status =
					quad_block[k].mem_status;

				quad_block[k].mem_blocks = NULL;

				/* loop & process replacement quad_block[i] */
			} else {
				i++;
			}

			/* block set is done if at end of quad-block array */

		} while (i < P->block_set[j].nr_of_entries);
	}
}

/**
 *
 * @brief Perform defragment memory pool request
 *
 * @return N/A
 */
void _k_defrag(struct k_args *A)
{
	struct pool_struct *P = _k_mem_pool_list + OBJ_INDEX(A->args.p1.pool_id);

	/* do complete defragmentation of memory pool (i.e. all block sets) */

	defrag(P, P->nr_of_block_sets - 1, 0);

	/* reschedule anybody waiting for a block */

	if (P->waiters) {
		struct k_args *NewGet;

		/*
		 * create a command packet to re-try block allocation
		 * for the waiting tasks, and add it to the command stack
		 */

		GETARGS(NewGet);
		*NewGet = *A;
		NewGet->Comm = _K_SVC_BLOCK_WAITERS_GET;
		TO_ALIST(&_k_command_stack, NewGet);
	}
}


void task_mem_pool_defragment(kmemory_pool_t Pid)
{
	struct k_args A;

	A.Comm = _K_SVC_DEFRAG;
	A.args.p1.pool_id = Pid;
	KERNEL_ENTRY(&A);
}

/**
 *
 * @brief Allocate block from an existing block set
 *
 * @param pfraglevelinfo pointer to block set
 * @param piblockindex area to return index of first unused quad-block
 *                     when allocation fails
 *
 * @return pointer to allocated block, or NULL if none available
 */
static char *get_existing_block(struct pool_block_set *pfraglevelinfo,
				int *piblockindex)
{
	char *found = NULL;
	int i = 0;
	int status;
	int free_bit;

	do {
		/* give up if no more quad-blocks exist */

		if (pfraglevelinfo->quad_block[i].mem_blocks == NULL) {
			break;
		}

		/* allocate a block from current quad-block, if possible */

		status = pfraglevelinfo->quad_block[i].mem_status;
		if (status != 0x0) {
			/* identify first free block */
			free_bit = find_lsb_set(status) - 1;

			/* compute address of free block */
			found = pfraglevelinfo->quad_block[i].mem_blocks +
				(OCTET_TO_SIZEOFUNIT(free_bit *
					pfraglevelinfo->block_size));

			/* mark block as unavailable (using XOR to invert) */
			pfraglevelinfo->quad_block[i].mem_status ^=
				1 << free_bit;
#ifdef CONFIG_OBJECT_MONITOR
			pfraglevelinfo->count++;
#endif
			break;
		}

		/* move on to next quad-block; give up if at end of array */

	} while (++i < pfraglevelinfo->nr_of_entries);

	*piblockindex = i;
	return found;
}

/**
 *
 * @brief Allocate a block, recursively fragmenting larger blocks if necessary
 *
 * @param P memory pool descriptor
 * @param index index of block set currently being examined
 * @param startindex index of block set for which allocation is being done
 *
 * @return pointer to allocated block, or NULL if none available
 */
static char *get_block_recursive(struct pool_struct *P,
				 int index, int startindex)
{
	int i;
	char *found, *larger_block;
	struct pool_block_set *fr_table;

	/* give up if we've exhausted the set of maximum size blocks */

	if (index < 0) {
		return NULL;
	}

	/* try allocating a block from the current block set */

	fr_table = P->block_set;
	i = 0;

	found = get_existing_block(&(fr_table[index]), &i);
	if (found != NULL) {
		return found;
	}

#if AUTODEFRAG == AD_BEFORE_SEARCH4BIGGERBLOCK
	/*
	 * do a partial defragmentation of memory pool & try allocating again
	 * - do this on initial invocation only, not recursive ones
	 *   (since there is no benefit in repeating the defrag)
	 * - defrag only the blocks smaller than the desired size,
	 *   and only until the size needed is reached
	 *
	 * note: defragging at this time tries to preserve the memory pool's
	 * larger blocks by fragmenting them only when necessary
	 * (i.e. at the cost of doing more frequent auto-defragmentations)
	 */

	if (index == startindex) {
		defrag(P, P->nr_of_block_sets - 1, startindex);
		found = get_existing_block(&(fr_table[index]), &i);
		if (found != NULL) {
			return found;
		}
	}
#endif

	/* try allocating a block from the next largest block set */

	larger_block = get_block_recursive(P, index - 1, startindex);
	if (larger_block != NULL) {
		/*
		 * add a new quad-block to the current block set,
		 * then mark one of its 4 blocks as used and return it
		 *
		 * note: "i" was earlier set to indicate the first unused
		 * quad-block entry in the current block set
		 */

		fr_table[index].quad_block[i].mem_blocks = larger_block;
		fr_table[index].quad_block[i].mem_status = 0xE;
#ifdef CONFIG_OBJECT_MONITOR
		fr_table[index].count++;
#endif
		return larger_block;
	}

#if AUTODEFRAG == AD_AFTER_SEARCH4BIGGERBLOCK
	/*
	 * do a partial defragmentation of memory pool & try allocating again
	 * - do this on initial invocation only, not recursive ones
	 *   (since there is no benefit in repeating the defrag)
	 * - defrag only the blocks smaller than the desired size,
	 *   and only until the size needed is reached
	 *
	 * note: defragging at this time tries to limit the cost of doing
	 * auto-defragmentations by doing them only when necessary
	 * (i.e. at the cost of fragmenting the memory pool's larger blocks)
	 */

	if (index == startindex) {
		defrag(P, P->nr_of_block_sets - 1, startindex);
		found = get_existing_block(&(fr_table[index]), &i);
		if (found != NULL) {
			return found;
		}
	}
#endif

	return NULL; /* can't find (or create) desired block */
}

/**
 *
 * @brief Examine tasks that are waiting for memory pool blocks
 *
 * This routine attempts to satisfy any incomplete block allocation requests for
 * the specified memory pool. It can be invoked either by the explicit freeing
 * of a used block or as a result of defragmenting the pool (which may create
 * one or more new, larger blocks).
 *
 * @return N/A
 */
void _k_block_waiters_get(struct k_args *A)
{
	struct pool_struct *P = _k_mem_pool_list + OBJ_INDEX(A->args.p1.pool_id);
	char *found_block;
	struct k_args *curr_task, *prev_task;
	int offset;

	curr_task = P->waiters;
	/* forw is first field in struct */
	prev_task = (struct k_args *)&(P->waiters);

	/* loop all waiters */
	while (curr_task != NULL) {

		/* locate block set to try allocating from */
		offset = compute_block_set_index(
			P, curr_task->args.p1.req_size);

		/* allocate block (fragmenting a larger block, if needed) */
		found_block = get_block_recursive(
			P, offset, offset);

		/* if success : remove task from list and reschedule */
		if (found_block != NULL) {
			/* return found block */
			curr_task->args.p1.rep_poolptr = found_block;
			curr_task->args.p1.rep_dataptr = found_block;

			/* reschedule task */

#ifdef CONFIG_SYS_CLOCK_EXISTS
			if (curr_task->Time.timer) {
				_k_timeout_free(curr_task->Time.timer);
			}
#endif
			curr_task->Time.rcode = RC_OK;
			_k_state_bit_reset(curr_task->Ctxt.task, TF_GTBL);

			/* remove from list */
			prev_task->next = curr_task->next;

			/* and get next task */
			curr_task = curr_task->next;

		} else {
			/* else just get next task */
			prev_task = curr_task;
			curr_task = curr_task->next;
		}
	}

	/* put used command packet on the empty packet list */
	FREEARGS(A);
}

/**
 *
 * @brief Finish handling an allocate block request that timed out
 *
 * @return N/A
 */
void _k_mem_pool_block_get_timeout_handle(struct k_args *A)
{
	_k_timeout_free(A->Time.timer);
	REMOVE_ELM(A);
	A->Time.rcode = RC_TIME;
	_k_state_bit_reset(A->Ctxt.task, TF_GTBL);
}

/**
 *
 * @brief Perform allocate memory pool block request
 *
 * @return N/A
 */
void _k_mem_pool_block_get(struct k_args *A)
{
	struct pool_struct *P = _k_mem_pool_list + OBJ_INDEX(A->args.p1.pool_id);
	char *found_block;
	int offset;

	/* locate block set to try allocating from */

	offset = compute_block_set_index(P, A->args.p1.req_size);

	/* allocate block (fragmenting a larger block, if needed) */

	found_block = get_block_recursive(P, offset, offset);

	if (found_block != NULL) {
		A->args.p1.rep_poolptr = found_block;
		A->args.p1.rep_dataptr = found_block;
		A->Time.rcode = RC_OK;
		return;
	}

	/*
	 * no suitable block is currently available,
	 * so either wait for one to appear or indicate failure
	 */

	if (likely(A->Time.ticks != TICKS_NONE)) {
		A->priority = _k_current_task->priority;
		A->Ctxt.task = _k_current_task;
		_k_state_bit_set(_k_current_task, TF_GTBL);

		INSERT_ELM(P->waiters, A);

#ifdef CONFIG_SYS_CLOCK_EXISTS
		if (A->Time.ticks == TICKS_UNLIMITED) {
			A->Time.timer = NULL;
		} else {
			A->Comm = _K_SVC_MEM_POOL_BLOCK_GET_TIMEOUT_HANDLE;
			_k_timeout_alloc(A);
		}
#endif
	} else {
		A->Time.rcode = RC_FAIL;
	}
}


/**
 * @brief Helper function invoking POOL_BLOCK_GET command
 *
 * Info: Since the _k_mem_pool_block_get() invoked here is returning the
 * same pointer in both  A->args.p1.rep_poolptr and A->args.p1.rep_dataptr, we
 * are passing down only one address (in alloc_mem)
 *
 * @return RC_OK, RC_FAIL, RC_TIME on success, failure, timeout respectively
 */
int _do_task_mem_pool_alloc(kmemory_pool_t pool_id, int reqsize,
				 int32_t timeout, void **alloc_mem)
{
	struct k_args A;

	A.Comm = _K_SVC_MEM_POOL_BLOCK_GET;
	A.Time.ticks = timeout;
	A.args.p1.pool_id = pool_id;
	A.args.p1.req_size = reqsize;

	KERNEL_ENTRY(&A);
	*alloc_mem = A.args.p1.rep_poolptr;

	return A.Time.rcode;
}

/**
 *
 * @brief Allocate memory pool block request
 *
 * This routine allocates a free block from the specified memory pool, ensuring
 * that its size is at least as big as the size requested (in bytes).
 *
 * @param blockptr poitner to requested block
 * @param pool_id pool from which to get block
 * @param reqsize requested block size
 * @param time maximum number of ticks to wait
 *
 * @return RC_OK, RC_FAIL, RC_TIME on success, failure, timeout respectively
 */
int task_mem_pool_alloc(struct k_block *blockptr, kmemory_pool_t pool_id,
			 int reqsize, int32_t timeout)
{
	void *pool_ptr;
	int retval;

	retval = _do_task_mem_pool_alloc(pool_id, reqsize, timeout,
				 &pool_ptr);

	blockptr->pool_id = pool_id;
	blockptr->address_in_pool = pool_ptr;
	blockptr->pointer_to_data = pool_ptr;
	blockptr->req_size = reqsize;

	return retval;
}

#define MALLOC_ALIGN (sizeof(uint32_t))

/**
 * @brief Allocate memory from heap pool
 *
 * This routine  provides traditional malloc semantics; internally it uses
 * the microkernel pool APIs on a dedicated HEAP pool
 *
 * @param size Size of memory requested by the caller.
 *
 * @retval address of the block if successful otherwise returns NULL
 */
void *task_malloc(uint32_t size)
{
	uint32_t new_size;
	uint32_t *aligned_addr;
	void *pool_ptr;

	/* The address pool returns, may not be aligned. Also
	*  pool_free requires both start address and size. So
	*  we end up needing 2 slots to save the size and
	*  start address in addition to padding space
	*/
	new_size =  size + (sizeof(uint32_t) << 1) + MALLOC_ALIGN - 1;

	if (_do_task_mem_pool_alloc(_heap_mem_pool_id, new_size, TICKS_NONE,
					&pool_ptr) != RC_OK) {
		return NULL;
	}

	/* Get the next aligned address following the address returned by pool*/
	aligned_addr = (uint32_t *) ROUND_UP(pool_ptr, MALLOC_ALIGN);

	/* Save the size requested to the pool API, to be used while freeing */
	*aligned_addr = new_size;

	/* Save the original unaligned_addr pointer too */
	aligned_addr++;
	*((void **) aligned_addr) = pool_ptr;

	/* return the subsequent address */
	return ++aligned_addr;
}

/**
 *
 * @brief Perform return memory pool block request
 *
 * Marks a block belonging to a pool as free; if there are waiters that can use
 * the the block it is passed to a waiting task.
 *
 * @return N/A
 */
void _k_mem_pool_block_release(struct k_args *A)
{
	struct pool_struct *P;
	int Pid;
	int offset;

	Pid = A->args.p1.pool_id;

	P = _k_mem_pool_list + OBJ_INDEX(Pid);

	/* determine block set that block belongs to */

	offset = compute_block_set_index(P, A->args.p1.req_size);

	/* mark the block as unused */

	free_existing_block(A->args.p1.rep_poolptr, P, offset);

	/* reschedule anybody waiting for a block */

	if (P->waiters != NULL) {
		struct k_args *NewGet;

		/*
		 * create a command packet to re-try block allocation
		 * for the waiting tasks, and add it to the command stack
		 */

		GETARGS(NewGet);
		*NewGet = *A;
		NewGet->Comm = _K_SVC_BLOCK_WAITERS_GET;
		TO_ALIST(&_k_command_stack, NewGet);
	}

	if (A->alloc) {
		FREEARGS(A);
	}
}

void task_mem_pool_free(struct k_block *blockptr)
{
	struct k_args A;

	A.Comm = _K_SVC_MEM_POOL_BLOCK_RELEASE;
	A.args.p1.pool_id = blockptr->pool_id;
	A.args.p1.req_size = blockptr->req_size;
	A.args.p1.rep_poolptr = blockptr->address_in_pool;
	A.args.p1.rep_dataptr = blockptr->pointer_to_data;
	KERNEL_ENTRY(&A);
}

/**
 * @brief Free memory allocated through task_malloc
 *
 * @param ptr pointer to be freed
 *
 * @return NA
 */
void task_free(void *ptr)
{
	struct k_args A;

	A.Comm = _K_SVC_MEM_POOL_BLOCK_RELEASE;

	A.args.p1.pool_id = _heap_mem_pool_id;

	/* Fetch the pointer returned by the pool API */
	A.args.p1.rep_poolptr = *((void **) ((uint32_t *)ptr - 1));
	/* Further fetch the size asked from pool */
	A.args.p1.req_size = *((uint32_t *)ptr - 2);

	KERNEL_ENTRY(&A);
}
