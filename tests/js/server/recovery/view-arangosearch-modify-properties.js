/* jshint globalstrict:false, strict:false, unused : false */
/* global assertEqual, assertTrue, assertFalse, assertNull, fail, AQL_EXECUTE */

////////////////////////////////////////////////////////////////////////////////
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


var db = require('@arangodb').db;
var internal = require('internal');
var jsunity = require('jsunity');

function runSetup () {
  'use strict';
  internal.debugClearFailAt();

  db._drop('UnitTestsRecoveryDummy');
  var c = db._create('UnitTestsRecoveryDummy');

  db._dropView('UnitTestsRecoveryView');
  var view = db._createView('UnitTestsRecoveryView', 'arangosearch', { "cleanupIntervalStep": 42 });

  var meta = { links: { 'UnitTestsRecoveryDummy': { includeAllFields: true } } };

  for (let i = 0; i < 10000; i++) {
    c.save({ a: "foo_" + i, b: "bar_" + i, c: i });
  }

  view.properties(meta);

  meta = {
    consolidationIntervalMsec: 10000,
    consolidationPolicy: { segmentThreshold: 20, threshold: 0.5, type: "bytes" },
  };
  view.properties(meta, true); // partial update

  c.save({ name: 'crashme' }, { waitForSync: true });
  internal.debugSegfault('crashing server');
}

////////////////////////////////////////////////////////////////////////////////
/// @brief test suite
////////////////////////////////////////////////////////////////////////////////

function recoverySuite () {
  'use strict';
  jsunity.jsUnity.attachAssertions();

  return {
    setUp: function () {},
    tearDown: function () {},

    ////////////////////////////////////////////////////////////////////////////////
    /// @brief test whether we can restore the trx data
    ////////////////////////////////////////////////////////////////////////////////

    testIResearchLinkPopulate: function () {
      var v = db._view('UnitTestsRecoveryView');
      assertEqual(v.name(), 'UnitTestsRecoveryView');
      assertEqual(v.type(), 'arangosearch');
      var p = v.properties().links;
      assertTrue(p.hasOwnProperty('UnitTestsRecoveryDummy'));
      assertTrue(p.UnitTestsRecoveryDummy.includeAllFields);

      var result = AQL_EXECUTE("FOR doc IN UnitTestsRecoveryView SEARCH doc.c >= 0 OPTIONS {waitForSync: true} COLLECT WITH COUNT INTO length RETURN length").json;
      assertEqual(result[0], 10000);

      // validate state
      var properties = v.properties();
      assertTrue(Object === properties.constructor);
      assertEqual(42, properties.cleanupIntervalStep);
      assertEqual(10000, properties.consolidationIntervalMsec);
      assertEqual(3, Object.keys(properties.consolidationPolicy).length);
      assertEqual("bytes", properties.consolidationPolicy.type);
      assertEqual(20, properties.consolidationPolicy.segmentThreshold);
      assertEqual((0.5).toFixed(6), properties.consolidationPolicy.threshold.toFixed(6));
    }

  };
}

////////////////////////////////////////////////////////////////////////////////
/// @brief executes the test suite
////////////////////////////////////////////////////////////////////////////////

function main (argv) {
  'use strict';
  if (argv[1] === 'setup') {
    runSetup();
    return 0;
  } else {
    jsunity.run(recoverySuite);
    return jsunity.done().status ? 0 : 1;
  }
}