#include "postgres.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "storage/bufmgr.h"
#include "access/relation.h"
#include "utils/varlena.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_bufmgr);


#define MAX_BLK_ENTRIES 200
#define MAX_BUFFER_ENTRIES 128
#define INVALID_BUFID -1


static Relation test_rel = NULL;
static int blkno2bufid[MAX_BLK_ENTRIES];

static void read_unpin_block (uint32 blkno);
static Buffer read_pin_block (uint32 blkno);
static void unpin_block (uint32 blkno);
static void InitTestBufferPool (Relation   rel);
static Relation InitTest (text *relname);



static void
read_unpin_block (uint32 blkno)
{
	Buffer		buf;
	int		bufid;

	if (blkno >= RelationGetNumberOfBlocksInFork(test_rel, MAIN_FORKNUM))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("block number %u is out of range for relation \"%s\"",
						blkno, RelationGetRelationName(test_rel))));

	buf = ReadBuffer(test_rel, blkno);
	ReleaseBuffer(buf);
	bufid = buf - 1;

	elog(NOTICE,"test_bufmgr read_unpin_block blkno %u bufid %d", blkno, bufid);
}



static Buffer
read_pin_block (uint32 blkno)
{
	Buffer		buf;
	int		bufid;

	if (blkno >= RelationGetNumberOfBlocksInFork(test_rel, MAIN_FORKNUM))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("block number %u is out of range for relation \"%s\"",
						blkno, RelationGetRelationName(test_rel))));

	Assert (blkno < MAX_BLK_ENTRIES);
	buf = ReadBuffer(test_rel, blkno);
	bufid = buf - 1;
	blkno2bufid[blkno] = bufid;

	elog(NOTICE,"test_bufmgr read_pin_block blkno %u bufid %d", blkno, bufid);
	return buf;
}




static void 
unpin_block (uint32 blkno)
{
	int    bufid;
	Buffer buf;


	Assert (blkno < MAX_BLK_ENTRIES);
	bufid = blkno2bufid[blkno];
	elog(NOTICE,"test_bufmgr unpin_block blkno %u bufid %d", blkno, bufid);
	Assert (bufid != INVALID_BUFID);
	buf = bufid + 1;
	ReleaseBuffer(buf);
	
}


static void 
InitTestBufferPool (Relation   rel)
{
	int		bufid;
	int		i;
	uint32 		blkno;
	Buffer		buf;
	uint32		bufid2blkno[MAX_BUFFER_ENTRIES];

	for (blkno = 0; blkno < NBuffers; blkno++)
	{
		/* read and pin blocks */
		buf = ReadBuffer(rel, blkno);
		bufid = buf - 1;
		Assert ((bufid >= 0) && (bufid < NBuffers));
		bufid2blkno[bufid] = blkno;
	}

	/* read and unpin blocks */
	for (i=0; i<2; i++)
	{
		for (bufid = 0; bufid < NBuffers; bufid++)
		{
			blkno = bufid2blkno[bufid];
			buf = ReadBuffer(rel, blkno);
			ReleaseBuffer(buf);
		}
	}

	/* unpin blocks */
	for (bufid = 0; bufid < NBuffers; bufid++)
	{
		buf = bufid + 1;
		ReleaseBuffer(buf);
	}
}


static Relation 
InitTest (text *relname)
{
	RangeVar   *relrv;
	Relation   rel;
	int		i;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use raw functions"))));

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	rel = relation_openrv(relrv, RowExclusiveLock);		

	/* Check that this relation has storage */
	if (rel->rd_rel->relkind == RELKIND_VIEW)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot get raw page from view \"%s\"",
						RelationGetRelationName(rel))));
	if (rel->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot get raw page from composite type \"%s\"",
						RelationGetRelationName(rel))));
	if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot get raw page from foreign table \"%s\"",
						RelationGetRelationName(rel))));

	/*
	 * Reject attempts to read non-local temporary relations; we would be
	 * likely to get wrong data since we have no visibility into the owning
	 * session's local buffers.
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary tables of other sessions")));

	for (i=0; i <MAX_BLK_ENTRIES; i++)
		blkno2bufid[i] = INVALID_BUFID;

	InitTestBufferPool (rel);
	return rel;
}



Datum
test_bufmgr (PG_FUNCTION_ARGS)
{
	bool 	   result = true; 
	int32      testnum = PG_GETARG_INT32(1); // testcase number
        text       *relname = PG_GETARG_TEXT_P(0); // relation name

	test_rel = InitTest (relname);
	elog(NOTICE,"test_bufmgr testcase %s %d", RelationGetRelationName(test_rel), testnum);

	switch (testnum) {
		case 0:
			#include "testcases/testcase0.c"
			break;
		case 1:
			#include "testcases/testcase1.c"
			break;
		case 2:
			#include "testcases/testcase2.c"
			break;
		case 3:
			#include "testcases/testcase3.c"
			break;
		case 4:
			#include "testcases/testcase4.c"
			break;
		case 5:
			#include "testcases/testcase5.c"
			break;
		case 6:
			#include "testcases/testcase6.c"
			break;
		case 7:
			#include "testcases/testcase7.c"
			break;
		case 8:
			#include "testcases/testcase8.c"
			break;
		case 9:
			#include "testcases/testcase9.c"
			break;
		default:
			elog(ERROR,"test_bufmgr testcase %d not found", testnum);
			result = false;
	}

	relation_close(test_rel, RowExclusiveLock);		
	PG_RETURN_BOOL(result);
}


