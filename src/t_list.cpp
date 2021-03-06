/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "server.h"

/*-----------------------------------------------------------------------------
 * List API
 *----------------------------------------------------------------------------*/

/* The function pushes an element to the specified list object 'subject',
 * at head or tail position as specified by 'where'.
 *
 * There is no need for the caller to increment the refcount of 'value' as
 * the function takes care of it if needed. */
void listTypePush(robj *subject, robj *value, int where) {
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        int pos = (where == LIST_HEAD) ? QUICKLIST_HEAD : QUICKLIST_TAIL;
        value = getDecodedObject(value);
        size_t len = sdslen((sds)value->ptr);
        quicklistPush((quicklist *)subject->ptr, value->ptr, len, pos);
        decrRefCount(value);
    } else {
        serverPanic("Unknown list encoding");
    }
}

void *listPopSaver(unsigned char *data, unsigned int sz) {
    return createStringObject((char*)data,sz);
}

robj *listTypePop(robj *subject, int where) {
    long long vlong;
    robj *value = NULL;

    int ql_where = where == LIST_HEAD ? QUICKLIST_HEAD : QUICKLIST_TAIL;
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        if (quicklistPopCustom((quicklist *)subject->ptr, ql_where, (unsigned char **)&value,
                               NULL, &vlong, listPopSaver)) {
            if (!value)
                value = createStringObjectFromLongLong(vlong);
        }
    } else {
        serverPanic("Unknown list encoding");
    }
    return value;
}

unsigned long listTypeLength(const robj *subject) {
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        return quicklistCount((quicklist *)subject->ptr);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Initialize an iterator at the specified index. */
listTypeIterator *listTypeInitIterator(robj *subject, long index,
                                       unsigned char direction) {
    listTypeIterator* li = new (zmalloc(sizeof(listTypeIterator))) listTypeIterator(subject, index, direction);

    return li;
}

listTypeIterator::listTypeIterator(robj* in_subject, long index, unsigned char direction)
: genericIterator(in_subject)
, m_direction(direction)
, m_ql_iter(NULL)
{
    /* LIST_HEAD means start at TAIL and move *towards* head.
     * LIST_TAIL means start at HEAD and move *towards tail. */
    int iter_direction =
        m_direction == LIST_HEAD ? AL_START_TAIL : AL_START_HEAD;
    if (m_encoding == OBJ_ENCODING_QUICKLIST) {
        m_ql_iter = quicklistGetIteratorAtIdx((quicklist *)m_subject->ptr,
                                             iter_direction, index);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Clean up the iterator. */
void listTypeReleaseIterator(listTypeIterator *li) {
    li->~listTypeIterator();
    zfree(li);
}

listTypeIterator::~listTypeIterator()
{
    if (NULL != m_ql_iter)
        zfree(m_ql_iter);
}

/* Stores pointer to current the entry in the provided entry structure
 * and advances the position of the iterator. Returns 1 when the current
 * entry is in fact an entry, 0 otherwise. */
int listTypeIterator::listTypeNext(listTypeEntry *entry) {
    /* Protect from converting when iterating */
    serverAssert(m_subject->encoding == m_encoding);

    entry->li = this;
    if (m_encoding == OBJ_ENCODING_QUICKLIST) {
        if (m_ql_iter)
            return m_ql_iter->quicklistNext(entry->m_ql_entry);
        else
            return 0;
    } else {
        serverPanic("Unknown list encoding");
    }
    return 0;
}

/* Return entry or NULL at the current position of the iterator. */
robj *listTypeGet(listTypeEntry *entry) {
    robj *value = NULL;
    if (entry->li->encoding() == OBJ_ENCODING_QUICKLIST) {
        if (entry->m_ql_entry.m_value) {
            value = createStringObject((char *)entry->m_ql_entry.m_value,
                                       entry->m_ql_entry.m_size);
        } else {
            value = createStringObjectFromLongLong(entry->m_ql_entry.m_longval);
        }
    } else {
        serverPanic("Unknown list encoding");
    }
    return value;
}

void listTypeInsert(listTypeEntry *entry, robj *value, int where) {
    if (entry->li->encoding() == OBJ_ENCODING_QUICKLIST) {
        value = getDecodedObject(value);
        sds str = (sds)value->ptr;
        size_t len = sdslen(str);
        if (where == LIST_TAIL) {
            quicklistInsertAfter((quicklist *)entry->m_ql_entry.m_quicklist,
                                 &entry->m_ql_entry, str, len);
        } else if (where == LIST_HEAD) {
            quicklistInsertBefore((quicklist *)entry->m_ql_entry.m_quicklist,
                                  &entry->m_ql_entry, str, len);
        }
        decrRefCount(value);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Compare the given object with the entry at the current position. */
int listTypeEqual(listTypeEntry *entry, robj *o) {
    if (entry->li->encoding() == OBJ_ENCODING_QUICKLIST) {
        serverAssertWithInfo(NULL,o,sdsEncodedObject(o));
        return quicklistCompare((unsigned char *)entry->m_ql_entry.m_zip_list,(unsigned char *)o->ptr,sdslen((sds)o->ptr));
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Delete the element pointed to. */
void listTypeIterator::listTypeDelete(listTypeEntry *entry)
{
    if (entry->li->m_encoding == OBJ_ENCODING_QUICKLIST) {
        m_ql_iter->quicklistDelEntry(&entry->m_ql_entry);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Create a quicklist from a single ziplist */
void listTypeConvert(robj *subject, int enc) {
    serverAssertWithInfo(NULL,subject,subject->type==OBJ_LIST);
    serverAssertWithInfo(NULL,subject,subject->encoding==OBJ_ENCODING_ZIPLIST);

    if (enc == OBJ_ENCODING_QUICKLIST) {
        size_t zlen = server.list_max_ziplist_size;
        int depth = server.list_compress_depth;
        subject->ptr = quicklistCreateFromZiplist(zlen, depth, (unsigned char *)subject->ptr);
        subject->encoding = OBJ_ENCODING_QUICKLIST;
    } else {
        serverPanic("Unsupported list conversion");
    }
}

/*-----------------------------------------------------------------------------
 * List Commands
 *----------------------------------------------------------------------------*/

void pushGenericCommand(client *c, int where) {
    int j, pushed = 0;
    robj *lobj = lookupKeyWrite(c->m_cur_selected_db,c->m_argv[1]);

    if (lobj && lobj->type != OBJ_LIST) {
        c->addReply(shared.wrongtypeerr);
        return;
    }

    for (j = 2; j < c->m_argc; j++) {
        if (!lobj) {
            lobj = createQuicklistObject();
            quicklistSetOptions((quicklist *)lobj->ptr, server.list_max_ziplist_size,
                                server.list_compress_depth);
            dbAdd(c->m_cur_selected_db,c->m_argv[1],lobj);
        }
        listTypePush(lobj,c->m_argv[j],where);
        pushed++;
    }
    c->addReplyLongLong( (lobj ? listTypeLength(lobj) : 0));
    if (pushed) {
        const char *event = (where == LIST_HEAD) ? "lpush" : "rpush";

        signalModifiedKey(c->m_cur_selected_db,c->m_argv[1]);
        notifyKeyspaceEvent(NOTIFY_LIST,event,c->m_argv[1],c->m_cur_selected_db->m_id);
    }
    server.dirty += pushed;
}

void lpushCommand(client *c) {
    pushGenericCommand(c,LIST_HEAD);
}

void rpushCommand(client *c) {
    pushGenericCommand(c,LIST_TAIL);
}

void pushxGenericCommand(client *c, int where) {
    int j, pushed = 0;
    robj *subject;

    if ((subject = lookupKeyWriteOrReply(c,c->m_argv[1],shared.czero)) == NULL ||
        checkType(c,subject,OBJ_LIST)) return;

    for (j = 2; j < c->m_argc; j++) {
        listTypePush(subject,c->m_argv[j],where);
        pushed++;
    }

    c->addReplyLongLong(listTypeLength(subject));

    if (pushed) {
        const char *event = (where == LIST_HEAD) ? "lpush" : "rpush";
        signalModifiedKey(c->m_cur_selected_db,c->m_argv[1]);
        notifyKeyspaceEvent(NOTIFY_LIST,event,c->m_argv[1],c->m_cur_selected_db->m_id);
    }
    server.dirty += pushed;
}

void lpushxCommand(client *c) {
    pushxGenericCommand(c,LIST_HEAD);
}

void rpushxCommand(client *c) {
    pushxGenericCommand(c,LIST_TAIL);
}

void linsertCommand(client *c) {
    int where;
    robj *subject;
    listTypeEntry entry;
    int inserted = 0;

    if (strcasecmp((const char*)c->m_argv[2]->ptr,"after") == 0) {
        where = LIST_TAIL;
    } else if (strcasecmp((const char*)c->m_argv[2]->ptr,"before") == 0) {
        where = LIST_HEAD;
    } else {
        c->addReply(shared.syntaxerr);
        return;
    }

    if ((subject = lookupKeyWriteOrReply(c,c->m_argv[1],shared.czero)) == NULL ||
        checkType(c,subject,OBJ_LIST)) return;

    /* Seek pivot from head to tail */
    listTypeIterator iter(subject,0,LIST_TAIL);
    while (iter.listTypeNext(&entry)) {
        if (listTypeEqual(&entry,c->m_argv[3])) {
            listTypeInsert(&entry,c->m_argv[4],where);
            inserted = 1;
            break;
        }
    }

    if (inserted) {
        signalModifiedKey(c->m_cur_selected_db,c->m_argv[1]);
        notifyKeyspaceEvent(NOTIFY_LIST,"linsert",
                            c->m_argv[1],c->m_cur_selected_db->m_id);
        server.dirty++;
    } else {
        /* Notify client of a failed insert */
        c->addReply(shared.cnegone);
        return;
    }

    c->addReplyLongLong(listTypeLength(subject));
}

void llenCommand(client *c) {
    robj *o = lookupKeyReadOrReply(c,c->m_argv[1],shared.czero);
    if (o == NULL || checkType(c,o,OBJ_LIST)) return;
    c->addReplyLongLong(listTypeLength(o));
}

void lindexCommand(client *c) {
    robj *o = lookupKeyReadOrReply(c,c->m_argv[1],shared.nullbulk);
    if (o == NULL || checkType(c,o,OBJ_LIST)) return;
    long index;
    robj *value = NULL;

    if ((getLongFromObjectOrReply(c, c->m_argv[2], &index, NULL) != C_OK))
        return;

    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistEntry entry;
        if (entry.quicklistIndex((quicklist *)o->ptr, index)) {
            if (entry.m_value) {
                value = createStringObject((char*)entry.m_value,entry.m_size);
            } else {
                value = createStringObjectFromLongLong(entry.m_longval);
            }
            c->addReplyBulk(value);
            decrRefCount(value);
        } else {
            c->addReply(shared.nullbulk);
        }
    } else {
        serverPanic("Unknown list encoding");
    }
}

void lsetCommand(client *c) {
    robj *o = lookupKeyWriteOrReply(c,c->m_argv[1],shared.nokeyerr);
    if (o == NULL || checkType(c,o,OBJ_LIST)) return;
    long index;
    robj *value = c->m_argv[3];

    if ((getLongFromObjectOrReply(c, c->m_argv[2], &index, NULL) != C_OK))
        return;

    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklist* ql = (quicklist*)o->ptr;
        int replaced = quicklistReplaceAtIndex(ql, index,
                                               value->ptr, sdslen((sds)value->ptr));
        if (!replaced) {
            c->addReply(shared.outofrangeerr);
        } else {
            c->addReply(shared.ok);
            signalModifiedKey(c->m_cur_selected_db,c->m_argv[1]);
            notifyKeyspaceEvent(NOTIFY_LIST,"lset",c->m_argv[1],c->m_cur_selected_db->m_id);
            server.dirty++;
        }
    } else {
        serverPanic("Unknown list encoding");
    }
}

void popGenericCommand(client *c, int where) {
    robj *o = lookupKeyWriteOrReply(c,c->m_argv[1],shared.nullbulk);
    if (o == NULL || checkType(c,o,OBJ_LIST)) return;

    robj *value = listTypePop(o,where);
    if (value == NULL) {
        c->addReply(shared.nullbulk);
    } else {
        const char *event = (where == LIST_HEAD) ? "lpop" : "rpop";

        c->addReplyBulk(value);
        decrRefCount(value);
        notifyKeyspaceEvent(NOTIFY_LIST,event,c->m_argv[1],c->m_cur_selected_db->m_id);
        if (listTypeLength(o) == 0) {
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",
                                c->m_argv[1],c->m_cur_selected_db->m_id);
            dbDelete(c->m_cur_selected_db,c->m_argv[1]);
        }
        signalModifiedKey(c->m_cur_selected_db,c->m_argv[1]);
        server.dirty++;
    }
}

void lpopCommand(client *c) {
    popGenericCommand(c,LIST_HEAD);
}

void rpopCommand(client *c) {
    popGenericCommand(c,LIST_TAIL);
}

void lrangeCommand(client *c) {
    robj *o;
    long start, end, llen, rangelen;

    if ((getLongFromObjectOrReply(c, c->m_argv[2], &start, NULL) != C_OK) ||
        (getLongFromObjectOrReply(c, c->m_argv[3], &end, NULL) != C_OK)) return;

    if ((o = lookupKeyReadOrReply(c,c->m_argv[1],shared.emptymultibulk)) == NULL
         || checkType(c,o,OBJ_LIST)) return;
    llen = listTypeLength(o);

    /* convert negative indexes */
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    if (start > end || start >= llen) {
        c->addReply(shared.emptymultibulk);
        return;
    }
    if (end >= llen) end = llen-1;
    rangelen = (end-start)+1;

    /* Return the result in form of a multi-bulk reply */
    c->addReplyMultiBulkLen(rangelen);
    if (o->encoding == OBJ_ENCODING_QUICKLIST)
    {
        listTypeIterator iter(o, start, LIST_TAIL);

        while(rangelen--)
        {
            listTypeEntry entry;
            iter.listTypeNext(&entry);
            quicklistEntry *qe = &entry.m_ql_entry;
            if (qe->m_value) {
                c->addReplyBulkCBuffer(qe->m_value,qe->m_size);
            } else {
                c->addReplyBulkLongLong(qe->m_longval);
            }
        }

    } else {
        serverPanic("List encoding is not QUICKLIST!");
    }
}

void ltrimCommand(client *c) {
    robj *o;
    long start, end, llen, ltrim, rtrim;

    if ((getLongFromObjectOrReply(c, c->m_argv[2], &start, NULL) != C_OK) ||
        (getLongFromObjectOrReply(c, c->m_argv[3], &end, NULL) != C_OK)) return;

    if ((o = lookupKeyWriteOrReply(c,c->m_argv[1],shared.ok)) == NULL ||
        checkType(c,o,OBJ_LIST)) return;
    llen = listTypeLength(o);

    /* convert negative indexes */
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    if (start > end || start >= llen) {
        /* Out of range start or start > end result in empty list */
        ltrim = llen;
        rtrim = 0;
    } else {
        if (end >= llen) end = llen-1;
        ltrim = start;
        rtrim = llen-end-1;
    }

    /* Remove list elements to perform the trim */
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistDelRange((quicklist *)o->ptr,0,ltrim);
        quicklistDelRange((quicklist *)o->ptr,-rtrim,rtrim);
    } else {
        serverPanic("Unknown list encoding");
    }

    notifyKeyspaceEvent(NOTIFY_LIST,"ltrim",c->m_argv[1],c->m_cur_selected_db->m_id);
    if (listTypeLength(o) == 0) {
        dbDelete(c->m_cur_selected_db,c->m_argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->m_argv[1],c->m_cur_selected_db->m_id);
    }
    signalModifiedKey(c->m_cur_selected_db,c->m_argv[1]);
    server.dirty++;
    c->addReply(shared.ok);
}

void lremCommand(client *c) {
    robj *subject, *obj;
    obj = c->m_argv[3];
    long toremove;
    long removed = 0;

    if ((getLongFromObjectOrReply(c, c->m_argv[2], &toremove, NULL) != C_OK))
        return;

    subject = lookupKeyWriteOrReply(c,c->m_argv[1],shared.czero);
    if (subject == NULL || checkType(c,subject,OBJ_LIST)) return;

    long index = 0; // default when toremove >= 0
    unsigned char direction = LIST_TAIL;// default when toremove >= 0
    if (toremove < 0)
    {
        toremove = -toremove;
        index = -1;
        direction = LIST_HEAD;
    }

    listTypeIterator li(subject, index, direction);
    listTypeEntry entry;
    while (li.listTypeNext(&entry))
    {
        if (listTypeEqual(&entry,obj))
        {
            li.listTypeDelete(&entry);
            server.dirty++;
            removed++;
            if (toremove && removed == toremove)
                break;
        }
    }

    if (removed) {
        signalModifiedKey(c->m_cur_selected_db,c->m_argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"lrem",c->m_argv[1],c->m_cur_selected_db->m_id);
    }

    if (listTypeLength(subject) == 0) {
        dbDelete(c->m_cur_selected_db,c->m_argv[1]);
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->m_argv[1],c->m_cur_selected_db->m_id);
    }

    c->addReplyLongLong(removed);
}

/* This is the semantic of this command:
 *  RPOPLPUSH srclist dstlist:
 *    IF LLEN(srclist) > 0
 *      element = RPOP srclist
 *      LPUSH dstlist element
 *      RETURN element
 *    ELSE
 *      RETURN nil
 *    END
 *  END
 *
 * The idea is to be able to get an element from a list in a reliable way
 * since the element is not just returned but pushed against another list
 * as well. This command was originally proposed by Ezra Zygmuntowicz.
 */

void rpoplpushHandlePush(client *c, robj *dstkey, robj *dstobj, robj *value) {
    /* Create the list if the key does not exist */
    if (!dstobj) {
        dstobj = createQuicklistObject();
        quicklistSetOptions((quicklist *)dstobj->ptr, server.list_max_ziplist_size,
                            server.list_compress_depth);
        dbAdd(c->m_cur_selected_db,dstkey,dstobj);
    }
    signalModifiedKey(c->m_cur_selected_db,dstkey);
    listTypePush(dstobj,value,LIST_HEAD);
    notifyKeyspaceEvent(NOTIFY_LIST,"lpush",dstkey,c->m_cur_selected_db->m_id);
    /* Always send the pushed value to the client. */
    c->addReplyBulk(value);
}

void rpoplpushCommand(client *c) {
    robj *sobj, *value;
    if ((sobj = lookupKeyWriteOrReply(c,c->m_argv[1],shared.nullbulk)) == NULL ||
        checkType(c,sobj,OBJ_LIST)) return;

    if (listTypeLength(sobj) == 0) {
        /* This may only happen after loading very old RDB files. Recent
         * versions of Redis delete keys of empty lists. */
        c->addReply(shared.nullbulk);
    } else {
        robj *dobj = lookupKeyWrite(c->m_cur_selected_db,c->m_argv[2]);
        robj *touchedkey = c->m_argv[1];

        if (dobj && checkType(c,dobj,OBJ_LIST)) return;
        value = listTypePop(sobj,LIST_TAIL);
        /* We saved touched key, and protect it, since rpoplpushHandlePush
         * may change the client command argument vector (it does not
         * currently). */
        incrRefCount(touchedkey);
        rpoplpushHandlePush(c,c->m_argv[2],dobj,value);

        /* listTypePop returns an object with its refcount incremented */
        decrRefCount(value);

        /* Delete the source list when it is empty */
        notifyKeyspaceEvent(NOTIFY_LIST,"rpop",touchedkey,c->m_cur_selected_db->m_id);
        if (listTypeLength(sobj) == 0) {
            dbDelete(c->m_cur_selected_db,touchedkey);
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",
                                touchedkey,c->m_cur_selected_db->m_id);
        }
        signalModifiedKey(c->m_cur_selected_db,touchedkey);
        decrRefCount(touchedkey);
        server.dirty++;
    }
}

/*-----------------------------------------------------------------------------
 * Blocking POP operations
 *----------------------------------------------------------------------------*/

/* This is how the current blocking POP works, we use BLPOP as example:
 * - If the user calls BLPOP and the key exists and contains a non empty list
 *   then LPOP is called instead. So BLPOP is semantically the same as LPOP
 *   if blocking is not required.
 * - If instead BLPOP is called and the key does not exists or the list is
 *   empty we need to block. In order to do so we remove the notification for
 *   new data to read in the client socket (so that we'll not serve new
 *   requests if the blocking request is not served). Also we put the client
 *   in a dictionary (db->m_blocking_keys) mapping keys to a list of clients
 *   blocking for this keys.
 * - If a PUSH operation against a key with blocked clients waiting is
 *   performed, we mark this key as "ready", and after the current command,
 *   MULTI/EXEC block, or script, is executed, we serve all the clients waiting
 *   for this list, from the one that blocked first, to the last, accordingly
 *   to the number of elements we have in the ready list.
 */

/* Set a client in blocking mode for the specified key, with the specified
 * timeout */
void blockForKeys(client *c, robj **keys, int numkeys, mstime_t timeout, robj *target) {
    dictEntry *de;
    list *l;
    int j;

    c->m_blocking_state.m_timeout = timeout;
    c->m_blocking_state.m_target = target;

    if (target != NULL) incrRefCount(target);

    for (j = 0; j < numkeys; j++) {
        /* If the key already exists in the dict ignore it. */
        if (c->m_blocking_state.m_keys->dictAdd(keys[j],NULL) != DICT_OK) continue;
        incrRefCount(keys[j]);

        /* And in the other "side", to map keys -> clients */
        de = c->m_cur_selected_db->m_blocking_keys->dictFind(keys[j]);
        if (de == NULL) {
            int retval;

            /* For every key we take a list of clients blocked for it */
            l = listCreate();
            retval = c->m_cur_selected_db->m_blocking_keys->dictAdd(keys[j],l);
            incrRefCount(keys[j]);
            serverAssertWithInfo(c,keys[j],retval == DICT_OK);
        } else {
            l = (list *)de->dictGetVal();
        }
        l->listAddNodeTail(c);
    }
    blockClient(c,BLOCKED_LIST);
}

/* Unblock a client that's waiting in a blocking operation such as BLPOP.
 * You should never call this function directly, but unblockClient() instead. */
void client::unblockClientWaitingData() {
    dictEntry *de;

    serverAssertWithInfo(this,NULL,m_blocking_state.m_keys->dictSize() != 0);
    {
        dictIterator di(m_blocking_state.m_keys);
        /* The client may wait for multiple keys, so unblock it for every key. */
        while((de = di.dictNext()) != NULL) {
            robj *key = (robj *)de->dictGetKey();

            /* Remove this client from the list of clients waiting for this key. */
            list *l = (list *)(m_cur_selected_db->m_blocking_keys)->dictFetchValue(key);
            serverAssertWithInfo(this,key,l != NULL);
            l->listDelNode(l->listSearchKey(this));
            /* If the list is empty we need to remove it to avoid wasting memory */
            if (l->listLength() == 0)
                m_cur_selected_db->m_blocking_keys->dictDelete(key);
        }
    }
    
    /* Cleanup the client structure */
    m_blocking_state.m_keys->dictEmpty(NULL);
    if (m_blocking_state.m_target) {
        decrRefCount(m_blocking_state.m_target);
        m_blocking_state.m_target = NULL;
    }
}

/* If the specified key has clients blocked waiting for list pushes, this
 * function will put the key reference into the server.ready_keys list.
 * Note that db->m_ready_keys is a hash table that allows us to avoid putting
 * the same key again and again in the list in case of multiple pushes
 * made by a script or in the context of MULTI/EXEC.
 *
 * The list will be finally processed by handleClientsBlockedOnLists() */
void signalListAsReady(redisDb *db, robj *key) {
    readyList *rl;

    /* No clients blocking for this key? No need to queue it. */
    if (db->m_blocking_keys->dictFind(key) == NULL) return;

    /* Key was already signaled? No need to queue it again. */
    if (db->m_ready_keys->dictFind(key) != NULL) return;

    /* Ok, we need to queue this key into server.ready_keys. */
    rl = (readyList *)zmalloc(sizeof(*rl));
    rl->key = key;
    rl->db = db;
    incrRefCount(key);
    server.ready_keys->listAddNodeTail(rl);

    /* We also add the key in the db->m_ready_keys dictionary in order
     * to avoid adding it multiple times into a list with a simple O(1)
     * check. */
    incrRefCount(key);
    serverAssert(db->m_ready_keys->dictAdd(key,NULL) == DICT_OK);
}

/* This is a helper function for handleClientsBlockedOnLists(). It's work
 * is to serve a specific client (receiver) that is blocked on 'key'
 * in the context of the specified 'db', doing the following:
 *
 * 1) Provide the client with the 'value' element.
 * 2) If the dstkey is not NULL (we are serving a BRPOPLPUSH) also push the
 *    'value' element on the destination list (the LPUSH side of the command).
 * 3) Propagate the resulting BRPOP, BLPOP and additional LPUSH if any into
 *    the AOF and replication channel.
 *
 * The argument 'where' is LIST_TAIL or LIST_HEAD, and indicates if the
 * 'value' element was popped fron the head (BLPOP) or tail (BRPOP) so that
 * we can propagate the command properly.
 *
 * The function returns C_OK if we are able to serve the client, otherwise
 * C_ERR is returned to signal the caller that the list POP operation
 * should be undone as the client was not served: This only happens for
 * BRPOPLPUSH that fails to push the value to the destination key as it is
 * of the wrong type. */
int serveClientBlockedOnList(client *receiver, robj *key, robj *dstkey, redisDb *db, robj *value, int where)
{
    robj *argv[3];

    if (dstkey == NULL) {
        /* Propagate the [LR]POP operation. */
        argv[0] = (where == LIST_HEAD) ? shared.lpop :
                                          shared.rpop;
        argv[1] = key;
        propagate((where == LIST_HEAD) ?
            server.lpopCommand : server.rpopCommand,
            db->m_id,argv,2,PROPAGATE_AOF|PROPAGATE_REPL);

        /* BRPOP/BLPOP */
        receiver->addReplyMultiBulkLen(2);
        receiver->addReplyBulk(key);
        receiver->addReplyBulk(value);
    } else {
        /* BRPOPLPUSH */
        robj *dstobj =
            lookupKeyWrite(receiver->m_cur_selected_db,dstkey);
        if (!(dstobj &&
             checkType(receiver,dstobj,OBJ_LIST)))
        {
            /* Propagate the RPOP operation. */
            argv[0] = shared.rpop;
            argv[1] = key;
            propagate(server.rpopCommand,
                db->m_id,argv,2,
                PROPAGATE_AOF|
                PROPAGATE_REPL);
            rpoplpushHandlePush(receiver,dstkey,dstobj,
                value);
            /* Propagate the LPUSH operation. */
            argv[0] = shared.lpush;
            argv[1] = dstkey;
            argv[2] = value;
            propagate(server.lpushCommand,
                db->m_id,argv,3,
                PROPAGATE_AOF|
                PROPAGATE_REPL);
        } else {
            /* BRPOPLPUSH failed because of wrong
             * destination type. */
            return C_ERR;
        }
    }
    return C_OK;
}

/* This function should be called by Redis every time a single command,
 * a MULTI/EXEC block, or a Lua script, terminated its execution after
 * being called by a client.
 *
 * All the keys with at least one client blocked that received at least
 * one new element via some PUSH operation are accumulated into
 * the server.ready_keys list. This function will run the list and will
 * serve clients accordingly. Note that the function will iterate again and
 * again as a result of serving BRPOPLPUSH we can have new blocking clients
 * to serve because of the PUSH side of BRPOPLPUSH. */
void handleClientsBlockedOnLists() {
    while(server.ready_keys->listLength() != 0) {

        /* Point server.ready_keys to a fresh list and save the current one
         * locally. This way as we run the old list we are free to call
         * signalListAsReady() that may push new elements in server.ready_keys
         * when handling clients blocked into BRPOPLPUSH. */
        list *l = server.ready_keys;
        server.ready_keys = listCreate();

        while(l->listLength() != 0) {
            listNode *ln = l->listFirst();
            readyList *rl = (readyList *)ln->listNodeValue();

            /* First of all remove this key from db->m_ready_keys so that
             * we can safely call signalListAsReady() against this key. */
            rl->db->m_ready_keys->dictDelete(rl->key);

            /* If the key exists and it's a list, serve blocked clients
             * with data. */
            robj *o = lookupKeyWrite(rl->db,rl->key);
            if (o != NULL && o->type == OBJ_LIST) {
                dictEntry *de;

                /* We serve clients in the same order they blocked for
                 * this key, from the first blocked to the last. */
                de = rl->db->m_blocking_keys->dictFind(rl->key);
                if (de) {
                    list* clients = (list*)de->dictGetVal();
                    int numclients = clients->listLength();

                    while(numclients--) {
                        listNode *clientnode = clients->listFirst();
                        client *receiver = (client *)clientnode->listNodeValue();
                        robj *dstkey = receiver->m_blocking_state.m_target;
                        int where = (receiver->m_last_cmd &&
                                     receiver->m_last_cmd->proc == blpopCommand) ?
                                    LIST_HEAD : LIST_TAIL;
                        robj *value = listTypePop(o,where);

                        if (value) {
                            /* Protect receiver->bpop.target, that will be
                             * freed by the next unblockClient()
                             * call. */
                            if (dstkey) incrRefCount(dstkey);
                            receiver->unblockClient();

                            if (serveClientBlockedOnList(receiver,
                                rl->key,dstkey,rl->db,value,
                                where) == C_ERR)
                            {
                                /* If we failed serving the client we need
                                 * to also undo the POP operation. */
                                    listTypePush(o,value,where);
                            }

                            if (dstkey) decrRefCount(dstkey);
                            decrRefCount(value);
                        } else {
                            break;
                        }
                    }
                }

                if (listTypeLength(o) == 0) {
                    dbDelete(rl->db,rl->key);
                }
                /* We don't call signalModifiedKey() as it was already called
                 * when an element was pushed on the list. */
            }

            /* Free this item. */
            decrRefCount(rl->key);
            zfree(rl);
            l->listDelNode(ln);
        }
        listRelease(l); /* We have the new list on place at this point. */
    }
}

/* Blocking RPOP/LPOP */
void blockingPopGenericCommand(client *c, int where) {
    robj *o;
    mstime_t timeout;
    int j;

    if (getTimeoutFromObjectOrReply(c,c->m_argv[c->m_argc-1],&timeout,UNIT_SECONDS)
        != C_OK) return;

    for (j = 1; j < c->m_argc-1; j++) {
        o = lookupKeyWrite(c->m_cur_selected_db,c->m_argv[j]);
        if (o != NULL) {
            if (o->type != OBJ_LIST) {
                c->addReply(shared.wrongtypeerr);
                return;
            } else {
                if (listTypeLength(o) != 0) {
                    /* Non empty list, this is like a non normal [LR]POP. */
                    const char *event = (where == LIST_HEAD) ? "lpop" : "rpop";
                    robj *value = listTypePop(o,where);
                    serverAssert(value != NULL);

                    c->addReplyMultiBulkLen(2);
                    c->addReplyBulk(c->m_argv[j]);
                    c->addReplyBulk(value);
                    decrRefCount(value);
                    notifyKeyspaceEvent(NOTIFY_LIST,(char *)event,
                                        c->m_argv[j],c->m_cur_selected_db->m_id);
                    if (listTypeLength(o) == 0) {
                        dbDelete(c->m_cur_selected_db,c->m_argv[j]);
                        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",
                                            c->m_argv[j],c->m_cur_selected_db->m_id);
                    }
                    signalModifiedKey(c->m_cur_selected_db,c->m_argv[j]);
                    server.dirty++;

                    /* Replicate it as an [LR]POP instead of B[LR]POP. */
                    c->rewriteClientCommandVector(2,
                        (where == LIST_HEAD) ? shared.lpop : shared.rpop,
                        c->m_argv[j]);
                    return;
                }
            }
        }
    }

    /* If we are inside a MULTI/EXEC and the list is empty the only thing
     * we can do is treating it as a timeout (even with timeout 0). */
    if (c->m_flags & CLIENT_MULTI) {
        c->addReply(shared.nullmultibulk);
        return;
    }

    /* If the list is empty or the key does not exists we must block */
    blockForKeys(c, c->m_argv + 1, c->m_argc - 2, timeout, NULL);
}

void blpopCommand(client *c) {
    blockingPopGenericCommand(c,LIST_HEAD);
}

void brpopCommand(client *c) {
    blockingPopGenericCommand(c,LIST_TAIL);
}

void brpoplpushCommand(client *c) {
    mstime_t timeout;

    if (getTimeoutFromObjectOrReply(c,c->m_argv[3],&timeout,UNIT_SECONDS)
        != C_OK) return;

    robj *key = lookupKeyWrite(c->m_cur_selected_db, c->m_argv[1]);

    if (key == NULL) {
        if (c->m_flags & CLIENT_MULTI) {
            /* Blocking against an empty list in a multi state
             * returns immediately. */
            c->addReply( shared.nullbulk);
        } else {
            /* The list is empty and the client blocks. */
            blockForKeys(c, c->m_argv + 1, 1, timeout, c->m_argv[2]);
        }
    } else {
        if (key->type != OBJ_LIST) {
            c->addReply( shared.wrongtypeerr);
        } else {
            /* The list exists and has elements, so
             * the regular rpoplpushCommand is executed. */
            serverAssertWithInfo(c,key,listTypeLength(key) > 0);
            rpoplpushCommand(c);
        }
    }
}
