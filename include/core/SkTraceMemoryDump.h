/*
 * Copyright 2015 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef SkTraceMemoryDump_DEFINED
#define SkTraceMemoryDump_DEFINED

#include "include/core/SkTypes.h"
#include "include/core/SkString.h"

class SkDiscardableMemory;

/**
 * Interface for memory tracing.
 * This interface is meant to be passed as argument to the memory dump methods of Skia objects.
 * The implementation of this interface is provided by the embedder.
 */
class SK_API SkTraceMemoryDump {
public:
    /**
     * Enum to specify the level of the requested details for the dump from the Skia objects.
     */
    enum LevelOfDetail {
        // Dump only the minimal details to get the total memory usage (Usually just the totals).
        kLight_LevelOfDetail,

        // Dump the detailed breakdown of the objects in the caches.
        kObjectsBreakdowns_LevelOfDetail
    };

    /**
     *  Appends a new memory dump (i.e. a row) to the trace memory infrastructure.
     *  If dumpName does not exist yet, a new one is created. Otherwise, a new column is appended to
     *  the previously created dump.
     *  Arguments:
     *    dumpName: an absolute, slash-separated, name for the item being dumped
     *        e.g., "skia/CacheX/EntryY".
     *    valueName: a string indicating the name of the column.
     *        e.g., "size", "active_size", "number_of_objects".
     *        This string is supposed to be long lived and is NOT copied.
     *    units: a string indicating the units for the value.
     *        e.g., "bytes", "objects".
     *        This string is supposed to be long lived and is NOT copied.
     *    value: the actual value being dumped.
     */
    virtual void dumpNumericValue(const char* dumpName,
                                  const char* valueName,
                                  const char* units,
                                  uint64_t value) = 0;

    virtual void dumpStringValue(const char* /*dumpName*/,
                                 const char* /*valueName*/,
                                 const char* /*value*/) { }

    /**
     * Sets the memory backing for an existing dump.
     * backingType and backingObjectId are used by the embedder to associate the memory dumped via
     * dumpNumericValue with the corresponding dump that backs the memory.
     */
    virtual void setMemoryBacking(const char* dumpName,
                                  const char* backingType,
                                  const char* backingObjectId) = 0;

    /**
     * Specialization for memory backed by discardable memory.
     */
    virtual void setDiscardableMemoryBacking(
        const char* dumpName,
        const SkDiscardableMemory& discardableMemoryObject) = 0;

    /**
     * Returns the type of details requested in the dump. The granularity of the dump is supposed to
     * match the LevelOfDetail argument. The level of detail must not affect the total size
     * reported, but only granularity of the child entries.
     */
    virtual LevelOfDetail getRequestedDetails() const = 0;

    /**
     * Returns true if we should dump wrapped objects. Wrapped objects come from outside Skia, and
     * may be independently tracked there.
     */
    virtual bool shouldDumpWrappedObjects() const { return true; }

protected:
    virtual ~SkTraceMemoryDump() { }
};

class TestSkTraceMemoryDump : public SkTraceMemoryDump {
public:
    TestSkTraceMemoryDump(bool shouldDumpWrappedObjects)
            : fShouldDumpWrappedObjects(shouldDumpWrappedObjects) {}
    ~TestSkTraceMemoryDump() override { }

    void dumpNumericValue(const char* dumpName, const char* valueName, const char* units,
                          uint64_t value) override {
        ++fNumDumpedObjects;
        fDumpedObjectsSize += value;
        printf("memory dump print dumpNumericValue %s, %s, %s, %llu , %zu \n",dumpName, valueName, units, value, fDumpedObjectsSize);
    }
    void dumpStringValue(const char* dumpName, const char* valueName, const char* value) override {
        //printf("memory dump print dumpStringValue %s, %s, %s \n",dumpName, valueName, value);
    }
    void setMemoryBacking(const char* dumpName, const char* backingType,
                          const char* backingObjectId) override {
        //printf("memory dump print setMemoryBacking \n");
    }
    void setDiscardableMemoryBacking(
        const char* dumpName,
        const SkDiscardableMemory& discardableMemoryObject) override {
        //printf("memory dump print setDiscardableMemoryBacking \n");
    }
    LevelOfDetail getRequestedDetails() const override {
        return SkTraceMemoryDump::kObjectsBreakdowns_LevelOfDetail;
    }
    bool shouldDumpWrappedObjects() const override { return fShouldDumpWrappedObjects; }

    size_t numDumpedObjects() const { return fNumDumpedObjects; }
    size_t dumpedObjectsSize() const { return fDumpedObjectsSize; }

private:
    bool fShouldDumpWrappedObjects;
    size_t fNumDumpedObjects = 0;
    size_t fDumpedObjectsSize = 0;
};


#endif
