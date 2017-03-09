// Copyright 2015, University of Freiburg,
// Chair of Algorithms and Data Structures.
// Author: Björn Buchhold (buchhold@informatik.uni-freiburg.de)
#pragma once

#include <string>
#include "./Operation.h"
#include "../util/Conversions.h"

using std::string;

class IndexScan : public Operation {
public:
  enum ScanType {
    PSO_BOUND_S = 0,
    POS_BOUND_O = 1,
    PSO_FREE_S = 2,
    POS_FREE_O = 3,
    SPO_FREE_P = 4,
    SOP_BOUND_O = 5,
    SOP_FREE_O = 6,
    OPS_FREE_P = 7,
    OSP_FREE_S = 8,
    FULL_INDEX_SCAN_SPO = 9,
    FULL_INDEX_SCAN_SOP = 10,
    FULL_INDEX_SCAN_PSO = 11,
    FULL_INDEX_SCAN_POS = 12,
    FULL_INDEX_SCAN_OSP = 13,
    FULL_INDEX_SCAN_OPS = 14
  };

  virtual string asString() const;

  IndexScan(QueryExecutionContext* qec, ScanType type) :
      Operation(qec), _type(type),
      _sizeEstimate(std::numeric_limits<size_t>::max()) {
  }

  virtual ~IndexScan() {}

  void setSubject(const string& subject) {
    _subject = subject;
  }

  void setPredicate(const string& predicate) {
    _predicate = predicate;
  }

  void setObject(const string& object) {
    if (!ad_utility::isXsdValue(object)) {
      _object = object;
    } else {
      _object = ad_utility::convertValueLiteralToIndexWord(object);
    }
  }

  virtual size_t getResultWidth() const;

  virtual size_t resultSortedOn() const { return 0; }

  virtual void setTextLimit(size_t) {
    // Do nothing.
  }

  virtual size_t getSizeEstimate() {
    if (_sizeEstimate == std::numeric_limits<size_t>::max()) {
      _sizeEstimate = computeSizeEstimate();
    }
    return _sizeEstimate;
  }

  virtual size_t getCostEstimate() {
    return getSizeEstimate();
  }

  void determineMultiplicities();

  virtual float getMultiplicity(size_t col) {
    if (_multiplicity.size() == 0) {
      determineMultiplicities();
    }
    assert(col < _multiplicity.size());
    return _multiplicity[col];
  }

  void precomputeSizeEstimate() {
    _sizeEstimate = computeSizeEstimate();
  }

  virtual bool knownEmptyResult() {
    return getSizeEstimate() == 0;
  }

protected:
  ScanType _type;
  string _subject;
  string _predicate;
  string _object;
  size_t _sizeEstimate;
  vector<float> _multiplicity;

  virtual void computeResult(ResultTable* result) const;

  void computePSOboundS(ResultTable* result) const;

  void computePSOfreeS(ResultTable* result) const;

  void computePOSboundO(ResultTable* result) const;

  void computePOSfreeO(ResultTable* result) const;

  void computeSPOfreeP(ResultTable* result) const;

  void computeSOPboundO(ResultTable* result) const;

  void computeSOPfreeO(ResultTable* result) const;

  void computeOPSfreeP(ResultTable* result) const;

  void computeOSPfreeS(ResultTable* result) const;

  size_t computeSizeEstimate() const;
};

