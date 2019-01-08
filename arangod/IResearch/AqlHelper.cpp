/// DISCLAIMER
///
/// Copyright 2017 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#include "AqlHelper.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/Expression.h"
#include "Aql/ExpressionContext.h"
#include "Aql/Function.h"
#include "Aql/Variable.h"
#include "Basics/fasthash.h"
#include "IResearchCommon.h"
#include "IResearchDocument.h"
#include "Logger/LogMacros.h"
#include "Misc.h"

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

namespace {

arangodb::aql::AstNodeType const CmpMap[]{
    arangodb::aql::NODE_TYPE_OPERATOR_BINARY_EQ,  // NODE_TYPE_OPERATOR_BINARY_EQ: 3 == a <==> a == 3
    arangodb::aql::NODE_TYPE_OPERATOR_BINARY_NE,  // NODE_TYPE_OPERATOR_BINARY_NE: 3 != a <==> a != 3
    arangodb::aql::NODE_TYPE_OPERATOR_BINARY_GT,  // NODE_TYPE_OPERATOR_BINARY_LT: 3 < a  <==> a > 3
    arangodb::aql::NODE_TYPE_OPERATOR_BINARY_GE,  // NODE_TYPE_OPERATOR_BINARY_LE: 3 <= a <==> a >= 3
    arangodb::aql::NODE_TYPE_OPERATOR_BINARY_LT,  // NODE_TYPE_OPERATOR_BINARY_GT: 3 > a  <==> a < 3
    arangodb::aql::NODE_TYPE_OPERATOR_BINARY_LE  // NODE_TYPE_OPERATOR_BINARY_GE: 3 >= a <==> a <= 3
};

}

namespace arangodb {
namespace iresearch {

bool equalTo(aql::AstNode const* lhs, aql::AstNode const* rhs) {
  if (lhs == rhs) {
    return true;
  }

  if ((!lhs && rhs) || (lhs && !rhs)) {
    return false;
  }

  if (lhs->type != rhs->type) {
    return false;
  }

  size_t const n = lhs->numMembers();

  if (n != rhs->numMembers()) {
    return false;
  }

  switch (lhs->type) {
    case aql::NODE_TYPE_VARIABLE: {
      return lhs->getData() == rhs->getData();
    }

    case aql::NODE_TYPE_ATTRIBUTE_ACCESS: {
      return attributeAccessEqual(lhs, rhs, nullptr);
    }

    case aql::NODE_TYPE_VALUE:
    case aql::NODE_TYPE_OBJECT: {
      return 0 == aql::CompareAstNodes(lhs, rhs, true);
    }

    case aql::NODE_TYPE_ARRAY: {
      for (size_t i = 0; i < n; ++i) {
        if (!equalTo(lhs->getMemberUnchecked(i), rhs->getMemberUnchecked(i))) {
          return false;
        }
      }
      return true;
    }

    case aql::NODE_TYPE_REFERENCE: {
      return lhs->getData() == rhs->getData();
    }

    case aql::NODE_TYPE_FCALL: {
      if (lhs->getData() != rhs->getData()) {
        // different function
        return false;
      }

      for (size_t i = 0; i < n; ++i) {
        if (!equalTo(lhs->getMemberUnchecked(i), rhs->getMemberUnchecked(i))) {
          return false;
        }
      }

      return true;
    }

    case aql::NODE_TYPE_FCALL_USER: {
      irs::string_ref lhsName, rhsName;
      iresearch::parseValue(lhsName, *lhs);
      iresearch::parseValue(rhsName, *rhs);

      if (lhsName != rhsName) {
        return false;
      }

      for (size_t i = 0; i < n; ++i) {
        if (!equalTo(lhs->getMemberUnchecked(i), rhs->getMemberUnchecked(i))) {
          return false;
        }
      }

      return true;
    }

    case aql::NODE_TYPE_RANGE: {
      for (size_t i = 0; i < n; ++i) {
        if (!equalTo(lhs->getMemberUnchecked(i), rhs->getMemberUnchecked(i))) {
          return false;
        }
      }

      return true;
    }

    default: { return lhs == rhs; }
  }
}

size_t hash(aql::AstNode const* node, size_t hash /*= 0*/) noexcept {
  if (!node) {
    return hash;
  }

  auto hashMembers = [](aql::AstNode const& node, size_t& hash) {
    for (size_t i = 0, n = node.numMembers(); i < n; ++i) {
      auto sub = node.getMemberUnchecked(i);

      if (sub) {
        hash = iresearch::hash(sub, hash);
      }
    }

    return hash;
  };

  switch (node->type) {
    case aql::NODE_TYPE_VARIABLE: {
      hash = fasthash64(static_cast<const void*>("variable"), 8, hash);
      return fasthash64(node->getData(), sizeof(void*), hash);
    }

    case aql::NODE_TYPE_ATTRIBUTE_ACCESS: {
      hash = fasthash64(static_cast<const void*>("attribute access"), 16, hash);
      hash = aql::AstNode(node->value).hashValue(hash);
      return hashMembers(*node, hash);
    }

    case aql::NODE_TYPE_INDEXED_ACCESS: {
      hash = fasthash64(static_cast<const void*>("indexed access"), 14, hash);
      hash = aql::AstNode(node->value).hashValue(hash);
      return hashMembers(*node, hash);
    }

    case aql::NODE_TYPE_EXPANSION: {
      hash = fasthash64(static_cast<const void*>("*"), 1, hash);
      return hashMembers(*node, hash);
    }

    case aql::NODE_TYPE_VALUE: {
      hash = fasthash64(static_cast<const void*>("value"), 5, hash);
      switch (node->value.type) {
        case aql::VALUE_TYPE_NULL:
          return fasthash64(static_cast<const void*>("null"), 4, hash);
        case aql::VALUE_TYPE_BOOL:
          if (node->value.value._bool) {
            return fasthash64(static_cast<const void*>("true"), 4, hash);
          }
          return fasthash64(static_cast<const void*>("false"), 5, hash);
        case aql::VALUE_TYPE_INT:
          return fasthash64(static_cast<const void*>(&node->value.value._int),
                            sizeof(node->value.value._int), hash);
        case aql::VALUE_TYPE_DOUBLE:
          return fasthash64(static_cast<const void*>(&node->value.value._double),
                            sizeof(node->value.value._double), hash);
        case aql::VALUE_TYPE_STRING:
          return fasthash64(static_cast<const void*>(node->getStringValue()),
                            node->getStringLength(), hash);
      }
    }

    case aql::NODE_TYPE_ARRAY: {
      hash = fasthash64(static_cast<const void*>("array"), 5, hash);
      return hashMembers(*node, hash);
    }

    case aql::NODE_TYPE_OBJECT: {
      hash = fasthash64(static_cast<const void*>("object"), 6, hash);
      for (size_t i = 0, n = node->numMembers(); i < n; ++i) {
        auto sub = node->getMemberUnchecked(i);

        if (sub) {
          hash = fasthash64(static_cast<const void*>(sub->getStringValue()),
                            sub->getStringLength(), hash);
          TRI_ASSERT(sub->numMembers() > 0);
          hash = iresearch::hash(sub->getMemberUnchecked(0), hash);
        }
      }
      return hash;
    }

    case aql::NODE_TYPE_REFERENCE: {
      hash = fasthash64(static_cast<const void*>("reference"), 9, hash);
      return fasthash64(node->getData(), sizeof(void*), hash);
    }

    case aql::NODE_TYPE_FCALL: {
      // convert name to lower case
      auto* fn = static_cast<aql::Function*>(node->getData());

      hash = fasthash64(static_cast<const void*>("fcall"), 5, hash);
      hash = fasthash64(node->getData(), sizeof(void*), hash);
      hash = fasthash64(fn->name.c_str(), fn->name.size(), hash);
      return hashMembers(*node, hash);
    }

    case aql::NODE_TYPE_FCALL_USER: {
      hash = fasthash64(static_cast<const void*>("fcalluser"), 9, hash);
      hash = fasthash64(static_cast<const void*>(node->getStringValue()),
                        node->getStringLength(), hash);
      return hashMembers(*node, hash);
    }

    case aql::NODE_TYPE_RANGE: {
      hash = fasthash64(static_cast<const void*>("range"), 5, hash);
      return hashMembers(*node, hash);
    }

    default: {
      return fasthash64(static_cast<void const*>(&node), sizeof(&node), hash);
    }
  }
}

irs::string_ref getFuncName(aql::AstNode const& node) {
  irs::string_ref fname;

  switch (node.type) {
    case aql::NODE_TYPE_FCALL:
      fname = reinterpret_cast<aql::Function const*>(node.getData())->name;
      break;

    case aql::NODE_TYPE_FCALL_USER:
      parseValue(fname, node);
      break;

    default:
      TRI_ASSERT(false);
  }

  return fname;
}

// ----------------------------------------------------------------------------
// --SECTION--                                    AqlValueTraits implementation
// ----------------------------------------------------------------------------

arangodb::aql::AstNode const ScopedAqlValue::INVALID_NODE(arangodb::aql::NODE_TYPE_ROOT);

// ----------------------------------------------------------------------------
// --SECTION--                                    ScopedAqlValue implementation
// ----------------------------------------------------------------------------

bool ScopedAqlValue::execute(arangodb::iresearch::QueryContext const& ctx) {
  if (_executed && _node->isDeterministic()) {
    // constant expression, nothing to do
    return true;
  }

  if (!ctx.plan) {  // || !ctx.ctx) {
    // can't execute expression without `ExecutionPlan`
    return false;
  }

  TRI_ASSERT(ctx.ctx);  // FIXME remove, uncomment condition

  if (!ctx.ast) {
    // can't execute expression without `AST` and `ExpressionContext`
    return false;
  }

  // don't really understand why we need `ExecutionPlan` and `Ast` here
  arangodb::aql::Expression expr(ctx.plan, ctx.ast,
                                 const_cast<arangodb::aql::AstNode*>(_node));

  destroy();

  try {
    _value = expr.execute(ctx.trx, ctx.ctx, _destroy);
  } catch (arangodb::basics::Exception const& e) {
    // can't execute expression
    LOG_TOPIC(WARN, arangodb::iresearch::TOPIC) << e.message();
    return false;
  } catch (...) {
    // can't execute expression
    return false;
  }

  _type = AqlValueTraits::type(_value);
  _executed = true;
  return true;
}

bool normalizeCmpNode(arangodb::aql::AstNode const& in,
                      arangodb::aql::Variable const& ref, NormalizedCmpNode& out) {
  static_assert(adjacencyChecker<arangodb::aql::AstNodeType>::checkAdjacency<
                    arangodb::aql::NODE_TYPE_OPERATOR_BINARY_GE, arangodb::aql::NODE_TYPE_OPERATOR_BINARY_GT,
                    arangodb::aql::NODE_TYPE_OPERATOR_BINARY_LE, arangodb::aql::NODE_TYPE_OPERATOR_BINARY_LT,
                    arangodb::aql::NODE_TYPE_OPERATOR_BINARY_NE, arangodb::aql::NODE_TYPE_OPERATOR_BINARY_EQ>(),
                "Values are not adjacent");

  if (!in.isDeterministic()) {
    // unable normalize nondeterministic node
    return false;
  }

  auto cmp = in.type;

  if (cmp < arangodb::aql::NODE_TYPE_OPERATOR_BINARY_EQ ||
      cmp > arangodb::aql::NODE_TYPE_OPERATOR_BINARY_GE || in.numMembers() != 2) {
    // wrong `in` type
    return false;
  }

  auto const* attribute = in.getMemberUnchecked(0);
  TRI_ASSERT(attribute);
  auto const* value = in.getMemberUnchecked(1);
  TRI_ASSERT(value);

  if (!arangodb::iresearch::checkAttributeAccess(attribute, ref)) {
    if (!arangodb::iresearch::checkAttributeAccess(value, ref)) {
      // no suitable attribute access node found
      return false;
    }

    std::swap(attribute, value);
    cmp = CmpMap[cmp - arangodb::aql::NODE_TYPE_OPERATOR_BINARY_EQ];
  }

  if (arangodb::iresearch::findReference(*value, ref)) {
    // value contains referenced variable
    return false;
  }

  out.attribute = attribute;
  out.value = value;
  out.cmp = cmp;

  return true;
}

bool attributeAccessEqual(arangodb::aql::AstNode const* lhs,
                          arangodb::aql::AstNode const* rhs, QueryContext const* ctx) {
  struct NodeValue {
    enum class Type {
      INVALID = 0,
      EXPANSION,  // [*]
      ACCESS,     // [<offset>] | [<string>] | .
      VALUE       // REFERENCE | VALUE
    };

    bool read(arangodb::aql::AstNode const* node, QueryContext const* ctx) noexcept {
      this->strVal = irs::string_ref::NIL;
      this->iVal = 0;
      this->type = Type::INVALID;
      this->root = nullptr;

      if (!node) {
        return false;
      }

      auto const n = node->numMembers();
      auto const type = node->type;

      if (n >= 2 && arangodb::aql::NODE_TYPE_EXPANSION == type) {  // [*]
        auto* itr = node->getMemberUnchecked(0);
        auto* ref = node->getMemberUnchecked(1);

        if (itr && itr->numMembers() == 2) {
          auto* var = itr->getMemberUnchecked(0);
          auto* root = itr->getMemberUnchecked(1);

          if (ref && arangodb::aql::NODE_TYPE_ITERATOR == itr->type &&
              arangodb::aql::NODE_TYPE_REFERENCE == ref->type && root && var &&
              arangodb::aql::NODE_TYPE_VARIABLE == var->type) {
            this->type = Type::EXPANSION;
            this->root = root;
            return true;
          }
        }

      } else if (n == 2 && arangodb::aql::NODE_TYPE_INDEXED_ACCESS == type) {  // [<something>]
        auto* root = node->getMemberUnchecked(0);
        auto* offset = node->getMemberUnchecked(1);

        if (root && offset) {
          aqlValue.reset(*offset);

          if (!ctx) {
            // can't evaluate expression at compile time
            return true;
          }

          if (!aqlValue.execute(*ctx)) {
            // failed to execute expression
            return false;
          }

          switch (aqlValue.type()) {
            case arangodb::iresearch::SCOPED_VALUE_TYPE_DOUBLE:
              this->iVal = aqlValue.getInt64();
              this->type = Type::ACCESS;
              this->root = root;
              return true;
            case arangodb::iresearch::SCOPED_VALUE_TYPE_STRING:
              if (!aqlValue.getString(this->strVal)) {
                // failed to parse value as string
                return false;
              }
              this->type = Type::ACCESS;
              this->root = root;
              return true;
            default:
              break;
          }
        }

      } else if (n == 1 && arangodb::aql::NODE_TYPE_ATTRIBUTE_ACCESS == type) {
        auto* root = node->getMemberUnchecked(0);

        if (root && arangodb::aql::VALUE_TYPE_STRING == node->value.type) {
          this->strVal = getStringRef(*node);
          this->type = Type::ACCESS;
          this->root = root;
          return true;
        }

      } else if (!n) {  // end of attribute path (base case)

        if (arangodb::aql::NODE_TYPE_REFERENCE == type) {
          this->iVal = reinterpret_cast<int64_t>(node->value.value._data);
          this->type = Type::VALUE;
          this->root = node;
          return false;  // end of path
        } else if (arangodb::aql::VALUE_TYPE_STRING == node->value.type) {
          this->strVal = getStringRef(*node);
          this->type = Type::VALUE;
          this->root = node;
          return false;  // end of path
        }
      }

      return false;  // invalid input
    }

    bool operator==(const NodeValue& rhs) const noexcept {
      return type == rhs.type && strVal == rhs.strVal && iVal == rhs.iVal;
    }

    bool operator!=(const NodeValue& rhs) const noexcept {
      return !(*this == rhs);
    }

    arangodb::iresearch::ScopedAqlValue aqlValue;
    irs::string_ref strVal;
    int64_t iVal;
    Type type{Type::INVALID};
    arangodb::aql::AstNode const* root;
  } lhsValue, rhsValue;

  while (lhsValue.read(lhs, ctx) & rhsValue.read(rhs, ctx)) {
    if (lhsValue != rhsValue) {
      return false;
    }

    lhs = lhsValue.root;
    rhs = rhsValue.root;
  }

  return lhsValue.type != NodeValue::Type::INVALID &&
         rhsValue.type != NodeValue::Type::INVALID && rhsValue == lhsValue;
}

bool nameFromAttributeAccess(std::string& name, arangodb::aql::AstNode const& node,
                             QueryContext const& ctx) {
  struct {
    bool attributeAccess(arangodb::aql::AstNode const& node) {
      irs::string_ref strValue;

      if (!parseValue(strValue, node)) {
        // wrong type
        return false;
      }

      append(strValue);
      return true;
    }

    bool expansion(arangodb::aql::AstNode const&) const {
      return false;  // do not support [*]
    }

    bool indexAccess(arangodb::aql::AstNode const& node) {
      value_.reset(node);

      if (!value_.execute(*ctx_)) {
        // failed to evalue value
        return false;
      }

      switch (value_.type()) {
        case arangodb::iresearch::SCOPED_VALUE_TYPE_DOUBLE:
          append(value_.getInt64());
          return true;
        case arangodb::iresearch::SCOPED_VALUE_TYPE_STRING: {
          irs::string_ref strValue;

          if (!value_.getString(strValue)) {
            // unable to parse value as string
            return false;
          }

          append(strValue);
          return true;
        }
        default:
          return false;
      }
    }

    void append(irs::string_ref const& value) {
      if (!str_->empty()) {
        (*str_) += NESTING_LEVEL_DELIMITER;
      }
      str_->append(value.c_str(), value.size());
    }

    void append(int64_t value) {
      (*str_) += NESTING_LIST_OFFSET_PREFIX;
      auto const written = sprintf(buf_, "%" PRIu64, value);
      str_->append(buf_, written);
      (*str_) += NESTING_LIST_OFFSET_SUFFIX;
    }

    ScopedAqlValue value_;
    std::string* str_;
    QueryContext const* ctx_;
    char buf_[21];  // enough to hold all numbers up to 64-bits
  } builder;

  name.clear();
  builder.str_ = &name;
  builder.ctx_ = &ctx;

  arangodb::aql::AstNode const* head = nullptr;
  return visitAttributeAccess(head, &node, builder) && head &&
         arangodb::aql::NODE_TYPE_REFERENCE == head->type;
}

arangodb::aql::AstNode const* checkAttributeAccess(arangodb::aql::AstNode const* node,
                                                   arangodb::aql::Variable const& ref) noexcept {
  struct {
    bool attributeAccess(arangodb::aql::AstNode const&) const { return true; }

    bool indexAccess(arangodb::aql::AstNode const&) const { return true; }

    bool expansion(arangodb::aql::AstNode const&) const {
      return false;  // do not support [*]
    }
  } checker;

  arangodb::aql::AstNode const* head = nullptr;

  return node && arangodb::aql::NODE_TYPE_REFERENCE != node->type  // do not allow root node to be REFERENCE
                 && visitAttributeAccess(head, node, checker) && head &&
                 arangodb::aql::NODE_TYPE_REFERENCE == head->type &&
                 reinterpret_cast<void const*>(&ref) == head->getData()  // same variable
             ? node
             : nullptr;
}

}  // namespace iresearch
}  // namespace arangodb

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------
