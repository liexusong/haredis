#include "server.h"
#include "raft.h"
#include "rand.h"
#include <assert.h>
#include "read_only.h"

bool matchVoteInfo(voteInfo* a, voteInfo* b)
{
    return a->id == b->id;
}

unsigned int dictKeyHash(const void *keyp) {
    unsigned long key = (unsigned long)keyp;
    key = dictGenHashFunction(&key,sizeof(key));
    key += ~(key << 15);
    key ^=  (key >> 10);
    key +=  (key << 3);
    key ^=  (key >> 6);
    key += ~(key << 11);
    key ^=  (key >> 16);
    return key;
}

int dictKeyCompare(void *privdata, const void *key1, const void *key2) {
    unsigned long k1 = (unsigned long)key1;
    unsigned long k2 = (unsigned long)key2;
    return k1 == k2;
}

dictType intKeydictType = {
    dictKeyHash,                   /* hash function */
    NULL,                          /* key dup */
    NULL,                          /* val dup */
    dictKeyCompare,                /* key compare */
    NULL,                          /* key destructor */
    NULL                           /* val destructor */
};

raft* newRaft(raftConfig* cfg)
{
    raftLog* log = newRaftLog(cfg->storage);
    hardState hs = getHardState(cfg->storage);
    confState *cs = getConfState(cfg->storage);
    list* peers = cfg->peers;
    if(listLength(cs->peers) > 0)
    {
        if(listLength(peers) > 0)
        {
            assert(false);
        }
        peers = cs->peers;
    }
    raft* r = zmalloc(sizeof(raft));
    r->id = cfg->id;
    r->leader = 0;
    r->maxSizePerMsg = cfg->maxSizePerMsg;
    r->maxInflightMsgs = cfg->maxInflightMsgs;
    r->peers = dictCreate(&intKeydictType, NULL);
    r->votes = dictCreate(&intKeydictType, NULL);
    //listSetFreeMethod(free); todo
    r->electionTimeout = cfg->electionTick;
    r->heartbeatTimeout = cfg->heartbeatTick;
    r->checkQuorum = cfg->checkQuorum;
    r->msgs = listCreate();
    listSetFreeMethod(r->msgs, freeRaftMessage);
    listSetDupMethod(r->msgs, dupRaftMessage);
    r->readStates = listCreate();
    listSetFreeMethod(r->readStates, freeReadState);
    listSetDupMethod(r->readStates, dupReadState);
    listIter li;  
    listRewind(cfg->peers,&li);
    uint8_t peer;
    listNode* ln;
    while ((ln = listNext(&li)) != NULL) {
        peer = (uint8_t)ln->value;
        raftNodeProgress* pr = newRaftNodeProgress(peer, r->maxInflightMsgs);
        pr->next = 1;
        dictAdd(r->peers, peer, pr);
    }

    becomeFollower(r, r->term, 0);
    return r;

}

void resetRaftTerm(raft* r, uint64_t term)
{
    if(r->term != term)
    {
        r->term = term;
        r->voteFor = 0;
    }
    r->leader = 0;
    r->electionElapsed = 0;
    r->heartbeatElapsed = 0;
    r->electionRandomTimeout = r->electionTimeout+ redisLrand48() % r->electionTimeout;
    dictEmpty(r->votes, NULL);
    dictIterator* it = dictGetIterator(r->peers);
    raftNodeProgress* progress;
    dictEntry* e = dictNext(it);
    while (e != NULL) {
        progress = dictGetVal(e);
        resetRaftNodeProgress(progress, NodeStateProb);
        progress->next = lastIndex(r->raftlog) + 1;
        if (dictGetKey(e) == r->id) {
            return progress->match = lastIndex(r->raftlog);
        }
        e = dictNext(it);
    }    
}

void becomeFollower(raft* r, uint64_t term, uint64_t leader)
{
    r->step = stepFollower;
    resetRaftTerm(r, term);
    r->tick = tickElection;
    r->leader = leader;
    r->state = NodeStateFollower;
}


void becomeCandidate(raft* r)
{
    assert(r->state != NodeStateLeader);
    r->step = stepLeader;
    resetRaftTerm(r, r->term + 1);
    r->tick = tickElection;
    r->voteFor = r->id;
    r->state = NodeStateCandidate;
}

void becomeLeader(raft* r)
{
    assert(r->state != NodeStateFollower);
    r->step = stepLeader;
    resetRaftTerm(r, r->term);
    r->tick = r->heartbeatTimeout;
    r->leader = r->id;
    r->state = NodeStateLeader;
    raftEntry* entry = createRaftEntry();
    appendEntry(r, entry);
}


raftNodeProgress* getProgress(raft* r, uint8_t id)
{
    dictIterator* it = dictGetIterator(r->peers);
    dictEntry* e = dictNext(it);
    while (e != NULL) {
        if (dictGetKey(e) == id) {
            return dictGetVal(e);
        }
        e = dictNext(it);
    }   
    return NULL;
}


void stepLeader(struct raft* r, raftMessage* msg)
{
    switch(msg->type)
    {
        case MessageBeat:
            broadcastHeartbeat(r);
            return;
        case MessageCheckQuorum:
            if(!checkQuorumActive(r))
            {
                becomeFollower(r, r->term, 0);
            }
            return;
        case MessageProp:
            assert(listLength(msg->entries) != 0);
            raftNodeProgress* pr = getProgress(r, r->id);
            if(pr == NULL)
            {
                return;
            }
            listNode* n = listFirst(msg->entries);
            while(n != NULL)
            {
                raftEntry* ent = n->value;
                if(ent->entryType == EntryConfChange)
                {
                    if(r->pendingConf)
                    {
                        ent->entryType = EntryNormal;
                    }
                    r->pendingConf = true;
                }               
            }
            appendEntries(r, msg->entries);
            broadCastAppend(r);
            return;
        case MessageReadIndex:
            if(quorum(r) > 1)
            {
                TermResult res = termOf(r, r->raftlog->commited);
                uint64_t t = zeroTermOnErrCompacted(res.term, res.err);
                if(t != r->term)
                {
                    return;
                }
                if(msg->from == 0 || msg->from == r->id)
                {
                    ReadState *rs = createReadState();
                    rs->index = r->raftlog->commited;
                    raftEntry* ent = listFirst(msg->entries)->value;
                    rs->requestCtx = sdsdup(ent->data);
                    listAddNodeTail(r->readStates, rs);
                }else 
                {
                    raftMessage* m = createRaftMessage();
                    m->to = msg->from;
                    m->type = MessageReadIndexResp;
                    m->index = r->raftlog->commited;
                    listRelease(m->entries);
                    m->entries = listDup(msg->entries);
                    sendMsg(r, m);
                }
            }else 
            {
                ReadState *rs = createReadState();
                rs->index = r->raftlog->commited;
                raftEntry* ent = listFirst(msg->entries)->value;
                rs->requestCtx = sdsdup(ent->data);
                listAddNodeTail(r->readStates, rs);
            }
            return;
    }

    raftNodeProgress* pr = getProgress(r, msg->from);
    if(pr == NULL)
    {
        return;
    }
    
    switch(msg->type)
    {
        case MessageAppResp:
            pr->active = true;
            if(msg->reject)
            {
                if(maybeDecrTo(pr, msg->index, msg->lastMatchIndex))
                {
                    if(pr->state == NodeStateReplicate)
                    {
                        becomeProbe(pr);
                    }
                }
                sendAppend(msg->from);
            }else
            {
                bool can_send = canSend(pr);
                if(maybeUpdate(pr, msg->index))
                {
                    switch(pr->state)
                    {
                        case NodeStateProb:
                        {
                            becomeReplicate(pr);
                            break;
                        }
                        case NodeStateReplicate:
                        {
                            removeInflights(pr->ins, msg->index);
                            break;
                        }
                        case NodeStateSnapshot:
                        {
                            if(shouldAbortSnapshot(pr))
                            {
                                becomeProbe(pr);
                            }
                            break;
                        }
                    }
                    if(maybeCommitRaft(r))//todo
                    {
                        broadCastAppend(r);
                    }else if(!can_send)
                    {
                        sendAppend(msg->from);
                    }
                }
            }
            break;
        case MessageHeartBeat:
            pr->active = true;
            resumeProgress(pr);
            if(pr->state == NodeStateReplicate && isInflightsFull(pr->ins))
            {
                freeFirstOneInflight(pr->ins);
            }
            if(pr->match < lastIndex(r->raftlog))
            {
                sendAppend(r, msg->from);
            }
            break;
        case MessageSnapStatus:
            if(pr->state != NodeStateSnapshot)
            {
                return;
            }
            if(msg->reject)
            {
                abortSnapshot(pr);
            }
            becomeProbe(pr);
            pauseProgress(pr);
            break;
        case MessageUnreachable:
            if(pr->state == NodeStateReplicate)
            {
                becomeProbe(pr);
            }
    }

}

int pollRaft(raft* r, uint64_t id, bool v)
{
    dictEntry* e = dictFind(r->votes, id);
    if(e == NULL)
    {
        dictAdd(r->votes, id, 1);
    }
    int peers = dictSize(r->votes);
    int granted = 0;
    dictIterator* it = dictGetIterator(r->votes);
    e = dictNext(it);
    while(e != NULL)
    {
        if(e->v.val == 1)
        {
            granted++;
        } 
    }
    return granted;
}


void stepCandidate(struct raft* r, raftMessage* msg)
{
    switch(msg->type)
    {
        case MessageProp:
        {
            serverLog(LL_NOTICE, "%d no leader at term %d; dropping proposal", r->id, r->term);
            break;
        }           
        case MessageApp:
        {
            becomeFollower(r, r->term, msg->from);
            handleAppendEntries(r, msg);
            break;
        }      
        case MessageSnap:
        {
            becomeFollower(r, r->term, msg->from);
            handleHeartBeat(r, msg);
            break;
        }
        case MessageVoteResp:
        {
            int granted = pollRaft(r, msg->from, !msg->reject);
            int quo = quorum(r);
            if(quo == granted)
            {
                becomeLeader(r);
                broadCastAppend(r);
            }else if(quo == dictSize(r->votes) - granted)
            {
                becomeFollower(r, msg->from, 0);
            }
            break;
        }         
        default:
            break;
    }
}

void stepFollower(struct raft* r, raftMessage* msg)
{
    switch(msg->type)
    {
        case MessageProp:
        {
            if(r->leader == 0)
            {
                return;
            }
            msg->to = r->leader;
            sendMsg(r, msg);
            break;
        }           
        case MessageApp:
        {
            r->electionElapsed = 0;
            r->leader = msg->from;
            handleAppendEntries(r, msg);//todo
            break;
        }
        case MessageHeartBeat:
        {
            r->electionElapsed = 0;
            r->leader = msg->from;
            handleHeartBeat(r, msg);
            break;
        }
        case MessageSnap:
        {
            r->electionElapsed = 0;
            r->leader = msg->from;
            handleSnapshot(r, msg);
            break;
        }
        case MessageReadIndex:
        {
            msg->to = r->leader;
            sendMsg(r, msg);
            break;
        }
        case MessageReadIndexResp:
        {
            ReadState *rs = createReadState();
            rs->index = msg->index;
            raftEntry* ent = listFirst(msg->entries)->value;
            rs->requestCtx = sdsdup(ent->data);
            listAddNodeTail(r->readStates, rs);
            break;
        }
        default:
            break;
    }
}

bool promotable(raft* r)
{
    dictEntry* e = dictFind(r->peers, r->id);
    return e != NULL;
}

bool pastElectionTimeout(raft* r)
{
    return r->electionElapsed >= r->electionRandomTimeout;
}

void tickElection(raft* r)
{
    r->electionElapsed++;
    if(promotable(r) && pastElectionTimeout(r))
    {
        r->electionElapsed = 0;
        raftMessage* m = createRaftMessage();
        m->to = r->id;
        m->type = MessageHup;
        Step(r, m);
        freeRaftMessage(m);
    }
}

void tickHeartbeat(raft* r)
{
    r->electionElapsed++;
    r->heartbeatElapsed++;
    if(r->electionElapsed >= r->electionTimeout)
    {
        r->electionElapsed = 0;
        raftMessage* m = createRaftMessage();
        m->from = r->id;
        m->type = MessageCheckQuorum;
        Step(r, m);
        freeRaftMessage(m);
    }
    if(r->state != NodeStateLeader)
    {
        return;
    }
    if(r->heartbeatElapsed >= r->heartbeatTimeout)
    {
        r->heartbeatElapsed = 0;
        raftMessage* m = createRaftMessage();
        m->from = r->id;
        m->type = MessageBeat;
        Step(r, m);
        freeRaftMessage(m);
    }
}

void appendEntry(raft* r, raftEntry* entry)
{

}

void appendEntries(raft* r, list* entries)
{
    uint64_t last_index = lastIndex(r->raftlog);
    listNode* n = listFirst(entries);
    uint64_t off = 1;
    while(n != NULL)
    {
        raftEntry* ent = n->value;
        ent->term = r->term;
        ent->index = last_index + off;
        off++;
    }
    append(r->raftlog, entries);
    raftNodeProgress* pr = getProgress(r, r->id);
    maybeUpdate(pr, lastIndex(r->raftlog));
    maybeCommitRaft(r);
}


int quorum(raft* r)
{
    return dictSize(r->peers)/2 + 1;
}

void sendMsg(raft* r, raftMessage* msg)
{
    msg->from = r->id;
    if(msg->type == MessageVote || msg->type == MessageVoteResp)
    {
        if(msg->term == 0)
        {
            assert(false);
        }
    }else 
    {
        if(msg->type != 0)
        {
            assert(false);
        }
        if(msg->type != MessageProp && msg->type != MessageReadIndex)
        {
            msg->term = r->term;
        }
    }
    listAddNodeTail(r->msgs, msg);
}

void sendAppend(raft* r, uint64_t to)
{
    raftNodeProgress* pr = getProgress(r, to);
    if(!canSend(pr))
    {
        return;
    }
    raftMessage* msg = createRaftMessage();
    msg->to = to;
    TermResult term_res = termOf(r->raftlog, pr->next - 1);
    EntriesResult entries_res = entriesOfLog(r->raftlog, pr->next, UINT64_MAX);
    if(term_res.err != StorageOk || entries_res.err != StorageOk)
    {
        if(!pr->active)
        {
            return;
        }
        msg->type = MessageSnap;
        //TODO
    }else
    {
        msg->type = MessageApp;
        msg->index = pr->next - 1;
        msg->logTerm = term_res.term;
        msg->entries = entries_res.entries;
        msg->commited = r->raftlog->commited;
        int len = listLength(msg->entries);
        if(len != 0)
        {
            if(pr->state == NodeStateReplicate)
            {
                raftEntry* ent = listLast(msg->entries)->value;
                uint64_t last_index = ent->index;
                optimisticUpdate(pr, last_index);
                addInflight(pr->ins, last_index);
            }else if(pr->state == NodeStateProb)
            {
                //todo
            }else
            {
                assert(false);
            }
        }

    }
    sendMsg(r, msg);
}


int numOfPendingConf(list* ents)
{
    int num = 0;
    listNode* n = listFirst(ents);
    while(n != NULL)
    {
        raftEntry* ent = n->value;
        if(ent->entryType == EntryConfChange)
        {
            num++;
        }
    }    
    return num;
}

bool Step(raft* r, raftMessage* msg)
{
    if(msg->term == 0)
    {
    }
    else if(msg->term > r->term)
    {
        if(msg->type ==  MessageVote)
        {
            //todo compaignTransfer
            bool in_lease = r->checkQuorum && r->leader != 0 && r->electionElapsed < r->electionTimeout;
            if(in_lease)
            {
                return true;
            }
        }
        if(msg->type == MessageApp || msg->type == MessageHeartBeat || msg->type == MessageSnap)
        {
            becomeFollower(r, msg->term, msg->from);
        }else
        {
            becomeFollower(r, msg->term, 0);
        }
    }else if(msg->type < r->term)
    {
        raftMessage* m = createRaftMessage();
        m->to = msg->from;
        m->type = MessageAppResp;
        sendMsg(r, m);
        return true;
    }

    switch(msg->type)
    {
        case MessageHup:
        {
            if (r->state != NodeStateLeader)
            {
                EntriesResult res = slice(r->raftlog, r->raftlog->applied + 1, r->raftlog->commited + 1, UINT64_MAX);
                if (res.err != StorageOk)
                {
                    serverLog(LL_WARNING, "unexpected error getting unapplied entries,err:%d", res.err);
                    assert(false);
                }
                int num = numOfPendingConf(res.entries);
                listRelease(res.entries);
                if (num > 0)
                {
                    serverLog(LL_WARNING, "%d cannot campaign at term %d since there are still %d pending configuration changes to apply", r->id, r->term, num);
                    return true;
                }
                serverLog(LL_NOTICE, "%d is starting a new election at term %d", r->id, r->term);
                campaign(r);
            }
            else
            {
                serverLog(LL_NOTICE, "%d ignoring MsgHup because already leader", r->id);
            }
            break;
        }
        case MessageVote:
        {
            raftMessage* m = createRaftMessage();
            m->to = msg->from;
            m->term = msg->term;
            m->type = MessageVoteResp;
            if((r->voteFor == 0 || r->voteFor == msg->from) && isUpToDate(r->raftlog, msg->index, msg->logTerm))
            {               
                sendMsg(r, m);
                r->electionElapsed = 0;
                r->voteFor = msg->from;
            }else
            {
                m->reject = true;;
                sendMsg(r, m);
            }
            break;
        }
        default:
            r->step(r, msg);
    }
    return true;
}

void handleAppendEntries(raft* r, raftMessage* msg)
{
    if(msg->index < r->raftlog->commited)
    {
        raftMessage* m = createRaftMessage();
        m->to = msg->from;
        m->index = r->raftlog->commited;
        m->type = MessageAppResp;
        sendMsg(r, m);
        return;
    }
    uint64_t last_index = maybeAppendEntries(r->raftlog, msg->logTerm, msg->index, msg->commited, msg->entries);
    if(last_index != UINT64_MAX)
    {
        raftMessage* m = createRaftMessage();
        m->to = msg->from;
        m->index = last_index;
        m->type = MessageAppResp;
        sendMsg(r, m);
    }else{
        raftMessage* m = createRaftMessage();
        m->to = msg->from;
        m->index = msg->index;
        m->type = MessageAppResp;
        m->reject = true;
        m->lastMatchIndex = lastIndex(r->raftlog);
        sendMsg(r, m);
    }

}

void handleHeartBeat(raft* r, raftMessage* msg)
{
    commitTo(r->raftlog, msg->commited);
    raftMessage* m = createRaftMessage();
    m->to = msg->from;
    m->context = sdsdup(msg->context);
    m->type = MessageHeartBeatResp;
    sendMsg(r, m);
}

void handleSnapshot(raft* r, raftMessage* msg)
{
    uint64_t index = msg->ss->metaData->lastLogIndex;
    uint64_t term = msg->ss->metaData->lastLogTerm;
    if(restoreSnapshot(r, msg->ss))
    {
        //todo
    }
}

bool restoreSnapshot(raft* r, snapshot* ss)
{
    if(ss->metaData->lastLogIndex <= r->raftlog->commited)
    {
        return false;
    }
    if(matchTerm(r->raftlog, ss->metaData->lastLogIndex, ss->metaData->lastLogTerm))
    {
        commitTo(r->raftlog, ss->metaData->lastLogIndex);
        return false;
    }
    restoreSnapshotMD(r->raftlog, ss->metaData);
    dictEmpty(r->peers, NULL);
    restoreNode(r, ss->metaData->cs->peers);
    return true;
}

void restoreNode(raft* r, list* nodes)
{
    listIter li;  
    listRewind(nodes,&li);
    uint8_t peer;
    listNode* ln;
    while ((ln = listNext(&li)) != NULL) {
        peer = (uint8_t)ln->value;
        raftNodeProgress* pr = newRaftNodeProgress(peer, r->maxInflightMsgs);
        pr->match = 0;
        pr->next = lastIndex(r->raftlog) + 1;
        dictAdd(r->peers, peer, pr);
    }    
}

bool maybeCommitRaft(raft* r)
{
    int quo = quorum(r);
    uint64_t* l = zmalloc(quo*sizeof(uint64_t));
    memset(l, 0, quo*sizeof(uint64_t));
    dictIterator* it = dictGetIterator(r->peers);
    raftNodeProgress* progress;
    dictEntry* e = dictNext(it);
    while (e != NULL) {
        raftNodeProgress* pr = dictGetVal(e);
        uint64_t match = pr->match;
        int smallest = 0;
        for(int i = 1; i < quo; i++)
        {
            if(l[smallest] > l[i])
            {
                smallest = i; 
            }
        }
        if(match > l[smallest])
        {
            l[smallest] = match;
        }
        e = dictNext(it);
    }
    int smallest = 0;
    for(int i = 1; i < quo; i++)
    {
        if(l[smallest] > l[i])
        {
            smallest = i; 
        }
    }
    maybeCommit(r->raftlog, l[smallest], r->term);    
    zfree(l);
}

bool checkQuorumActive(raft* r)
{
    int active_num = 0;
    dictIterator* it = dictGetIterator(r->peers);
    raftNodeProgress* progress;
    dictEntry* e = dictNext(it);
    while (e != NULL) {
        raftNodeProgress* pr = dictGetVal(e);
        if(pr->active)
        {
            active_num++;
        }
        e = dictNext(it);
    }   
    return active_num >= quorum(r);
}

void campaign(raft* r)
{
    becomeCandidate(r);
    int granted = pollRaft(r, r->id, true);
    if(granted == quorum(r))
    {
        becomeLeader(r);
        return;
    }
    dictIterator* it = dictGetIterator(r->peers);
    raftNodeProgress* progress;
    dictEntry* e = dictNext(it);
    while (e != NULL) {
        raftNodeProgress* pr = dictGetVal(e);
        if(pr->id == r->id)
        {
            continue;
        }
        raftMessage* msg = createRaftMessage();
        msg->term = r->term;
        msg->to = pr->id;
        msg->type = MessageVote;
        msg->index = lastIndex(r);
        msg->logTerm = lastTerm(r);
        sendMsg(r, msg);
        e = dictNext(it);
    }   

}

void broadCastAppend(raft* r)
{
    dictIterator* it = dictGetIterator(r->peers);
    raftNodeProgress* progress;
    dictEntry* e = dictNext(it);
    while (e != NULL) {
        raftNodeProgress* pr = dictGetVal(e);
        sendAppend(r, pr->id);
        e = dictNext(it);
    }   
}

void sendHeartBeat(raft* r, uint64_t to)
{
    raftNodeProgress* pr = getProgress(r, to);
    if(pr == NULL)
    {
        return;
    }
    uint64_t match = pr->match;
    uint64_t commit = match < r->raftlog->commited ? match : r->raftlog->commited;
    raftMessage* msg = createRaftMessage();
    msg->to = to;
    msg->type = MessageHeartBeat;
    msg->commited = commit;
    sendMsg(r, msg);
}

void broadcastHeartbeat(raft *r)
{
    dictIterator* it = dictGetIterator(r->peers);
    raftNodeProgress* progress;
    dictEntry* e = dictNext(it);
    while (e != NULL) {
        raftNodeProgress* pr = dictGetVal(e);
        sendHeartBeat(r, pr->id);
        e = dictNext(it);
    }     
}