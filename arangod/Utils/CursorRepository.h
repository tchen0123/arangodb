////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_UTILS_CURSOR_REPOSITORY_H
#define ARANGOD_UTILS_CURSOR_REPOSITORY_H 1

#include "Basics/Common.h"
#include "Basics/Mutex.h"
#include "Utils/Cursor.h"
#include "VocBase/voc-types.h"

struct TRI_vocbase_t;

namespace arangodb {

namespace velocypack {
class Builder;
}

namespace aql {
struct QueryResult;
}

class CursorRepository {
 public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief create a cursors repository
  //////////////////////////////////////////////////////////////////////////////

  explicit CursorRepository(TRI_vocbase_t& vocbase);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief destroy a cursors repository
  //////////////////////////////////////////////////////////////////////////////

  ~CursorRepository();

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief stores a cursor in the registry
  /// the repository will take ownership of the cursor
  ////////////////////////////////////////////////////////////////////////////////

  Cursor* addCursor(std::unique_ptr<Cursor> cursor);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief creates a cursor and stores it in the registry
  /// the cursor will be returned with the usage flag set to true. it must be
  /// returned later using release()
  /// the cursor will take ownership and retain the entire QueryResult object
  //////////////////////////////////////////////////////////////////////////////

  Cursor* createFromQueryResult(aql::QueryResult&&, size_t, double, bool);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief creates a cursor and stores it in the registry
  /// the cursor will be returned with the usage flag set to true. it must be
  /// returned later using release()
  /// the cursor will create a query internally and retain it until deleted
  //////////////////////////////////////////////////////////////////////////////

  Cursor* createQueryStream(std::string const& query,
                            std::shared_ptr<velocypack::Builder> const& binds,
                            std::shared_ptr<velocypack::Builder> const& opts,
                            size_t batchSize, double ttl, bool contextOwnedByExterior);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief remove a cursor by id
  //////////////////////////////////////////////////////////////////////////////

  bool remove(CursorId, Cursor::CursorType);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief find an existing cursor by id
  /// if found, the cursor will be returned with the usage flag set to true.
  /// it must be returned later using release()
  //////////////////////////////////////////////////////////////////////////////

  Cursor* find(CursorId, Cursor::CursorType, bool&);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief return a cursor
  //////////////////////////////////////////////////////////////////////////////

  void release(Cursor*);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief whether or not the repository contains a used cursor
  //////////////////////////////////////////////////////////////////////////////

  bool containsUsedCursor();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief run a garbage collection on the cursors
  /// @return
  //////////////////////////////////////////////////////////////////////////////

  bool garbageCollect(bool);

 private:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief vocbase
  //////////////////////////////////////////////////////////////////////////////

  TRI_vocbase_t& _vocbase;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief mutex for the cursors repository
  //////////////////////////////////////////////////////////////////////////////

  Mutex _lock;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief list of current cursors
  //////////////////////////////////////////////////////////////////////////////

  std::unordered_map<CursorId, std::pair<Cursor*, std::string>> _cursors;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief maximum number of cursors to garbage-collect in one go
  //////////////////////////////////////////////////////////////////////////////

  static size_t const MaxCollectCount;
};

}  // namespace arangodb

#endif
