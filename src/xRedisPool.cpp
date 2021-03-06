/*
 * ----------------------------------------------------------------------------
 * Copyright (c) 2013-2021, xSky <guozhw at gmail dot com>
 * All rights reserved.
 * Distributed under GPL license.
 * ----------------------------------------------------------------------------
 */

#include "xRedisPool.h"
#include "hiredis.h"
#include "xRedisLog.h"
#include <time.h>

namespace xrc {

RedisPool::RedisPool()
{
    mRedisCacheList = NULL;
    mTypeSize = 0;
    srand((unsigned)time(NULL));
}

RedisPool::~RedisPool() { }

bool RedisPool::Init(uint32_t typesize)
{
    mTypeSize = typesize;
    if (mTypeSize > MAX_REDIS_CACHE_TYPE) {
        return false;
    }

    mRedisCacheList = new RedisCache[mTypeSize];
    return mRedisCacheList != NULL;
}

bool RedisPool::setHashBase(uint32_t cachetype, uint32_t hashbase)
{
    if ((hashbase > MAX_REDIS_DB_HASHBASE) || (cachetype > mTypeSize - 1)) {
        return false;
    }
    bool bRet = mRedisCacheList[cachetype].InitDB(cachetype, hashbase);
    return bRet;
}

uint32_t RedisPool::getHashBase(uint32_t cachetype)
{
    if ((cachetype > mTypeSize) || (cachetype > MAX_REDIS_CACHE_TYPE)) {
        return 0;
    }
    return mRedisCacheList[cachetype].GetHashBase();
}

void RedisPool::Keepalive()
{
    for (uint32_t i = 0; i < mTypeSize; i++) {
        if (mRedisCacheList[i].GetHashBase() > 0) {
            mRedisCacheList[i].KeepAlive();
        }
    }
}

bool RedisPool::CheckReply(const redisReply* reply)
{
    if (NULL == reply) {
        return false;
    }

    switch (reply->type) {
    case REDIS_REPLY_STRING: {
        return true;
    }
    case REDIS_REPLY_ARRAY: {
        return true;
    }
    case REDIS_REPLY_INTEGER: {
        return true;
    }
    case REDIS_REPLY_NIL: {
        return false;
    }
    case REDIS_REPLY_STATUS: {
        return true;
    }
    case REDIS_REPLY_ERROR: {
        return false;
    }
    default: {
        return false;
    }
    }

    return false;
}

void RedisPool::FreeReply(const redisReply* reply)
{
    if (NULL != reply) {
        freeReplyObject((void*)reply);
    }
}

bool RedisPool::ConnectRedisDB(uint32_t cachetype, uint32_t dbindex,
    const std::string& host, uint32_t port,
    const std::string& passwd, uint32_t poolsize,
    uint32_t timeout, uint32_t role)
{
    if ((0 == host.length()) || (cachetype > MAX_REDIS_CACHE_TYPE) || (dbindex > MAX_REDIS_DB_HASHBASE)
        || (cachetype > mTypeSize - 1) || (role > SLAVE) || (poolsize > MAX_REDIS_CONN_POOLSIZE)) {
        xredis_error("cachetype:%u dbindex:%u host:%s port:%u passwd:%s poolsize:%u timeout:%u role:%u",
            cachetype, dbindex, host.c_str(), port, passwd.c_str(), poolsize, timeout, role);
        return false;
    }

    return mRedisCacheList[cachetype].ConnectRedisDB(
        cachetype, dbindex, host, port, passwd, poolsize, timeout, role);
}

void RedisPool::Release()
{
    for (uint32_t i = 0; i < mTypeSize; i++) {
        if (mRedisCacheList[i].GetHashBase() > 0) {
            mRedisCacheList[i].ClosePool();
        }
    }
    delete[] mRedisCacheList;
}

RedisConn* RedisPool::GetConnection(uint32_t cachetype, uint32_t dbindex,
    uint32_t ioType)
{
    RedisConn* pRedisConn = NULL;

    if ((cachetype > mTypeSize) || (dbindex > mRedisCacheList[cachetype].GetHashBase()) || (ioType > SLAVE)) {
        return NULL;
    }

    RedisCache* pRedisCache = &mRedisCacheList[cachetype];
    pRedisConn = pRedisCache->GetConn(dbindex, ioType);

    return pRedisConn;
}

void RedisPool::FreeConnection(RedisConn* redisconn)
{
    if (NULL != redisconn) {
        mRedisCacheList[redisconn->GetType()].FreeConn(redisconn);
    }
}

RedisConn::RedisConn()
{
    mCtx = NULL;
    mPort = 0;
    mTimeout = 0;
    mPoolsize = 0;
    mType = 0;
    mDbindex = 0;
    mConnStatus = false;
}

RedisConn::~RedisConn() { }

redisContext* RedisConn::ConnectWithTimeout()
{
    struct timeval timeoutVal;
    timeoutVal.tv_sec = mTimeout;
    timeoutVal.tv_usec = 0;

    redisContext* ctx = NULL;
    ctx = redisConnectWithTimeout(mHost.c_str(), mPort, timeoutVal);
    if (NULL == ctx || ctx->err) {
        xredis_error("failed dbindex:%u host:%s port:%u passwd:%s poolsize:%u timeout:%u role:%u",
            mDbindex, mHost.c_str(), mPort, mPass.c_str(), mPoolsize, mTimeout, mRole);
        if (NULL != ctx) {
            redisFree(ctx);
            ctx = NULL;
        } else {
        }
    }

    return ctx;
}

bool RedisConn::auth()
{
    bool bRet = false;
    if (0 == mPass.length()) {
        bRet = true;
    } else {
        redisReply* reply = static_cast<redisReply*>(redisCommand(mCtx, "AUTH %s", mPass.c_str()));
        if ((NULL == reply) || (strcasecmp(reply->str, "OK") != 0)) {
            xredis_error("failed dbindex:%u host:%s port:%u passwd:%s poolsize:%u timeout:%u role:%u",
                mDbindex, mHost.c_str(), mPort, mPass.c_str(), mPoolsize, mTimeout, mRole);
            bRet = false;
        } else {
            xredis_info("success dbindex:%u host:%s port:%u passwd:%s poolsize:%u timeout:%u role:%u",
                mDbindex, mHost.c_str(), mPort, mPass.c_str(), mPoolsize, mTimeout, mRole);
            bRet = true;
        }
        freeReplyObject(reply);
    }

    return bRet;
}

bool RedisConn::RedisConnect()
{
    bool bRet = false;
    if (NULL != mCtx) {
        redisFree(mCtx);
        mCtx = NULL;
    }

    mCtx = ConnectWithTimeout();
    if (NULL == mCtx) {
        bRet = false;
    } else {
        bRet = auth();
        mConnStatus = bRet;
    }

    const char* result = bRet ? "success" : "failed";
    xredis_info("%s dbindex:%u host:%s port:%u passwd:%s poolsize:%u timeout:%u role:%u",
        result, mDbindex, mHost.c_str(), mPort, mPass.c_str(), mPoolsize, mTimeout, mRole);

    return bRet;
}

bool RedisConn::RedisReConnect()
{
    if (NULL == mCtx) {
        return false;
    }

    bool bRet = false;
    redisContext* tmp_ctx = ConnectWithTimeout();
    if (NULL == tmp_ctx) {
        xredis_warn("failed dbindex:%u host:%s port:%u passwd:%s poolsize:%u timeout:%u role:%u",
            mDbindex, mHost.c_str(), mPort, mPass.c_str(), mPoolsize, mTimeout, mRole);
        bRet = false;
    } else {
        redisFree(mCtx);
        mCtx = tmp_ctx;
        bRet = auth();
    }

    mConnStatus = bRet;
    return bRet;
}

bool RedisConn::Ping()
{
    redisReply* reply = static_cast<redisReply*>(redisCommand(mCtx, "PING"));
    bool bRet = (NULL != reply) && (reply->str) && (strcasecmp(reply->str, "PONG") == 0);
    mConnStatus = bRet;
    if (bRet) {
        freeReplyObject(reply);
    }
    if (bRet) {
        xredis_debug("OK dbindex:%u host:%s port:%u passwd:%s poolsize:%u timeout:%u role:%u",
            mDbindex, mHost.c_str(), mPort, mPass.c_str(), mPoolsize, mTimeout, mRole);
    } else {
        xredis_warn("failed dbindex:%u host:%s port:%u passwd:%s poolsize:%u timeout:%u role:%u",
            mDbindex, mHost.c_str(), mPort, mPass.c_str(), mPoolsize, mTimeout, mRole);
    }

    return bRet;
}

void RedisConn::Init(uint32_t cachetype, uint32_t dbindex,
    const std::string& host, uint32_t port,
    const std::string& pass, uint32_t poolsize,
    uint32_t timeout, uint32_t role, uint32_t slaveidx)
{
    mType = cachetype;
    mDbindex = dbindex;
    mHost = host;
    mPort = port;
    mPass = pass;
    mPoolsize = poolsize;
    mTimeout = timeout;
    mRole = role;
    mSlaveIdx = slaveidx;
}

RedisDBSlice::RedisDBSlice()
{
    mType = 0;
    mDbindex = 0;
    mStatus = 0;
    mHaveSlave = false;
}

RedisDBSlice::~RedisDBSlice() { }

void RedisDBSlice::Init(uint32_t cachetype, uint32_t dbindex)
{
    mType = cachetype;
    mDbindex = dbindex;
}

bool RedisDBSlice::ConnectRedisNodes(uint32_t cachetype, uint32_t dbindex,
    const std::string& host, uint32_t port,
    const std::string& passwd,
    uint32_t poolsize, uint32_t timeout,
    int32_t role)
{
    bool bRet = false;
    if ((host.empty()) || (cachetype > MAX_REDIS_CACHE_TYPE) || (dbindex > MAX_REDIS_DB_HASHBASE) || (poolsize > MAX_REDIS_CONN_POOLSIZE)) {
        return false;
    }

    try {
        if (MASTER == role) {
            XLOCK(mSliceConn.MasterLock);
            for (uint32_t i = 0; i < poolsize; ++i) {
                RedisConn* pRedisconn = new RedisConn;
                if (NULL == pRedisconn) {
                    continue;
                }
                xredis_info("cachetype:%u dbindex:%u host:%s port:%u passwd:%s poolsize:%u timeout:%u role:%u",
                    cachetype, dbindex, host.c_str(), port, passwd.c_str(), poolsize, timeout, role);

                pRedisconn->Init(cachetype, dbindex, host, port, passwd, poolsize,
                    timeout, role, 0);
                if (pRedisconn->RedisConnect()) {

                    mSliceConn.RedisMasterConn.push_back(pRedisconn);
                    mStatus = REDISDB_WORKING;
                    bRet = true;
                } else {
                    delete pRedisconn;
                }
            }

        } else if (SLAVE == role) {
            XLOCK(mSliceConn.SlaveLock);
            RedisConnPool* pSlaveNode = new RedisConnPool;
            int32_t slave_idx = mSliceConn.RedisSlaveConn.size();
            for (uint32_t i = 0; i < poolsize; ++i) {
                RedisConn* pRedisconn = new RedisConn;
                if (NULL == pRedisconn) {
                    continue;
                }

                pRedisconn->Init(cachetype, dbindex, host, port, passwd, poolsize,
                    timeout, role, slave_idx);
                if (pRedisconn->RedisConnect()) {
                    pSlaveNode->push_back(pRedisconn);
                    bRet = true;
                } else {
                    delete pRedisconn;
                }
            }
            mSliceConn.RedisSlaveConn.push_back(pSlaveNode);
            mHaveSlave = true;
        } else {
            bRet = false;
        }

    } catch (...) {
        return false;
    }

    return bRet;
}

RedisConn* RedisDBSlice::GetMasterConn()
{
    RedisConn* pRedisConn = NULL;
    XLOCK(mSliceConn.MasterLock);
    if (!mSliceConn.RedisMasterConn.empty()) {
        pRedisConn = mSliceConn.RedisMasterConn.front();
        mSliceConn.RedisMasterConn.pop_front();
    } else {
        mStatus = REDISDB_DEAD;
    }
    return pRedisConn;
}

RedisConn* RedisDBSlice::GetSlaveConn()
{
    RedisConn* pRedisConn = NULL;
    XLOCK(mSliceConn.SlaveLock);
    if (!mSliceConn.RedisSlaveConn.empty()) {
        size_t slave_cnt = mSliceConn.RedisSlaveConn.size();
        uint32_t idx = rand() % slave_cnt;
        RedisConnPool* pSlave = mSliceConn.RedisSlaveConn[idx];
        pRedisConn = pSlave->front();
        pSlave->pop_front();
        // if (idx != pRedisConn->GetSlaveIdx()) {
        //}
    }
    return pRedisConn;
}

RedisConn* RedisDBSlice::GetConn(int32_t ioRole)
{
    RedisConn* pRedisConn = NULL;
    if (!mHaveSlave) {
        ioRole = MASTER;
    }
    if (MASTER == ioRole) {
        pRedisConn = GetMasterConn();
    } else if (SLAVE == ioRole) {
        pRedisConn = GetSlaveConn();
    } else {
        pRedisConn = NULL;
    }

    return pRedisConn;
}

void RedisDBSlice::FreeConn(RedisConn* redisconn)
{
    if (NULL != redisconn) {
        uint32_t role = redisconn->GetRole();
        if (MASTER == role) {
            XLOCK(mSliceConn.MasterLock);
            mSliceConn.RedisMasterConn.push_back(redisconn);
        } else if (SLAVE == role) {
            XLOCK(mSliceConn.SlaveLock);
            RedisConnPool* pSlave = mSliceConn.RedisSlaveConn[redisconn->GetSlaveIdx()];
            pSlave->push_back(redisconn);
        } else {
        }
    }
}

void RedisDBSlice::CloseConnPool()
{
    {
        XLOCK(mSliceConn.MasterLock);
        RedisConnIter master_iter = mSliceConn.RedisMasterConn.begin();
        for (; master_iter != mSliceConn.RedisMasterConn.end(); ++master_iter) {
            redisFree((*master_iter)->getCtx());
            delete *master_iter;
        }
    }

    {
        XLOCK(mSliceConn.SlaveLock);
        RedisSlaveGroupIter slave_iter = mSliceConn.RedisSlaveConn.begin();
        for (; slave_iter != mSliceConn.RedisSlaveConn.end(); ++slave_iter) {
            RedisConnPool* pConnPool = (*slave_iter);
            RedisConnIter iter = pConnPool->begin();
            for (; iter != pConnPool->end(); ++iter) {
                redisFree((*iter)->getCtx());
                delete *iter;
            }
            delete pConnPool;
        }
    }

    mStatus = REDISDB_DEAD;
}

void RedisDBSlice::ConnPoolPing()
{
    xredis_info("type:%u index:%u  mStatus:%u", mType, mDbindex, mStatus);

    {
        XLOCK(mSliceConn.MasterLock);
        RedisConnIter master_iter = mSliceConn.RedisMasterConn.begin();
        for (; master_iter != mSliceConn.RedisMasterConn.end(); ++master_iter) {
            bool bRet = (*master_iter)->Ping();
            if (!bRet) {
                bool bRet = (*master_iter)->RedisReConnect();
                if (bRet) {
                    xredis_info("RedisReConnect success type:%u index:%u  mStatus:%u", mType, mDbindex, mStatus);
                }
            } else {
            }
        }
    }

    {
        XLOCK(mSliceConn.SlaveLock);
        RedisSlaveGroupIter slave_iter = mSliceConn.RedisSlaveConn.begin();
        for (; slave_iter != mSliceConn.RedisSlaveConn.end(); ++slave_iter) {
            RedisConnPool* pConnPool = (*slave_iter);
            RedisConnIter iter = pConnPool->begin();
            for (; iter != pConnPool->end(); ++iter) {
                bool bRet = (*iter)->Ping();
                if (!bRet) {
                    bool bRet = (*iter)->RedisReConnect();
                    if (bRet) {
                        xredis_info("RedisReConnect success type:%u index:%u  mStatus:%u", mType, mDbindex, mStatus);
                    }
                } else {
                }
            }
        }
    }
}

uint32_t RedisDBSlice::GetStatus() const { return mStatus; }

RedisCache::RedisCache()
{
    mCachetype = 0;
    mHashbase = 0;
    mDBList = NULL;
}

RedisCache::~RedisCache() { }

bool RedisCache::InitDB(uint32_t cachetype, uint32_t hashbase)
{
    mCachetype = cachetype;
    mHashbase = hashbase;
    if (NULL == mDBList) {
        mDBList = new RedisDBSlice[hashbase];
    }

    return true;
}

bool RedisCache::ConnectRedisDB(uint32_t cachetype, uint32_t dbindex,
    const std::string& host, uint32_t port,
    const std::string& passwd, uint32_t poolsize,
    uint32_t timeout, uint32_t role)
{
    mDBList[dbindex].Init(cachetype, dbindex);
    return mDBList[dbindex].ConnectRedisNodes(cachetype, dbindex, host, port,
        passwd, poolsize, timeout, role);
}

void RedisCache::ClosePool()
{
    for (uint32_t i = 0; i < mHashbase; i++) {
        mDBList[i].CloseConnPool();
    }
    delete[] mDBList;
    mDBList = NULL;
}

void RedisCache::KeepAlive()
{
    for (uint32_t i = 0; i < mHashbase; i++) {
        mDBList[i].ConnPoolPing();
    }
}

uint32_t RedisCache::GetDBStatus(uint32_t dbindex)
{
    RedisDBSlice* pdbSclice = &mDBList[dbindex];
    if (NULL == pdbSclice) {
        return REDISDB_UNCONN;
    }
    return pdbSclice->GetStatus();
}

void RedisCache::FreeConn(RedisConn* redisconn)
{
    return mDBList[redisconn->getdbindex()].FreeConn(redisconn);
}

RedisConn* RedisCache::GetConn(uint32_t dbindex, uint32_t ioRole)
{
    return mDBList[dbindex].GetConn(ioRole);
}

uint32_t RedisCache::GetHashBase() const { return mHashbase; }

} // namespace xrc

