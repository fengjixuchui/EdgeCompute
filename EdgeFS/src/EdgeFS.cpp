#include "EdgeFS.h"
#include "EdgeFSConst.h"
#include "./common/common.h"

EdgeFS* EdgeFS::m_pInstance = NULL;

IEdgeFS* CreateEdgeFS()
{
    EdgeFS::create();
    return EdgeFS::instance();
}

void DestroyPcdnSdk(IEdgeFS* fs)
{
    EdgeFS::release();
}

void EdgeFS::create()
{
    if (NULL == m_pInstance)
    {
        m_pInstance = new EdgeFS();
    }
}
void  EdgeFS::release()
{
    SAFE_DELETE(m_pInstance);
}

EdgeFS* EdgeFS::instance()
{
    return m_pInstance;
}

EdgeFS::EdgeFS()
{
    m_pDataMgr = new DataMgr();
    m_pIndexMgr = new IndexMgr();
    m_pBitMap = new Bitmap();
}
EdgeFS::~EdgeFS()
{
    SAFE_DELETE(m_pBitMap);
    SAFE_DELETE(m_pIndexMgr);
    SAFE_DELETE(m_pDataMgr);
}

bool EdgeFS::initFS(const SystemInfo& info)
{
    AsyncLogging::create();
    AsyncLogging::instance()->init(info.m_diskRootDir + "/" + kLogFileName);

    linfo("========================");
    lnotice("initFs, systemInfo disk %" PRIu64 " rootdir %s memory %" PRIu64, info.m_diskCapacity,
        info.m_diskRootDir.c_str(), info.m_edgeFSUsableMemory);

    // 入参数检查
    if (!initFSCheckParam(info))
    {
        return false;
    }

    bool isExistIdxFile = false;

    // 初始化数据文件和index文件
    // TODO 目前不支持大文件，后续需要用mmap64, ftruncate64等
    m_pDataMgr->initDataMgr(info.m_diskRootDir);
    m_pIndexMgr->initIndexMgr(info.m_diskRootDir, isExistIdxFile);

    // 根据收入的内存大小和磁盘大小，计算chunk个数，chunk大小，需要映射的内存
    uint32_t chunkNum = 0, chunkSize = 0, bitmapSize = 0;
    uint64_t diskSize = 0, mmapSize = 0;
    if (!initFSCalcVariable(info, chunkNum, chunkSize, diskSize, bitmapSize, mmapSize))
    {
        return false;
    }

    if (!initFSCalcPointerAddr(isExistIdxFile, chunkNum, chunkSize, diskSize, bitmapSize, mmapSize))
    {
        return false;
    }
    return true;
}

bool EdgeFS::initFSCheckParam(const SystemInfo& info)
{
    // 内存检查，最少1个meta占用的内存
    uint64_t minMemory = sizeof(EdgeFSHead) + 1 + sizeof(MetaInfo);
    if (minMemory >= info.m_edgeFSUsableMemory)
    {
        lfatal("initFS failed, out of memory, minimum %" PRIu64 " memory", minMemory);
        return false;
    }
    return true;
}

bool EdgeFS::initFSCalcVariable(const SystemInfo& info, uint32_t& chunkNum, uint32_t& chunkSize, uint64_t& diskSize,
    uint32_t& bitmapSize, uint64_t& mmapSize)
{
    chunkNum = DIV_ROUND_DOWN(info.m_edgeFSUsableMemory - sizeof(EdgeFSHead), sizeof(MetaInfo));
    chunkSize = DIV_ROUND_DOWN(info.m_diskCapacity, chunkNum);
    chunkSize = ALIGN_DOWN(chunkSize, kDiskRWAlignSize);
    Utils::limit<uint32_t>(chunkSize, kMinChunkSize, kMaxChunkSize);
    chunkNum = DIV_ROUND_DOWN(info.m_diskCapacity, chunkSize);
    bitmapSize = DIV_ROUND_UP(chunkNum, 8);
    diskSize = (uint64_t)chunkNum * (uint64_t)chunkSize;
    mmapSize = sizeof(EdgeFSHead) + bitmapSize + (uint64_t)chunkNum * sizeof(MetaInfo);

    if (0 == chunkNum ||
        0 == chunkSize ||
        0 == bitmapSize ||
        0 == diskSize ||
        0 == mmapSize ||
        diskSize > info.m_diskCapacity ||
        mmapSize > info.m_edgeFSUsableMemory)
    {
        lfatal("initFS failed, calc variable failed, chunkNum %u chunkSize %u bitmapSize %u diskSize %" PRIu64
            " mmapSize %" PRIu64, chunkNum, chunkSize, bitmapSize, diskSize, mmapSize);
        lfatal("initFS failed, calc variable failed, diskSize too large: %d diskSize %" PRIu64
            " info.m_diskCapacity %" PRIu64, diskSize >= info.m_diskCapacity, diskSize, info.m_diskCapacity);
        lfatal("initFS failed, calc variable failed, mmapSize too large: %d mmapSize %" PRIu64
            " info.m_edgeFSUsableMemory %" PRIu64, mmapSize >= info.m_edgeFSUsableMemory, mmapSize,
            info.m_edgeFSUsableMemory);
        return false;
    }

    linfo("chunkNum %d chunkSize %u bitmapSize %u mmapSize %" PRIu64 " diskSize %" PRIu64, chunkNum, chunkSize,
        bitmapSize, mmapSize, diskSize);
    linfo("EdgeFSHead size %zu MetaInfo size %zu", sizeof(EdgeFSHead), sizeof(MetaInfo));

    return true;
}

bool EdgeFS::initFSCalcPointerAddr(bool isExistsIdxFile, uint32_t chunkNum, uint32_t chunkSize, uint64_t diskSize,
    uint32_t bitmapSize, uint64_t mmapSize)
{
    if (isExistsIdxFile)
    {
        return initFSCalcPointerAddrForReloadIdxFile(chunkNum, chunkSize, diskSize, bitmapSize, mmapSize);
    }
    return initFSCalcPointerAddrForCreateIdxFile(chunkNum, chunkSize, diskSize, bitmapSize, mmapSize);
}

bool EdgeFS::initFSCalcPointerAddrForCreateIdxFile(uint32_t chunkNum, uint32_t chunkSize, uint64_t diskSize,
    uint32_t bitmapSize, uint64_t mmapSize)
{
    int fd = m_pIndexMgr->getfd();
    if (-1 == fd)
    {
        lfatal("initFS failed, fd error");
        return false;
    }

    char *ptr = (char *)mmap(0, mmapSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (MAP_FAILED == ptr)
    {
        lfatal("initFS failed, mmap failed, mmapSize %" PRIu64 " fd %d err %s", mmapSize, fd, strerror(errno));
        return false;
    }
    
    if (ftruncate(fd, mmapSize))
    {
        lfatal("initFS failed, ftruncate failed, mmapSize %" PRIu64 " err %s", mmapSize, strerror(errno));
        return false;
    }

    // 必须放在ftruncate后面
    memset(ptr, 0, mmapSize);

    // 赋值指针
    m_pFSHead = (EdgeFSHead*)ptr;
    m_pBitMap->initBitmap((char*)m_pFSHead + sizeof(EdgeFSHead), bitmapSize, chunkNum);
    m_pMetaPool = (MetaInfo*)((char*)m_pBitMap->getPtr() + bitmapSize);

    linfo("start %p fsHead %p bitmap %p metapool %p", ptr, m_pFSHead, m_pBitMap->getPtr(), m_pMetaPool);
    
    // 赋值FS头部信息
    memcpy(m_pFSHead->m_magic, kEdgeFSMagic.c_str(), kEdgeFSMagic.size());
    m_pFSHead->m_usableMemory = mmapSize;
    m_pFSHead->m_coverableDiskSize = diskSize;
    m_pFSHead->m_chunkNum = chunkNum;
    m_pFSHead->m_chunkSize = chunkSize;
    m_pFSHead->m_bitmapSize = bitmapSize;

    linfo("index file EdgeFSHead, magic %s memory %" PRIu64 " diskSize %" PRIu64 " chunkNum %u chunkSize %u"
        " bitmapSize %u",
        m_pFSHead->m_magic, m_pFSHead->m_usableMemory, m_pFSHead->m_coverableDiskSize,
        m_pFSHead->m_chunkNum, m_pFSHead->m_chunkSize, m_pFSHead->m_bitmapSize);

    return true;
}

bool EdgeFS::initFSCalcPointerAddrForReloadIdxFile(uint32_t chunkNum, uint32_t chunkSize, uint64_t diskSize,
    uint32_t bitmapSize, uint64_t mmapSize)
{
    int fd = m_pIndexMgr->getfd();
    if (-1 == fd)
    {
        lfatal("initFS failed, fd error");
        return false;
    }
    char *ptr = (char *)mmap(0, mmapSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (MAP_FAILED == ptr)
    {
        lfatal("initFS failed, mmap failed, mmapSize %" PRIu64 " fd %d err %s", mmapSize, fd, strerror(errno));
        return false;
    }

    m_pFSHead = (EdgeFSHead*)ptr;
    m_pBitMap->initBitmap((char*)m_pFSHead + sizeof(EdgeFSHead), bitmapSize, chunkNum);
    m_pMetaPool = (MetaInfo*)((char*)m_pBitMap->getPtr() + bitmapSize);

    linfo("start %p fsHead %p bitmap %p metapool %p", ptr, m_pFSHead, m_pBitMap->getPtr(), m_pMetaPool);

    // 对index文件中的FS头部信息做校验
    if (0 != memcmp(m_pFSHead->m_magic, kEdgeFSMagic.c_str(), kEdgeFSMagic.size()) ||
        m_pFSHead->m_usableMemory != mmapSize ||
        m_pFSHead->m_coverableDiskSize != diskSize ||
        m_pFSHead->m_chunkNum != chunkNum ||
        m_pFSHead->m_chunkSize != chunkSize ||
        m_pFSHead->m_bitmapSize != bitmapSize
        )
    {
        lfatal("initFS failed, index file EdgeFSHead error, magic %s %s memory %" PRIu64
            " %" PRIu64 " diskSize %" PRIu64 " %" PRIu64 " chunkNum %u %u chunkSize %u %u"
            " bitmapSize %u %u",
            m_pFSHead->m_magic, kEdgeFSMagic.c_str(), m_pFSHead->m_usableMemory, mmapSize,
            m_pFSHead->m_coverableDiskSize, diskSize, m_pFSHead->m_chunkNum, chunkNum,
            m_pFSHead->m_chunkSize, chunkSize, m_pFSHead->m_bitmapSize, bitmapSize);
        return false;
    }

    linfo("index file EdgeFSHead, magic %s memory %" PRIu64 " diskSize %" PRIu64 "chunkNum %u chunkSize %u"
        " bitmapSize %u",
        m_pFSHead->m_magic, m_pFSHead->m_usableMemory, m_pFSHead->m_coverableDiskSize,
        m_pFSHead->m_chunkNum, m_pFSHead->m_chunkSize, m_pFSHead->m_bitmapSize);

    printAllMetaInfo();

    return true;
}

void EdgeFS::unitFS()
{
    AsyncLogging::release();
}

void EdgeFS::calcWriteVariable(const MetaInfo* pTailMtInfo, uint32_t writeLen, uint32_t& firstWriteLen,
    uint32_t& needChunkNum, uint32_t& lastChunkWriteLen)
{
    if (NULL != pTailMtInfo)
    {
        if (!pTailMtInfo->m_isUsed)
        {
            // chunk块没有被使用
            firstWriteLen = writeLen >= m_pFSHead->m_chunkSize ?
                m_pFSHead->m_chunkSize : writeLen;
        }
        else
        {
            firstWriteLen = writeLen >= pTailMtInfo->m_idleLen ?
                pTailMtInfo->m_idleLen : writeLen;
        }
    }
    uint64_t remainLen = writeLen - firstWriteLen;
    needChunkNum = DIV_ROUND_UP(remainLen, m_pFSHead->m_chunkSize);
    lastChunkWriteLen = remainLen % m_pFSHead->m_chunkSize;

    linfo("pTailMtInfo %p writeLen %u firstWriteLen %u needChunkNum %u lastChunkWriteLen %u",
        pTailMtInfo, writeLen, firstWriteLen, needChunkNum, lastChunkWriteLen);
}

MetaInfo* EdgeFS::findTailMetaInfo(const MetaInfo* pHeadMtInfo, const char* sha1Val)
{
    assert(NULL != pHeadMtInfo);
    assert(NULL != sha1Val);

    const MetaInfo* pTailMtInfo = NULL;
    const MetaInfo* tmp = pHeadMtInfo;

    while (1)
    {
        if (!tmp->m_isUsed)
        {
            pTailMtInfo = tmp;
            break;
        }
        if (0 == memcmp(tmp->m_metaData.m_sha1, sha1Val, sizeof(tmp->m_metaData.m_sha1)))
        {
            pTailMtInfo = tmp;
        }
        if (kInvalidChunkid == tmp->m_nextChunkid)
        {
            break;
        }
        tmp = calcMetaInfoPtr(tmp->m_nextChunkid);
    }
    return const_cast<MetaInfo*>(pTailMtInfo);
}

uint32_t EdgeFS::calcChunkid(const MetaInfo* pMtInfo)
{
    if (NULL == pMtInfo)
    {
        return kInvalidChunkid;
    }
    return ((char*)pMtInfo - (char*)m_pMetaPool) / (uint64_t)sizeof(MetaInfo);
}

MetaInfo* EdgeFS::calcMetaInfoPtr(uint32_t chunkid)
{
    return (MetaInfo*)((char*)m_pMetaPool + (uint64_t)chunkid * (uint64_t)sizeof(MetaInfo));
}

uint64_t EdgeFS::calcOffset(uint32_t chunkid)
{
    return (uint64_t)chunkid * m_pFSHead->m_chunkSize;
}

uint32_t EdgeFS::generateHashKey(const char* sha1Val)
{
    return (*(uint32_t*)sha1Val) % m_pFSHead->m_chunkNum;
}

int64_t EdgeFS::write(const std::string& fileName, const char* buff, uint32_t len)
{
    if (NULL == buff)
    {
        return -1;
    }
    lnotice("fileName %s len %u", fileName.c_str(), len);

    char sha1Val[SHA_DIGEST_LENGTH] = { '\0' };
    ShaHelper::calcShaToHex(fileName, sha1Val);

    uint32_t hashKey = generateHashKey(sha1Val);
    MetaInfo* pHeadMtInfo = calcMetaInfoPtr(hashKey);
    MetaInfo* pTailMtInfo = findTailMetaInfo(pHeadMtInfo, sha1Val);

    linfo("hashKey %u pHeadMtInfo %p %d pTailMtInfo %p %d", hashKey, pHeadMtInfo,
        calcChunkid(pHeadMtInfo), pTailMtInfo, calcChunkid(pTailMtInfo));

    uint32_t firstWriteLen = 0;
    uint32_t needChunkNum = 0;
    std::vector<uint32_t> idleChunkids;
    uint32_t lastChunkWriteLen = 0;
    
    calcWriteVariable(pTailMtInfo, len, firstWriteLen, needChunkNum, lastChunkWriteLen);

    if (0 != needChunkNum)
    {
        if (!m_pBitMap->generateIdleChunkids(idleChunkids, needChunkNum) || idleChunkids.empty())
        {
            lwarn("no idle chunk");
            return -1;
        }
    }
    ldebug("idleChunkSize %zu", idleChunkids.size());

    uint64_t remainLen = len;
    uint64_t offset = 0;
    uint32_t chunkid = 0;
    uint64_t realWriteLen = 0;
    uint32_t chunkSize = m_pFSHead->m_chunkSize;

    do
    {
        if (0 != firstWriteLen)
        {
            chunkid = calcChunkid(pTailMtInfo);
            offset = chunkid * chunkSize;
            // 要注意pTailMtInfo使用的是共享内存的地址，成员变量默认都是0
            if (pTailMtInfo->m_isUsed)
            {
                offset += chunkSize - pTailMtInfo->m_idleLen;
            }

            linfo("first write, firstWriteLen %u offset %" PRIu64 "", firstWriteLen, offset);

            if (!m_pDataMgr->write(buff+realWriteLen, firstWriteLen, offset))
            {
                lerror("[err] write failed, writeLen %u offset %" PRIu64, len, offset);
                break;
            }
            realWriteLen += firstWriteLen;
            remainLen -= firstWriteLen;

            if (pTailMtInfo->m_isUsed)
            {
                pTailMtInfo->m_idleLen -= firstWriteLen;
            }
            else
            {
                m_pBitMap->insert(chunkid);
                pTailMtInfo->m_isUsed = true;
                memcpy(pTailMtInfo->m_metaData.m_sha1, sha1Val, sizeof(pTailMtInfo->m_metaData.m_sha1));
                pTailMtInfo->m_idleLen = chunkSize - firstWriteLen;
                pTailMtInfo->m_nextChunkid = kInvalidChunkid;
            }
            if (0 == pTailMtInfo->m_idleLen)
            {
                // 当前chunk已经下满了，赋值nextChunkid
                pTailMtInfo->m_nextChunkid = idleChunkids.empty() ? kInvalidChunkid : idleChunkids[0];
            }

            linfo("chunkid %u offset %" PRIu64 " tailMetaInfo %s", chunkid, offset, pTailMtInfo->print().c_str());
        }
        else
        {
            // 前面一个chunk刚刚写满，但是nextChunkids还未赋值
            if (!idleChunkids.empty())
            {
                pTailMtInfo->m_nextChunkid = idleChunkids[0];
            }
        }

        if (0 != needChunkNum)
        {
            for (uint32_t i = 0; i < needChunkNum; i++)
            {
                uint32_t writeLen = remainLen >=  chunkSize ? chunkSize : remainLen;
                chunkid = idleChunkids[i];
                offset = chunkid * chunkSize;
                MetaInfo* pCurrMtInfo = calcMetaInfoPtr(chunkid);

                linfo("chunkid %u offset %" PRIu64 " writeLen %u", chunkid, offset, writeLen);

                if (!m_pDataMgr->write(buff+realWriteLen, writeLen, offset))
                {
                    lerror("write failed, writeLen %u offset %" PRIu64, len, offset);
                    break;
                }
                remainLen -= writeLen;
                realWriteLen += writeLen;

                // 写入成功更新bitmap、metainfo
                m_pBitMap->insert(chunkid);
                pCurrMtInfo->m_isUsed = true;
                memcpy(pCurrMtInfo->m_metaData.m_sha1, sha1Val, sizeof(pCurrMtInfo->m_metaData.m_sha1));

                pCurrMtInfo->m_idleLen = chunkSize - firstWriteLen;

                if (i + 1 == needChunkNum)
                {
                    // 最后一个chunk
                    pCurrMtInfo->m_nextChunkid = kInvalidChunkid;
                    pCurrMtInfo->m_idleLen = chunkSize - lastChunkWriteLen;
                }
                else
                {
                    pCurrMtInfo->m_nextChunkid = idleChunkids[i+1];
                    pCurrMtInfo->m_idleLen = 0;
                }
                linfo("metaInfo %s", pCurrMtInfo->print().c_str());
            }
        }
    } while (0);

    printAllMetaInfo();

    return realWriteLen;
}

int64_t EdgeFS::read(const std::string& fileName, char* buff, uint32_t len, uint64_t offset)
{
    if (NULL == buff)
    {
        return -1;
    }
    lnotice("fileName %s len %u", fileName.c_str(), len);

    char sha1Val[SHA_DIGEST_LENGTH] = { '\0' };
    ShaHelper::calcShaToHex(fileName, sha1Val);

    uint32_t hashKey = generateHashKey(sha1Val);
    MetaInfo* pHeadMtInfo = calcMetaInfoPtr(hashKey);
    std::vector<uint32_t> writeChunkids;
    uint32_t lastChunkWriteLen = 0;
    uint64_t writeTotalLen = 0;

    generateReadChunkids(pHeadMtInfo, sha1Val, writeChunkids, writeTotalLen, lastChunkWriteLen);

    if (writeChunkids.empty())
    {
        lwarn("not found file, fileName %s", fileName.c_str());
        return -1;
    }
    if (offset > writeTotalLen)
    {
        lwarn("offset too large, offset %" PRIu64 " writeTotalLen %" PRIu64, offset, writeTotalLen);
        return -1;
    }

    linfo("writeChunkNum %zu writeTotalLen %" PRIu64 " lastChunkWriteLen %u", writeChunkids.size(), writeTotalLen,
        lastChunkWriteLen);
    linfo("write chunkids : ");
    for (uint32_t i = 0; i < writeChunkids.size(); i++)
    {
        linfo("%d ", writeChunkids[i]);
    }

    std::map<uint64_t, uint32_t> readInfo;     // offset -> len
    calcReadVariable(writeChunkids, lastChunkWriteLen, len, offset, readInfo);

    for (auto it = readInfo.begin(); it != readInfo.end(); ++it)
    {
        linfo("read chunkId %u offset %" PRIu64 " len %u", (uint32_t)(it->first/m_pFSHead->m_chunkSize), it->first,
            it->second);
    }

    uint32_t realReadLen = 0;
    for (auto it = readInfo.begin(); it != readInfo.end(); ++it)
    {
        if (!m_pDataMgr->read(buff+realReadLen, it->second, it->first))
        {
            lerror("read failed, chunkid %u offset %" PRIu64 " readLen %u", (uint32_t)(it->first/m_pFSHead->m_chunkSize),
                it->first, it->second);
            break;
        }
        realReadLen += it->second;
    }
    return realReadLen;
}

void EdgeFS::generateReadChunkids(const MetaInfo* pHeadMtInfo, const char* sha1Val,
    std::vector<uint32_t>& writeChunkids, uint64_t& writeTotalLen, uint32_t& lastChunkidWriteLen)
{
    assert(NULL != pHeadMtInfo);
    assert(NULL != sha1Val);

    if (!pHeadMtInfo->m_isUsed)
    {
        writeChunkids.clear();
        writeTotalLen = 0;
        lastChunkidWriteLen = 0;
        return ;
    }

    const MetaInfo* tmp = pHeadMtInfo;
    while (true)
    {
        if (0 == memcmp(tmp->m_metaData.m_sha1, sha1Val, sizeof(tmp->m_metaData.m_sha1)))
        {
            writeChunkids.push_back(calcChunkid(tmp));
            lastChunkidWriteLen = m_pFSHead->m_chunkSize - tmp->m_idleLen;
            writeTotalLen += lastChunkidWriteLen;
        }
        if (kInvalidChunkid == tmp->m_nextChunkid)
        {
            break;
        }
        tmp = calcMetaInfoPtr(tmp->m_nextChunkid);
    }
}

void EdgeFS::calcReadVariable(const std::vector<uint32_t>& writeChunkids, uint32_t lastChunkWriteLen,
    uint32_t readLen, uint64_t offset, std::map<uint64_t, uint32_t>& readInfo)
{
    const uint32_t chunkSize = m_pFSHead->m_chunkSize;
    const uint32_t firstReadIdx = DIV_ROUND_DOWN(offset, chunkSize);
    const uint32_t firstChunkSkipLen = offset % chunkSize;
    uint32_t firstChunkReadLen = 0;

    if (firstReadIdx + 1 == writeChunkids.size())
    {
        // 已经读取到最后一个chunk了
        firstChunkReadLen = std::min(lastChunkWriteLen - firstChunkSkipLen, readLen);
        readInfo[calcOffset(writeChunkids[firstReadIdx]) + firstChunkSkipLen] = firstChunkReadLen;
        return ;
    }
    else
    {
        firstChunkReadLen = std::min(chunkSize - firstChunkSkipLen, readLen);
        readInfo[calcOffset(writeChunkids[firstReadIdx]) + firstChunkSkipLen] = firstChunkReadLen;
    }

    uint32_t remainLen = readLen - firstChunkReadLen;
    uint32_t needChunkNum = DIV_ROUND_UP(remainLen, chunkSize);

    for (uint32_t i = 0; i < needChunkNum; i++)
    {
        uint32_t idx = firstReadIdx + 1 + i;
        if (idx >= writeChunkids.size())
        {
            break;
        }
        uint32_t chunkid = writeChunkids[idx];

        if (idx + 1 == writeChunkids.size())
        {
            // 最后一个chunk需要读的长度
            MetaInfo* pMtInfo = calcMetaInfoPtr(chunkid);
            uint32_t lastChunkReadlen = std::min(remainLen % chunkSize, chunkSize - pMtInfo->m_idleLen);
            readInfo[calcOffset(chunkid)] = lastChunkReadlen;
        }
        else
        {
            readInfo[calcOffset(chunkid)] = chunkSize;
        }
    }
}

void EdgeFS::printAllMetaInfo()
{
    linfo("start");
    for (uint32_t chunkid = 0; chunkid < m_pFSHead->m_chunkNum; chunkid++)
    {
        MetaInfo* pMtInfo = calcMetaInfoPtr(chunkid);
        if (!pMtInfo->m_isUsed)
        {
            continue;
        }
        linfo("chunkid %u idleLen %u nextChunkId %d", chunkid, pMtInfo->m_idleLen, pMtInfo->m_nextChunkid);
    }
    linfo("end");
}
