/*
 * Copyright 2017 MapD Technologies, Inc.
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

/**
 * @file        FileBuffer.cpp
 * @author      Steven Stewart <steve@map-d.com>
 * @author      Todd Mostak <todd@map-d.com>
 */

#include "DataMgr/FileMgr/FileBuffer.h"

#include <future>
#include <map>
#include <thread>

#include "DataMgr/FileMgr/FileMgr.h"
#include "Shared/File.h"
#include "Shared/checked_alloc.h"

#ifdef HAVE_DCPMM
#include "../Pmem.h"
#include "PmmPersistentBufferMgr.h"
#endif /* HAVE_DCPMM */

#define METADATA_PAGE_SIZE 4096

using namespace std;

namespace File_Namespace {
size_t FileBuffer::headerBufferOffset_ = 32;

FileBuffer::FileBuffer(FileMgr* fm,
                       const size_t pageSize,
                       const ChunkKey& chunkKey,
                       const size_t initialSize)
    : AbstractBuffer(fm->getDeviceId())
    , fm_(fm)
    , metadataPages_(METADATA_PAGE_SIZE)
    , pageSize_(pageSize)
#ifdef HAVE_DCPMM
    , pmmMem_(NULL)
    , pmmBufferDescriptor_(NULL)
#endif /* HAVE_DCPMM */
    , chunkKey_(chunkKey) {
  // Create a new FileBuffer
  CHECK(fm_);
  calcHeaderBuffer();
  pageDataSize_ = pageSize_ - reservedHeaderSize_;
  //@todo reintroduce initialSize - need to develop easy way of
  // differentiating these pre-allocated pages from "written-to" pages
  /*
  if (initalSize > 0) {
      // should expand to initialSize bytes
      size_t initialNumPages = (initalSize + pageSize_ -1) / pageSize_;
      int epoch = fm_->epoch();
      for (size_t pageNum = 0; pageNum < initialNumPages; ++pageNum) {
          Page page = addNewMultiPage(epoch);
          writeHeader(page,pageNum,epoch);
      }
  }
  */
}

FileBuffer::FileBuffer(FileMgr* fm,
                       const size_t pageSize,
                       const ChunkKey& chunkKey,
                       const SQLTypeInfo sql_type,
                       const size_t initialSize)
    : AbstractBuffer(fm->getDeviceId(), sql_type)
    , fm_(fm)
    , metadataPages_(METADATA_PAGE_SIZE)
    , pageSize_(pageSize)
#ifdef HAVE_DCPMM
    , pmmMem_(NULL)
    , pmmBufferDescriptor_(NULL)
#endif /* HAVE_DCPMM */
    , chunkKey_(chunkKey) {
  CHECK(fm_);
  calcHeaderBuffer();
  pageDataSize_ = pageSize_ - reservedHeaderSize_;
}

FileBuffer::FileBuffer(FileMgr* fm,
                       /* const size_t pageSize,*/ const ChunkKey& chunkKey,
                       const std::vector<HeaderInfo>::const_iterator& headerStartIt,
                       const std::vector<HeaderInfo>::const_iterator& headerEndIt)
    : AbstractBuffer(fm->getDeviceId())
    , fm_(fm)
    , metadataPages_(METADATA_PAGE_SIZE)
    , pageSize_(0)
#ifdef HAVE_DCPMM
    , pmmMem_(NULL)
    , pmmBufferDescriptor_(NULL)
#endif /* HAVE_DCPMM */
    , chunkKey_(chunkKey) {
  // We are being assigned an existing FileBuffer on disk

  CHECK(fm_);
  calcHeaderBuffer();
  // MultiPage multiPage(pageSize_); // why was this here?
  int lastPageId = -1;
  // Page lastMetadataPage;
  for (auto vecIt = headerStartIt; vecIt != headerEndIt; ++vecIt) {
    int curPageId = vecIt->pageId;

    // We only want to read last metadata page
    if (curPageId == -1) {  // stats page
      metadataPages_.epochs.push_back(vecIt->versionEpoch);
      metadataPages_.pageVersions.push_back(vecIt->page);
    } else {
      if (curPageId != lastPageId) {
        // protect from bad data on disk, and give diagnostics
        if (curPageId != lastPageId + 1) {
          LOG(FATAL) << "Failure reading DB file " << show_chunk(chunkKey)
                     << " Current page " << curPageId << " last page " << lastPageId
                     << " epoch " << vecIt->versionEpoch;
        }
        if (lastPageId == -1) {
          // If we are on first real page
          CHECK(metadataPages_.pageVersions.back().fileId != -1);  // was initialized
          readMetadata(metadataPages_.pageVersions.back());
          pageDataSize_ = pageSize_ - reservedHeaderSize_;
        }
        MultiPage multiPage(pageSize_);
        multiPages_.push_back(multiPage);
        lastPageId = curPageId;
      }
      multiPages_.back().epochs.push_back(vecIt->versionEpoch);
      multiPages_.back().pageVersions.push_back(vecIt->page);
    }
    if (curPageId == -1) {  // meaning there was only a metadata page
      readMetadata(metadataPages_.pageVersions.back());
      pageDataSize_ = pageSize_ - reservedHeaderSize_;
    }
  }
  // auto lastHeaderIt = std::prev(headerEndIt);
  // size_ = lastHeaderIt->chunkSize;
}

#ifdef HAVE_DCPMM
FileBuffer::FileBuffer(FileMgr* fm,
                       ChunkKey chunkKey,
                       int8_t* pmmAddr,
                       PersistentBufferDescriptor* p,
                       bool existed)
    : AbstractBuffer(fm->getDeviceId())
    , fm_(fm)
    , metadataPages_(METADATA_PAGE_SIZE)
    , pageSize_(2 * 1024 * 1024)  // default page size 2MB
    , pmmMem_(pmmAddr)
    , pmmBufferDescriptor_(p)
    , chunkKey_(chunkKey) {
  if (existed) {
    readMetadata();
  }
}
#endif /* HAVE_DCPMM */

FileBuffer::~FileBuffer() {
  // need to free pages
  // NOP
}

void FileBuffer::reserve(const size_t numBytes) {
  size_t numPagesRequested = (numBytes + pageSize_ - 1) / pageSize_;
  size_t numCurrentPages = multiPages_.size();
  int epoch = fm_->epoch();

  for (size_t pageNum = numCurrentPages; pageNum < numPagesRequested; ++pageNum) {
    Page page = addNewMultiPage(epoch);
    writeHeader(page, pageNum, epoch);
  }
}

void FileBuffer::calcHeaderBuffer() {
  // 3 * sizeof(int) is for headerSize, for pageId and versionEpoch
  // sizeof(size_t) is for chunkSize
  // reservedHeaderSize_ = (chunkKey_.size() + 3) * sizeof(int) + sizeof(size_t);
  reservedHeaderSize_ = (chunkKey_.size() + 3) * sizeof(int);
  size_t headerMod = reservedHeaderSize_ % headerBufferOffset_;
  if (headerMod > 0) {
    reservedHeaderSize_ += headerBufferOffset_ - headerMod;
  }
  // pageDataSize_ = pageSize_-reservedHeaderSize_;
}

void FileBuffer::freeMetadataPages() {
  for (auto metaPageIt = metadataPages_.pageVersions.begin();
       metaPageIt != metadataPages_.pageVersions.end();
       ++metaPageIt) {
    FileInfo* fileInfo = fm_->getFileInfoForFileId(metaPageIt->fileId);
    fileInfo->freePage(metaPageIt->pageNum);
  }
  while (metadataPages_.pageVersions.size() > 0) {
    metadataPages_.pop();
  }
}

size_t FileBuffer::freeChunkPages() {
  size_t num_pages_freed = multiPages_.size();
  for (auto multiPageIt = multiPages_.begin(); multiPageIt != multiPages_.end();
       ++multiPageIt) {
    for (auto pageIt = multiPageIt->pageVersions.begin();
         pageIt != multiPageIt->pageVersions.end();
         ++pageIt) {
      FileInfo* fileInfo = fm_->getFileInfoForFileId(pageIt->fileId);
      fileInfo->freePage(pageIt->pageNum);
    }
  }
  multiPages_.clear();
  return num_pages_freed;
}

void FileBuffer::freePages() {
  freeMetadataPages();
  freeChunkPages();
}

struct readThreadDS {
  FileMgr* t_fm;       // ptr to FileMgr
  size_t t_startPage;  // start page for the thread
  size_t t_endPage;    // last page for the thread
  int8_t* t_curPtr;    // pointer to the current location of the target for the thread
  size_t t_bytesLeft;  // number of bytes to be read in the thread
  size_t t_startPageOffset;  // offset - used for the first page of the buffer
  bool t_isFirstPage;        // true - for first page of the buffer, false - otherwise
  std::vector<MultiPage> multiPages;  // MultiPages of the FileBuffer passed to the thread
};

static size_t readForThread(FileBuffer* fileBuffer, const readThreadDS threadDS) {
  size_t startPage = threadDS.t_startPage;  // start reading at startPage, including it
  size_t endPage = threadDS.t_endPage;      // stop reading at endPage, not including it
  int8_t* curPtr = threadDS.t_curPtr;
  size_t bytesLeft = threadDS.t_bytesLeft;
  size_t totalBytesRead = 0;
  bool isFirstPage = threadDS.t_isFirstPage;

  // Traverse the logical pages
  for (size_t pageNum = startPage; pageNum < endPage; ++pageNum) {
    CHECK(threadDS.multiPages[pageNum].pageSize == fileBuffer->pageSize());
    Page page = threadDS.multiPages[pageNum].current();

    FileInfo* fileInfo = threadDS.t_fm->getFileInfoForFileId(page.fileId);
    CHECK(fileInfo);

    // Read the page into the destination (dst) buffer at its
    // current (cur) location
    size_t bytesRead = 0;
    if (isFirstPage) {
      bytesRead = fileInfo->read(
          page.pageNum * fileBuffer->pageSize() + threadDS.t_startPageOffset +
              fileBuffer->reservedHeaderSize(),
          min(fileBuffer->pageDataSize() - threadDS.t_startPageOffset, bytesLeft),
          curPtr);
      isFirstPage = false;
    } else {
      bytesRead = fileInfo->read(
          page.pageNum * fileBuffer->pageSize() + fileBuffer->reservedHeaderSize(),
          min(fileBuffer->pageDataSize(), bytesLeft),
          curPtr);
    }
    curPtr += bytesRead;
    bytesLeft -= bytesRead;
    totalBytesRead += bytesRead;
  }
  CHECK(bytesLeft == 0);

  return (totalBytesRead);
}

void FileBuffer::read(int8_t* const dst,
                      const size_t numBytes,
                      const size_t offset,
                      const MemoryLevel dstBufferType,
                      const int deviceId) {
  if (dstBufferType != CPU_LEVEL) {
    LOG(FATAL) << "Unsupported Buffer type";
  }

#ifdef HAVE_DCPMM
  if (pmmMem_) {
    // pmmMem_ is always pageSize_ aligned
    size_t unitSize = 2 * pageSize_;
    size_t numPagesToRead = (numBytes + (offset % unitSize) + unitSize - 1) / unitSize;
    if ((fm_->getNumReaderThreads() == 1) || (numPagesToRead == 1)) {
      memcpy(dst, pmmMem_ + offset, numBytes);
      return;
    }

    size_t numPagesPerThread;
    size_t numThreads = fm_->getNumReaderThreads();
    if (numPagesToRead > numThreads) {
      numPagesPerThread = numPagesToRead / numThreads;
    } else {
      numThreads = numPagesToRead;
      numPagesPerThread = 1;
    }

    int8_t* sliceDst = dst;
    int8_t* sliceSrc = pmmMem_ + offset;
    size_t sliceSize = unitSize * numPagesPerThread - (offset % unitSize);
    std::vector<std::future<size_t>> threads;
    threads.push_back(std::async(std::launch::async, [=] {
      memcpy(sliceDst, sliceSrc, sliceSize);
      return sliceSize;
    }));
    sliceDst += sliceSize;
    sliceSrc += sliceSize;
    sliceSize = unitSize * numPagesPerThread;

    for (size_t i = 0; i < numThreads - 2; i++) {
      threads.push_back(std::async(std::launch::async, [=] {
        memcpy(sliceDst, sliceSrc, sliceSize);
        return sliceSize;
      }));
      sliceDst += sliceSize;
      sliceSrc += sliceSize;
    }

    memcpy(sliceDst,
           sliceSrc,
           (numBytes + (offset % unitSize)) -
               (numPagesPerThread * unitSize * (numThreads - 1)));

    for (auto& p : threads) {
      p.wait();
    }

    return;
  }
#endif /* HAVE_DCPMM */

  // variable declarations
  size_t startPage = offset / pageDataSize_;
  size_t startPageOffset = offset % pageDataSize_;
  size_t numPagesToRead =
      (numBytes + startPageOffset + pageDataSize_ - 1) / pageDataSize_;
  /*
  if (startPage + numPagesToRead > multiPages_.size()) {
      cout << "Start page: " << startPage << endl;
      cout << "Num pages to read: " << numPagesToRead << endl;
      cout << "Num multipages: " << multiPages_.size() << endl;
      cout << "Offset: " << offset << endl;
      cout << "Num bytes: " << numBytes << endl;
  }
  */

  CHECK(startPage + numPagesToRead <= multiPages_.size());

  size_t numPagesPerThread = 0;
  size_t numBytesCurrent = numBytes;  // total number of bytes still to be read
  size_t bytesRead = 0;               // total number of bytes already being read
  size_t bytesLeftForThread = 0;      // number of bytes to be read in the thread
  size_t numExtraPages = 0;  // extra pages to be assigned one per thread as needed
  size_t numThreads = fm_->getNumReaderThreads();
  std::vector<readThreadDS>
      threadDSArr;  // array of threadDS, needed to avoid racing conditions

  if (numPagesToRead > numThreads) {
    numPagesPerThread = numPagesToRead / numThreads;
    numExtraPages = numPagesToRead - (numThreads * numPagesPerThread);
  } else {
    numThreads = numPagesToRead;
    numPagesPerThread = 1;
  }

  /* set threadDS for the first thread */
  readThreadDS threadDS;
  threadDS.t_fm = fm_;
  threadDS.t_startPage = offset / pageDataSize_;
  if (numExtraPages > 0) {
    threadDS.t_endPage = threadDS.t_startPage + numPagesPerThread + 1;
    numExtraPages--;
  } else {
    threadDS.t_endPage = threadDS.t_startPage + numPagesPerThread;
  }
  threadDS.t_curPtr = dst;
  threadDS.t_startPageOffset = offset % pageDataSize_;
  threadDS.t_isFirstPage = true;

  bytesLeftForThread = min(((threadDS.t_endPage - threadDS.t_startPage) * pageDataSize_ -
                            threadDS.t_startPageOffset),
                           numBytesCurrent);
  threadDS.t_bytesLeft = bytesLeftForThread;
  threadDS.multiPages = getMultiPage();

  if (numThreads == 1) {
    bytesRead += readForThread(this, threadDS);
  } else {
    std::vector<std::future<size_t>> threads;

    for (size_t i = 0; i < numThreads; i++) {
      threadDSArr.push_back(threadDS);
      threads.push_back(
          std::async(std::launch::async, readForThread, this, threadDSArr[i]));

      // calculate elements of threadDS
      threadDS.t_fm = fm_;
      threadDS.t_isFirstPage = false;
      threadDS.t_curPtr += bytesLeftForThread;
      threadDS.t_startPage +=
          threadDS.t_endPage -
          threadDS.t_startPage;  // based on # of pages read on previous iteration
      if (numExtraPages > 0) {
        threadDS.t_endPage = threadDS.t_startPage + numPagesPerThread + 1;
        numExtraPages--;
      } else {
        threadDS.t_endPage = threadDS.t_startPage + numPagesPerThread;
      }
      numBytesCurrent -= bytesLeftForThread;
      bytesLeftForThread = min(
          ((threadDS.t_endPage - threadDS.t_startPage) * pageDataSize_), numBytesCurrent);
      threadDS.t_bytesLeft = bytesLeftForThread;
      threadDS.multiPages = getMultiPage();
    }

    for (auto& p : threads) {
      p.wait();
    }
    for (auto& p : threads) {
      bytesRead += p.get();
    }
  }
  CHECK(bytesRead == numBytes);
}

void FileBuffer::copyPage(Page& srcPage,
                          Page& destPage,
                          const size_t numBytes,
                          const size_t offset) {
  // FILE *srcFile = fm_->files_[srcPage.fileId]->f;
  // FILE *destFile = fm_->files_[destPage.fileId]->f;
  CHECK(offset + numBytes < pageDataSize_);
  FileInfo* srcFileInfo = fm_->getFileInfoForFileId(srcPage.fileId);
  FileInfo* destFileInfo = fm_->getFileInfoForFileId(destPage.fileId);

  int8_t* buffer = reinterpret_cast<int8_t*>(checked_malloc(numBytes));
  size_t bytesRead = srcFileInfo->read(
      srcPage.pageNum * pageSize_ + offset + reservedHeaderSize_, numBytes, buffer);
  CHECK(bytesRead == numBytes);
  size_t bytesWritten = destFileInfo->write(
      destPage.pageNum * pageSize_ + offset + reservedHeaderSize_, numBytes, buffer);
  CHECK(bytesWritten == numBytes);
  free(buffer);
}

Page FileBuffer::addNewMultiPage(const int epoch) {
  Page page = fm_->requestFreePage(pageSize_, false);
  MultiPage multiPage(pageSize_);
  multiPage.epochs.push_back(epoch);
  multiPage.pageVersions.push_back(page);
  multiPages_.push_back(multiPage);
  return page;
}

void FileBuffer::writeHeader(Page& page,
                             const int pageId,
                             const int epoch,
                             const bool writeMetadata) {
  int intHeaderSize = chunkKey_.size() + 3;  // does not include chunkSize
  vector<int> header(intHeaderSize);
  // in addition to chunkkey we need size of header, pageId, version
  header[0] =
      (intHeaderSize - 1) * sizeof(int);  // don't need to include size of headerSize
                                          // value - sizeof(size_t) is for chunkSize
  std::copy(chunkKey_.begin(), chunkKey_.end(), header.begin() + 1);
  header[intHeaderSize - 2] = pageId;
  header[intHeaderSize - 1] = epoch;
  FileInfo* fileInfo = fm_->getFileInfoForFileId(page.fileId);
  size_t pageSize = writeMetadata ? METADATA_PAGE_SIZE : pageSize_;
  fileInfo->write(
      page.pageNum * pageSize, (intHeaderSize) * sizeof(int), (int8_t*)&header[0]);
}

#ifdef HAVE_DCPMM
void FileBuffer::readMetadata(void) {
  if (!pmmBufferDescriptor_) {
    size_ = 0;
    return;
  }

  // pageSize_ = pmmBufferDescriptor_->pageSize;
  pageSize_ = fm_->getPersistentBufferPageSize();
  size_ = pmmBufferDescriptor_->size;

  int version = pmmBufferDescriptor_->metaData[0];
  CHECK(version == METADATA_VERSION);  // add backward compatibility code here
  has_encoder = static_cast<bool>(pmmBufferDescriptor_->metaData[1]);
  if (has_encoder) {
    sql_type.set_type(static_cast<SQLTypes>(pmmBufferDescriptor_->metaData[2]));
    sql_type.set_subtype(static_cast<SQLTypes>(pmmBufferDescriptor_->metaData[3]));
    sql_type.set_dimension(pmmBufferDescriptor_->metaData[4]);
    sql_type.set_scale(pmmBufferDescriptor_->metaData[5]);
    sql_type.set_notnull(static_cast<bool>(pmmBufferDescriptor_->metaData[6]));
    sql_type.set_compression(
        static_cast<EncodingType>(pmmBufferDescriptor_->metaData[7]));
    sql_type.set_comp_param(pmmBufferDescriptor_->metaData[8]);
    sql_type.set_size(pmmBufferDescriptor_->metaData[9]);
    initEncoder(sql_type);
    encoder->readMetadata(pmmBufferDescriptor_->encoderMetaData);
  }
}
#endif /* HAVE_DCPMM */

void FileBuffer::readMetadata(const Page& page) {
  FILE* f = fm_->getFileForFileId(page.fileId);
  fseek(f, page.pageNum * METADATA_PAGE_SIZE + reservedHeaderSize_, SEEK_SET);
  fread((int8_t*)&pageSize_, sizeof(size_t), 1, f);
  fread((int8_t*)&size_, sizeof(size_t), 1, f);
  vector<int> typeData(NUM_METADATA);  // assumes we will encode has_encoder, bufferType,
                                       // encodingType, encodingBits all as int
  fread((int8_t*)&(typeData[0]), sizeof(int), typeData.size(), f);
  int version = typeData[0];
  CHECK(version == METADATA_VERSION);  // add backward compatibility code here
  bool has_encoder = static_cast<bool>(typeData[1]);
  if (has_encoder) {
    sql_type_.set_type(static_cast<SQLTypes>(typeData[2]));
    sql_type_.set_subtype(static_cast<SQLTypes>(typeData[3]));
    sql_type_.set_dimension(typeData[4]);
    sql_type_.set_scale(typeData[5]);
    sql_type_.set_notnull(static_cast<bool>(typeData[6]));
    sql_type_.set_compression(static_cast<EncodingType>(typeData[7]));
    sql_type_.set_comp_param(typeData[8]);
    sql_type_.set_size(typeData[9]);
    initEncoder(sql_type_);
    encoder_->readMetadata(f);
  }
}

void FileBuffer::writeMetadata(const int epoch) {
#ifdef HAVE_DCPMM
  if (pmmBufferDescriptor_) {
    pmmBufferDescriptor_->size = size_;
    size_t pSize = fm_->getPersistentBufferPageSize();
    if ((pSize * pmmBufferDescriptor_->numPages) - size_ > pSize) {
      fm_->shrinkPersistentBuffer(pmmBufferDescriptor_, pmmMem_);
    }

    pmmBufferDescriptor_->metaData[0] = METADATA_VERSION;
    pmmBufferDescriptor_->metaData[1] = static_cast<int>(has_encoder);
    if (has_encoder) {
      pmmBufferDescriptor_->metaData[2] = static_cast<int>(sql_type.get_type());
      pmmBufferDescriptor_->metaData[3] = static_cast<int>(sql_type.get_subtype());
      pmmBufferDescriptor_->metaData[4] = sql_type.get_dimension();
      pmmBufferDescriptor_->metaData[5] = sql_type.get_scale();
      pmmBufferDescriptor_->metaData[6] = static_cast<int>(sql_type.get_notnull());
      pmmBufferDescriptor_->metaData[7] = static_cast<int>(sql_type.get_compression());
      pmmBufferDescriptor_->metaData[8] = sql_type.get_comp_param();
      pmmBufferDescriptor_->metaData[9] = sql_type.get_size();
    }
    PmemPersist(&(pmmBufferDescriptor_->metaData[0]),
                sizeof(pmmBufferDescriptor_->metaData));

    if (has_encoder) {  // redundant
      encoder->writeMetadata(pmmBufferDescriptor_->encoderMetaData);
      PmemPersist(pmmBufferDescriptor_->encoderMetaData,
                  sizeof(pmmBufferDescriptor_->encoderMetaData));
    }

    pmmBufferDescriptor_->epoch = epoch;
    PmemPersist(&(pmmBufferDescriptor_->epoch), sizeof(pmmBufferDescriptor_->epoch));

    return;
  }
#endif /* HAVE_DCPMM */

  // Right now stats page is size_ (in bytes), bufferType, encodingType,
  // encodingDataType, numElements
  Page page = fm_->requestFreePage(METADATA_PAGE_SIZE, true);
  writeHeader(page, -1, epoch, true);
  FILE* f = fm_->getFileForFileId(page.fileId);
  fseek(f, page.pageNum * METADATA_PAGE_SIZE + reservedHeaderSize_, SEEK_SET);
  fwrite((int8_t*)&pageSize_, sizeof(size_t), 1, f);
  fwrite((int8_t*)&size_, sizeof(size_t), 1, f);
  vector<int> typeData(NUM_METADATA);  // assumes we will encode has_encoder, bufferType,
                                       // encodingType, encodingBits all as int
  typeData[0] = METADATA_VERSION;
  typeData[1] = static_cast<int>(hasEncoder());
  if (hasEncoder()) {
    typeData[2] = static_cast<int>(sql_type_.get_type());
    typeData[3] = static_cast<int>(sql_type_.get_subtype());
    typeData[4] = sql_type_.get_dimension();
    typeData[5] = sql_type_.get_scale();
    typeData[6] = static_cast<int>(sql_type_.get_notnull());
    typeData[7] = static_cast<int>(sql_type_.get_compression());
    typeData[8] = sql_type_.get_comp_param();
    typeData[9] = sql_type_.get_size();
  }
  fwrite((int8_t*)&(typeData[0]), sizeof(int), typeData.size(), f);
  if (hasEncoder()) {  // redundant
    encoder_->writeMetadata(f);
  }
  metadataPages_.epochs.push_back(epoch);
  metadataPages_.pageVersions.push_back(page);
}

void FileBuffer::append(int8_t* src,
                        const size_t numBytes,
                        const MemoryLevel srcBufferType,
                        const int deviceId) {
  setAppended();

#ifdef HAVE_DCPMM
  if (fm_->isPersistentMemoryPresent()) {
    if (pmmMem_) {
      if ((numBytes + size_) >
          (fm_->getPersistentBufferPageSize() * pmmBufferDescriptor_->numPages)) {
        pmmMem_ = fm_->reallocatePersistentBuffer(
            chunkKey_, pmmMem_, numBytes + size_, &pmmBufferDescriptor_);
      }
    } else {
      if (size_ != 0) {
        LOG(FATAL) << "First time to append to an empty buffer with non-zero size";
      }
      pmmMem_ = fm_->allocatePersistentBuffer(
          chunkKey_, numBytes + size_, &pmmBufferDescriptor_);
    }
    pmmBufferDescriptor_->setEpoch(fm_->epoch());
    PmemMemCpy((char*)pmmMem_ + size_, (char*)src, numBytes);
    size_ += numBytes;
    return;
  }
#endif /* HAVE_DCPMM */

  size_t startPage = size_ / pageDataSize_;
  size_t startPageOffset = size_ % pageDataSize_;
  size_t numPagesToWrite =
      (numBytes + startPageOffset + pageDataSize_ - 1) / pageDataSize_;
  size_t bytesLeft = numBytes;
  int8_t* curPtr = src;  // a pointer to the current location in dst being written to
  size_t initialNumPages = multiPages_.size();
  size_ = size_ + numBytes;
  int epoch = fm_->epoch();
  for (size_t pageNum = startPage; pageNum < startPage + numPagesToWrite; ++pageNum) {
    Page page;
    if (pageNum >= initialNumPages) {
      page = addNewMultiPage(epoch);
      writeHeader(page, pageNum, epoch);
    } else {
      // we already have a new page at current
      // epoch for this page - just grab this page
      page = multiPages_[pageNum].current();
    }
    CHECK(page.fileId >= 0);  // make sure page was initialized
    FileInfo* fileInfo = fm_->getFileInfoForFileId(page.fileId);
    size_t bytesWritten;
    if (pageNum == startPage) {
      bytesWritten = fileInfo->write(
          page.pageNum * pageSize_ + startPageOffset + reservedHeaderSize_,
          min(pageDataSize_ - startPageOffset, bytesLeft),
          curPtr);
    } else {
      bytesWritten = fileInfo->write(page.pageNum * pageSize_ + reservedHeaderSize_,
                                     min(pageDataSize_, bytesLeft),
                                     curPtr);
    }
    curPtr += bytesWritten;
    bytesLeft -= bytesWritten;
  }
  CHECK(bytesLeft == 0);
}

void FileBuffer::write(int8_t* src,
                       const size_t numBytes,
                       const size_t offset,
                       const MemoryLevel srcBufferType,
                       const int deviceId) {
  if (srcBufferType != CPU_LEVEL) {
    LOG(FATAL) << "Unsupported Buffer type";
  }

  bool tempIsAppended = false;
  setDirty();
#ifdef HAVE_DCPMM
  if (fm_->isPersistentMemoryPresent()) {
    if (pmmMem_) {
      if ((numBytes + offset) >
          (fm_->getPersistentBufferPageSize() * pmmBufferDescriptor_->numPages)) {
        pmmMem_ = fm_->reallocatePersistentBuffer(
            chunkKey_, pmmMem_, numBytes + offset, &pmmBufferDescriptor_);
      }
    } else {
      if (size_ != 0) {
        LOG(FATAL) << "First time to append to an empty buffer with non-zero size";
      }
      pmmMem_ = fm_->allocatePersistentBuffer(
          chunkKey_, numBytes + offset, &pmmBufferDescriptor_);
    }
    pmmBufferDescriptor_->setEpoch(fm_->epoch());
    PmemMemCpy((char*)pmmMem_ + offset, (char*)src, numBytes);
  }
#endif /* HAVE_DCPMM */
  if (offset < size_) {
    setUpdated();
  }
  if (offset + numBytes > size_) {
    tempIsAppended = true;  // because is_appended_ could have already been true - to
                            // avoid rewriting header
    setAppended();
    size_ = offset + numBytes;
  }

#ifdef HAVE_DCPMM
  if (fm_->isPersistentMemoryPresent()) {
    return;
  }
#endif /* HAVE_DCPMM */

  size_t startPage = offset / pageDataSize_;
  size_t startPageOffset = offset % pageDataSize_;
  size_t numPagesToWrite =
      (numBytes + startPageOffset + pageDataSize_ - 1) / pageDataSize_;
  size_t bytesLeft = numBytes;
  int8_t* curPtr = src;  // a pointer to the current location in dst being written to
  size_t initialNumPages = multiPages_.size();
  int epoch = fm_->epoch();

  if (startPage >
      initialNumPages) {  // means there is a gap we need to allocate pages for
    for (size_t pageNum = initialNumPages; pageNum < startPage; ++pageNum) {
      Page page = addNewMultiPage(epoch);
      writeHeader(page, pageNum, epoch);
    }
  }
  for (size_t pageNum = startPage; pageNum < startPage + numPagesToWrite; ++pageNum) {
    Page page;
    if (pageNum >= initialNumPages) {
      page = addNewMultiPage(epoch);
      writeHeader(page, pageNum, epoch);
    } else if (multiPages_[pageNum].epochs.back() <
               epoch) {  // need to create new page b/c this current one lags epoch and we
                         // can't overwrite it also need to copy if we are on first or
                         // last page
      Page lastPage = multiPages_[pageNum].current();
      page = fm_->requestFreePage(pageSize_, false);
      multiPages_[pageNum].epochs.push_back(epoch);
      multiPages_[pageNum].pageVersions.push_back(page);
      if (pageNum == startPage && startPageOffset > 0) {
        // copyPage takes care of header offset so don't worry
        // about it
        copyPage(lastPage, page, startPageOffset, 0);
      }
      if (pageNum == startPage + numPagesToWrite &&
          bytesLeft > 0) {  // bytesLeft should always > 0
        copyPage(lastPage,
                 page,
                 pageDataSize_ - bytesLeft,
                 bytesLeft);  // these would be empty if we're appending but we won't
                              // worry about it right now
      }
      writeHeader(page, pageNum, epoch);
    } else {
      // we already have a new page at current
      // epoch for this page - just grab this page
      page = multiPages_[pageNum].current();
    }
    CHECK(page.fileId >= 0);  // make sure page was initialized
    FileInfo* fileInfo = fm_->getFileInfoForFileId(page.fileId);
    size_t bytesWritten;
    if (pageNum == startPage) {
      bytesWritten = fileInfo->write(
          page.pageNum * pageSize_ + startPageOffset + reservedHeaderSize_,
          min(pageDataSize_ - startPageOffset, bytesLeft),
          curPtr);
    } else {
      bytesWritten = fileInfo->write(page.pageNum * pageSize_ + reservedHeaderSize_,
                                     min(pageDataSize_, bytesLeft),
                                     curPtr);
    }
    curPtr += bytesWritten;
    bytesLeft -= bytesWritten;
    if (tempIsAppended && pageNum == startPage + numPagesToWrite - 1) {  // if last page
      //@todo below can lead to undefined - we're overwriting num
      // bytes valid at checkpoint
      writeHeader(page, 0, multiPages_[0].epochs.back(), true);
    }
  }
  CHECK(bytesLeft == 0);
}

#ifdef HAVE_DCPMM
void FileBuffer::constructPersistentBuffer(int8_t* addr, PersistentBufferDescriptor* p) {
  pmmMem_ = addr;
  pmmBufferDescriptor_ = p;
}
#endif /* HAVE_DCPMM */

}  // namespace File_Namespace
