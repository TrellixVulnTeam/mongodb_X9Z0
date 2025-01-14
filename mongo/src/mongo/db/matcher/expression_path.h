/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/db/matcher/expression.h"

namespace mongo {

/**
 * A PathMatchExpression is an expression that acts on a field path with syntax
 * like "path.to.something": {$operator: ...}. Many such expressions are leaves in
 * the AST, such as $gt, $mod, $exists, and so on. But expressions that are not
 * leaves, such as $_internalSchemaObjectMatch, may also match against a field
 * path.
 */
/*
Expression_array.h (src\mongo\db\matcher):class ArrayMatchingMatchExpression : public PathMatchExpression {
Expression_internal_schema_object_match.h (src\mongo\db\matcher\schema):class InternalSchemaObjectMatchExpression final : public PathMatchExpression {
Expression_leaf.h (src\mongo\db\matcher):class LeafMatchExpression : public PathMatchExpression {
*/
//ComparisonMatchExpression继承LeafMatchExpression，LeafMatchExpression继承PathMatchExpression
//上图的几个类继承该类
class PathMatchExpression : public MatchExpression {
public:
    PathMatchExpression(MatchType matchType) : MatchExpression(matchType) {}

    virtual ~PathMatchExpression() {}

    /**
     * Returns whether or not this expression should match against each element of an array (in
     * addition to the array as a whole).
     *
     * For example, returns true if a path match expression on "f" should match against 1, 2, and
     * [1, 2] for document {f: [1, 2]}. Returns false if this expression should only match against
     * [1, 2].
     */
	// 被 LeafMatchExpression 继承的返回都是 true
    virtual bool shouldExpandLeafArray() const = 0;

	// MatchExpression::matchesBSON, matchesBSONElement -> matches -> 
	// 子类的matchesSingleElement
    bool matches(const MatchableDocument* doc, MatchDetails* details = nullptr) const final {
        MatchableDocument::IteratorHolder cursor(doc, &_elementPath);
        while (cursor->more()) {
            ElementIterator::Context e = cursor->next();
			// 用各个子类的实现的 matchesSingleElement， 例如 ComparisonMatchExpression::matchesSingleElement 
            if (!matchesSingleElement(e.element(), details)) {
                continue;
            }
            if (details && details->needRecord() && !e.arrayOffset().eoo()) {
                details->setElemMatchKey(e.arrayOffset().fieldName());
            }
            return true;
        }
        return false;
    }

    const StringData path() const final {
        return _path;
    }

    //ComparisonMatchExpression::init调用
    //path也就是{ aa : 0.99 }或者{ aa: { $lt: "0.99" } }中的 aa 
    Status setPath(StringData path) {//PathMatchExpression::setPath
        _path = path;
        //ElementPath::init
		// 对 filed 按 "." 切分，如 "a.b" -> "a" "b"
        auto status = _elementPath.init(_path);
        if (!status.isOK()) {
            return status;
        }

        _elementPath.setTraverseLeafArray(shouldExpandLeafArray());
        return Status::OK();
    }

    /**
     * Finds an applicable rename from 'renameList' (if one exists) and applies it to the expression
     * path. Each pair in 'renameList' specifies a path prefix that should be renamed (as the first
     * element) and the path components that should replace the renamed prefix (as the second
     * element).
     */
    void applyRename(const StringMap<std::string>& renameList) {
        FieldRef pathFieldRef(_path);

        int renamesFound = 0;
        for (auto rename : renameList) {
            if (rename.first == _path) {
                _rewrittenPath = rename.second;
                invariantOK(setPath(_rewrittenPath));

                ++renamesFound;
            }

            FieldRef prefixToRename(rename.first);
            if (prefixToRename.isPrefixOf(pathFieldRef)) {
                // Get the 'pathTail' by chopping off the 'prefixToRename' path components from the
                // beginning of the 'pathFieldRef' path.
                auto pathTail = pathFieldRef.dottedSubstring(prefixToRename.numParts(),
                                                             pathFieldRef.numParts());
                // Replace the chopped off components with the component names resulting from the
                // rename.
                _rewrittenPath = str::stream() << rename.second << "." << pathTail.toString();
                invariantOK(setPath(_rewrittenPath));

                ++renamesFound;
            }
        }

        // There should never be multiple applicable renames.
        invariant(renamesFound <= 1);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final {
        if (!_path.empty()) {
            deps->fields.insert(_path.toString());
        }
    }

private:
    //上面的setPath赋值
    StringData _path;
    //上面的setPath赋值
    ElementPath _elementPath;

    // We use this when we rewrite the value in '_path' and we need a backing store for the
    // rewritten string.
    std::string _rewrittenPath;
};
}  // namespace mongo
