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


#include "mongo/base/status.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/parsed_projection.h"
#include "mongo/db/query/query_request.h"

namespace mongo {

class OperationContext;
/*查询计划相关参考
https://blog.csdn.net/baijiwei/article/details/78170387
https://blog.csdn.net/baijiwei/article/details/78128632
https://blog.csdn.net/baijiwei/article/details/78195766
https://blog.csdn.net/joy0921/article/details/80131186
https://segmentfault.com/a/1190000015236644
https://yq.aliyun.com/articles/647563?spm=a2c4e.11155435.0.0.7cb74df3gUVck4
https://blog.csdn.net/baijiwei/article/details/78127191
https://blog.csdn.net/baijiwei/article/category/7189912
*/
//执行计划 优化器可以参考https://yq.aliyun.com/articles/647563?spm=a2c4e.11155435.0.0.7cb74df3gUVck4
//规范查询，见FindCmd::run  
class CanonicalQuery { 
public:
    /**
     * If parsing succeeds, returns a std::unique_ptr<CanonicalQuery> representing the parsed
     * query (which will never be NULL).  If parsing fails, returns an error Status.
     *
     * 'opCtx' must point to a valid OperationContext, but 'opCtx' does not need to outlive the
     * returned CanonicalQuery.
     *
     * Used for legacy find through the OP_QUERY message.
     */
    static StatusWith<std::unique_ptr<CanonicalQuery>> canonicalize(
        OperationContext* opCtx,
        const QueryMessage& qm,
        const boost::intrusive_ptr<ExpressionContext>& expCtx = nullptr,
        const ExtensionsCallback& extensionsCallback = ExtensionsCallbackNoop(),
        MatchExpressionParser::AllowedFeatureSet allowedFeatures =
            MatchExpressionParser::kDefaultSpecialFeatures);

    /**
     * If parsing succeeds, returns a std::unique_ptr<CanonicalQuery> representing the parsed
     * query (which will never be NULL).  If parsing fails, returns an error Status.
     *
     * 'opCtx' must point to a valid OperationContext, but 'opCtx' does not need to outlive the
     * returned CanonicalQuery.
     */
    static StatusWith<std::unique_ptr<CanonicalQuery>> canonicalize(
        OperationContext* opCtx,
        std::unique_ptr<QueryRequest> qr,
        const boost::intrusive_ptr<ExpressionContext>& expCtx = nullptr,
        const ExtensionsCallback& extensionsCallback = ExtensionsCallbackNoop(),
        MatchExpressionParser::AllowedFeatureSet allowedFeatures =
            MatchExpressionParser::kDefaultSpecialFeatures);

    /**
     * For testing or for internal clients to use.
     */

    /**
     * Used for creating sub-queries from an existing CanonicalQuery.
     *
     * 'root' must be an expression in baseQuery.root().
     *
     * Does not take ownership of 'root'.
     */
    static StatusWith<std::unique_ptr<CanonicalQuery>> canonicalize(OperationContext* opCtx,
                                                                    const CanonicalQuery& baseQuery,
                                                                    MatchExpression* root);

    /**
     * Returns true if "query" describes an exact-match query on _id, possibly with
     * the $isolated/$atomic modifier.
     */
    static bool isSimpleIdQuery(const BSONObj& query);

    const NamespaceString& nss() const {
        return _qr->nss();
    }
    const std::string& ns() const {
        return _qr->nss().ns();
    }

    //
    // Accessors for the query
    //query.root()->toString()输出打印，参考QueryPlanner::plan
    MatchExpression* root() const {
        return _root.get();
    }
    BSONObj getQueryObj() const {
        return _qr->getFilter();
    }
    const QueryRequest& getQueryRequest() const {
        return *_qr;
    }
    const ParsedProjection* getProj() const {
        return _proj.get();
    }
    const CollatorInterface* getCollator() const {
        return _collator.get();
    }

    /**
     * Sets this CanonicalQuery's collator, and sets the collator on this CanonicalQuery's match
     * expression tree.
     *
     * This setter can be used to override the collator that was created from the query request
     * during CanonicalQuery construction.
     */
    void setCollator(std::unique_ptr<CollatorInterface> collator);

    // Debugging
    std::string toString() const;
    std::string toStringShort() const;

    /**
     * Validates match expression, checking for certain
     * combinations of operators in match expression and
     * query options in QueryRequest.
     * Since 'root' is derived from 'filter' in QueryRequest,
     * 'filter' is not validated.
     *
     * TODO: Move this to query_validator.cpp
     */
    static Status isValid(MatchExpression* root, const QueryRequest& parsed);

    /**
     * Traverses expression tree post-order.
     * Sorts children at each non-leaf node by (MatchType, path(), children, number of children)
     */
    static void sortTree(MatchExpression* tree);

    /**
     * Returns a count of 'type' nodes in expression tree.
     */
    static size_t countNodes(const MatchExpression* root, MatchExpression::MatchType type);

    /**
     * Returns true if this canonical query may have converted extensions such as $where and $text
     * into no-ops during parsing. This will be the case if it allowed $where and $text in parsing,
     * but parsed using an ExtensionsCallbackNoop. This does not guarantee that a $where or $text
     * existed in the query.
     *
     * Queries with a no-op extension context are special because they can be parsed and planned,
     * but they cannot be executed.
     */
    bool canHaveNoopMatchNodes() const {
        return _canHaveNoopMatchNodes;
    }

    /**
     * Returns true if the query this CanonicalQuery was parsed from included a $isolated/$atomic
     * operator.
     */
    bool isIsolated() const {
        return _isIsolated;
    }

private:
    // You must go through canonicalize to create a CanonicalQuery.
    CanonicalQuery() {}

    Status init(OperationContext* opCtx,
                std::unique_ptr<QueryRequest> qr,
                bool canHaveNoopMatchNodes,
                std::unique_ptr<MatchExpression> root,
                std::unique_ptr<CollatorInterface> collator);

    std::unique_ptr<QueryRequest> _qr; //查询中的相关详细信息及操作符等都记录在这里面

    //CanonicalQuery._root  QuerySolutionNode.filter  一个filter对应一个MatchExpression
    //所有的QuerySolutionNode(代表CanonicalQuery._root树中的一个节点，对应一个MatchExpression)组成一颗树
    //参考https://blog.csdn.net/baijiwei/article/details/78170387

    // _root points into _qr->getFilter()
    //参考https://blog.csdn.net/baijiwei/article/details/78170387
    //MatchExpression是将filter算子里每个逻辑运算转换成各个类型的表达式(GT,ET,LT,AND,OR...)，构成一个表达式tree结构，顶层root是一个AndMatchExpression，如果含有AND、OR、NOR，tree的深度就+1. 这个表达式tree会用做以后过滤记录。
    //赋值参考CanonicalQuery::canonicalize->CanonicalQuery::init，该root tree通过MatchExpressionParser::parse生成
    std::unique_ptr<MatchExpression> _root;   //CanonicalQuery._root
    

    //projection : 选择输出指定的fields，类比SQL的select  例如db.news.find( {}, { id: 1, title: 1 } )只输出id和title字段，第一个参数为查询条件，空代表查询所有
    //projection赋值见ParsedProjection::make
    std::unique_ptr<ParsedProjection> _proj; 

    //collator是用户可以自定义的除了ByteComparator(逐字节比较排序)之外的比较方法，比如内置的中文比较。collator需要和filter里的逻辑表达式相关联(比如$gt大于运算)。
    //_collator赋值见ParsedProjection::make
    std::unique_ptr<CollatorInterface> _collator;

    bool _canHaveNoopMatchNodes = false;

    //针对其他线程的并发写操作，$isolate保证了提交前其他线程无法修改对应的文档。
    //针对其他线程的读操作，$isolate保证了其他线程读取不到未提交的数据。
    //但是$isolate有验证的性能问题，因为这种情况下线程持有锁的时间较长，严重的影响mongo的并发性�
    bool _isIsolated;
};

}  // namespace mongo
