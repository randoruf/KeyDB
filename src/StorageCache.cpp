#include "server.h"

uint64_t hashPassthrough(const void *hash) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(hash));
}

int hashCompare(void *, const void *key1, const void *key2) {
    auto diff = (reinterpret_cast<uintptr_t>(key1) - reinterpret_cast<uintptr_t>(key2));
    return !diff;
}

dictType dbStorageCacheType = {
    hashPassthrough,            /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    hashCompare,                /* key compare */
    NULL,                       /* key destructor */
    NULL                        /* val destructor */
};

StorageCache::StorageCache(IStorage *storage, bool fCache)
        : m_spstorage(storage)
{
    if (fCache)
        m_pdict = dictCreate(&dbStorageCacheType, nullptr);
}

StorageCache::~StorageCache()
{
    if (m_pdict != nullptr)
        dictRelease(m_pdict);
}

void StorageCache::clear()
{
    std::unique_lock<fastlock> ul(m_lock);
    if (m_pdict != nullptr)
        dictEmpty(m_pdict, nullptr);
    m_spstorage->clear();
    m_collisionCount = 0;
}

void StorageCache::cacheKey(sds key)
{
    if (m_pdict == nullptr)
        return;
    uintptr_t hash = dictSdsHash(key);
    if (dictAdd(m_pdict, reinterpret_cast<void*>(hash), (void*)1) != DICT_OK) {
        dictEntry *de = dictFind(m_pdict, reinterpret_cast<void*>(hash));
        serverAssert(de != nullptr);
        de->v.s64++;
        m_collisionCount++;
    }
}

void StorageCache::cacheKey(const char *rgch, size_t cch)
{
    if (m_pdict == nullptr)
        return;
    uintptr_t hash = dictGenHashFunction(rgch, (int)cch);
    if (dictAdd(m_pdict, reinterpret_cast<void*>(hash), (void*)1) != DICT_OK) {
        dictEntry *de = dictFind(m_pdict, reinterpret_cast<void*>(hash));
        serverAssert(de != nullptr);
        de->v.s64++;
        m_collisionCount++;
    }
}

bool StorageCache::erase(sds key)
{
    bool result = m_spstorage->erase(key, sdslen(key));
    std::unique_lock<fastlock> ul(m_lock);
    if (result && m_pdict != nullptr)
    {
        uint64_t hash = dictSdsHash(key);
        dictEntry *de = dictFind(m_pdict, reinterpret_cast<void*>(hash));
        serverAssert(de != nullptr);
        de->v.s64--;
        serverAssert(de->v.s64 >= 0);
        if (de->v.s64 == 0) {
            dictDelete(m_pdict, reinterpret_cast<void*>(hash));
        } else {
            m_collisionCount--;
        }
    }
    return result;
}

void StorageCache::insert(sds key, const void *data, size_t cbdata, bool fOverwrite)
{
    std::unique_lock<fastlock> ul(m_lock);
    if (!fOverwrite && m_pdict != nullptr)
    {
        cacheKey(key);
    }
    ul.unlock();
    m_spstorage->insert(key, sdslen(key), (void*)data, cbdata, fOverwrite);
}

void StorageCache::bulkInsert(sds *rgkeys, sds *rgvals, size_t celem)
{
    std::unique_lock<fastlock> ul(m_lock);
    bulkInsertsInProgress++;
    if (m_pdict != nullptr) {
        for (size_t ielem = 0; ielem < celem; ++ielem) {
            cacheKey(rgkeys[ielem]);
        }
    }
    ul.unlock();
    m_spstorage->bulkInsert(rgkeys, rgvals, celem);
    bulkInsertsInProgress--;
}

const StorageCache *StorageCache::clone()
{
    std::unique_lock<fastlock> ul(m_lock);
    // Clones never clone the cache
    StorageCache *cacheNew = new StorageCache(const_cast<IStorage*>(m_spstorage->clone()), false /*fCache*/);
    return cacheNew;
}

void StorageCache::retrieve(sds key, IStorage::callbackSingle fn) const
{
    std::unique_lock<fastlock> ul(m_lock);
    if (m_pdict != nullptr)
    {
        uint64_t hash = dictSdsHash(key);
        dictEntry *de = dictFind(m_pdict, reinterpret_cast<void*>(hash));
        
        if (de == nullptr)
            return; // Not found
    }
    ul.unlock();
    m_spstorage->retrieve(key, sdslen(key), fn);
}

size_t StorageCache::count() const
{
    std::unique_lock<fastlock> ul(m_lock, std::defer_lock);
    bool fLocked = ul.try_lock();
    size_t count = m_spstorage->count();
    if (m_pdict != nullptr && fLocked) {
        serverAssert(bulkInsertsInProgress.load(std::memory_order_seq_cst) || count == (dictSize(m_pdict) + m_collisionCount));
    }
    return count;
}

void StorageCache::beginWriteBatch() { 
    serverAssert(GlobalLocksAcquired());    // Otherwise we deadlock
    m_spstorage->beginWriteBatch(); 
}

void StorageCache::emergencyFreeCache() {
    dict *d = m_pdict;
    m_pdict = nullptr;
    if (d != nullptr) {
        g_pserver->asyncworkqueue->AddWorkFunction([d]{
            dictRelease(d);
        });
    }
}