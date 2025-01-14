/**
*    Copyright (C) 2013 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#pragma once

#include <boost/optional.hpp>
#include <string>

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/bson/ordering.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/kv/kv_prefix.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class CollatorInterface;
class CollectionCatalogEntry;
class CollectionInfoCache;
class HeadManager;
class IndexAccessMethod;
class IndexDescriptor;
class MatchExpression;
class OperationContext;

//IndexCatalogImpl::_setupInMemoryStructures中构造使用
//IndexCatalogImpl(IndexCatalogEntryContainer)._entries(索引数组信息) 成员为该类型，一个索引对应一个IndexCatalogEntry，参考IndexCatalogImpl::_setupInMemoryStructures

//IndexCatalogImpl::_setupInMemoryStructures中构造使用
class IndexCatalogEntryImpl : public IndexCatalogEntry::Impl {
    MONGO_DISALLOW_COPYING(IndexCatalogEntryImpl);

public:
    explicit IndexCatalogEntryImpl(
        IndexCatalogEntry* this_,
        OperationContext* opCtx,
        StringData ns,
        CollectionCatalogEntry* collection,           // not owned
        std::unique_ptr<IndexDescriptor> descriptor,  // ownership passes to me
        CollectionInfoCache* infoCache);              // not owned, optional

    ~IndexCatalogEntryImpl() final;

    const std::string& ns() const final {
        return _ns;
    }

    void init(std::unique_ptr<IndexAccessMethod> accessMethod) final;

    IndexDescriptor* descriptor() final {
        return _descriptor.get();
    }
    const IndexDescriptor* descriptor() const final {
        return _descriptor.get();
    }

    IndexAccessMethod* accessMethod() final {
        return _accessMethod.get();
    }
    const IndexAccessMethod* accessMethod() const final {
        return _accessMethod.get();
    }

    const Ordering& ordering() const final {
        return _ordering;
    }

    //基于age列创建大于25岁的部分索引 db.persons.createIndex({country:1},{partialFilterExpression: {age: {$gt:25}}})
    const MatchExpression* getFilterExpression() const final {
        return _filterExpression.get();
    }

    const CollatorInterface* getCollator() const final {
        return _collator.get();
    }

    /// ---------------------

    const RecordId& head(OperationContext* opCtx) const final;

    void setHead(OperationContext* opCtx, RecordId newHead) final;

    void setIsReady(bool newIsReady) final;

    HeadManager* headManager() const final {
        return _headManager.get();
    }

    // --

    /**
     * Returns true if this index is multikey, and returns false otherwise.
     */
    bool isMultikey() const final;

    /**
     * Returns the path components that cause this index to be multikey if this index supports
     * path-level multikey tracking, and returns an empty vector if path-level multikey tracking
     * isn't supported.
     *
     * If this index supports path-level multikey tracking but isn't multikey, then this function
     * returns a vector with size equal to the number of elements in the index key pattern where
     * each element in the vector is an empty set.
     */
    MultikeyPaths getMultikeyPaths(OperationContext* opCtx) const final;

    /**
     * Sets this index to be multikey. Information regarding which newly detected path components
     * cause this index to be multikey can also be specified.
     *
     * If this index doesn't support path-level multikey tracking, then 'multikeyPaths' is ignored.
     *
     * If this index supports path-level multikey tracking, then 'multikeyPaths' must be a vector
     * with size equal to the number of elements in the index key pattern. Additionally, at least
     * one path component of the indexed fields must cause this index to be multikey.
     */
    void setMultikey(OperationContext* opCtx, const MultikeyPaths& multikeyPaths) final;

    // if this ready is ready for queries
    bool isReady(OperationContext* opCtx) const final;

    KVPrefix getPrefix() const final {
        return _prefix;
    }

    /**
     * If return value is not boost::none, reads with majority read concern using an older snapshot
     * must treat this index as unfinished.
     */
    boost::optional<Timestamp> getMinimumVisibleSnapshot() final {
        return _minVisibleSnapshot;
    }

    void setMinimumVisibleSnapshot(Timestamp name) final {
        _minVisibleSnapshot = name;
    }

private:
    class SetMultikeyChange;
    class SetHeadChange;

    bool _catalogIsReady(OperationContext* opCtx) const;
    RecordId _catalogHead(OperationContext* opCtx) const;

    /**
     * Retrieves the multikey information associated with this index from '_collection',
     *
     * See CollectionCatalogEntry::isIndexMultikey() for more details.
     */
    bool _catalogIsMultikey(OperationContext* opCtx, MultikeyPaths* multikeyPaths) const;

    KVPrefix _catalogGetPrefix(OperationContext* opCtx) const;

    // -----

    std::string _ns;
   

    //对应KVCollectionCatalogEntry
    CollectionCatalogEntry* _collection;  // not owned here
    //索引描述信息全部存到这里,对应索引  
    std::unique_ptr<IndexDescriptor> _descriptor;  // owned here

    //CollectionInfoCacheImpl类型
    CollectionInfoCache* _infoCache;  // not owned here

    //btree对应BtreeAccessMethod
    //IndexCatalogImpl::_setupInMemoryStructures->IndexCatalogEntryImpl::init调用
    //通过这里进行底层wt存储引擎的KV操作，参考IndexCatalogImpl::_indexFilteredRecords
    std::unique_ptr<IndexAccessMethod> _accessMethod;

    // Owned here.
    std::unique_ptr<HeadManager> _headManager;
    std::unique_ptr<CollatorInterface> _collator;
    //例如基于age列创建大于25岁的部分索引 db.persons.createIndex({country:1},{partialFilterExpression: {age: {$gt:25}}})
    std::unique_ptr<MatchExpression> _filterExpression;

    // cached stuff

    Ordering _ordering;  // TODO: this might be b-tree specific
    bool _isReady;       // cache of NamespaceDetails info
    RecordId _head;      // cache of IndexDetails

    // Set to true if this index supports path-level multikey tracking.
    // '_indexTracksPathLevelMultikeyInfo' is effectively const after IndexCatalogEntry::init() is
    // called.
    bool _indexTracksPathLevelMultikeyInfo = false;

    // Set to true if this index is multikey. '_isMultikey' serves as a cache of the information
    // stored in the NamespaceDetails or KVCatalog.
    AtomicWord<bool> _isMultikey;

    // Controls concurrent access to '_indexMultikeyPaths'. We acquire this mutex rather than the
    // RESOURCE_METADATA lock as a performance optimization so that it is cheaper to detect whether
    // there is actually any path-level multikey information to update or not.
    mutable stdx::mutex _indexMultikeyPathsMutex;

    // Non-empty only if '_indexTracksPathLevelMultikeyInfo' is true.
    //
    // If non-empty, '_indexMultikeyPaths' is a vector with size equal to the number of elements
    // in the index key pattern. Each element in the vector is an ordered set of positions (starting
    // at 0) into the corresponding indexed field that represent what prefixes of the indexed field
    // causes the index to be multikey.
    MultikeyPaths _indexMultikeyPaths;

    // KVPrefix used to differentiate between index entries in different logical indexes sharing the
    // same underlying sorted data interface.
    const KVPrefix _prefix;

    // The earliest snapshot that is allowed to read this index.
    boost::optional<Timestamp> _minVisibleSnapshot;
};
}  // namespace mongo
