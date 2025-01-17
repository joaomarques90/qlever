// Copyright 2018, University of Freiburg,
// Chair of Algorithms and Data Structures.
// Author: Johannes Kalmbach(joka921) <johannes.kalmbach@gmail.com>
//

#pragma once

#include <gtest/gtest.h>
#include <sys/mman.h>

#include <codecvt>
#include <exception>
#include <future>
#include <locale>
#include <string_view>

#include "../global/Constants.h"
#include "../index/ConstantsIndexBuilding.h"
#include "../util/Exception.h"
#include "../util/File.h"
#include "../util/HashMap.h"
#include "../util/Log.h"
#include "../util/TaskQueue.h"
#include "./Tokenizer.h"
#include "./TokenizerCtre.h"
#include "ParallelBuffer.h"

using std::string;

/**
 * @brief The actual parser class
 *
 * If TokenizerCtre is used as a Tokenizer, a relaxed parsing mode is applied,
 * that does not quite fulfill the SPARQL standard. This means that:
 * - IRIS  of any kind (prefixed or not) must be limited to the ascii range
 * - Prefixed names (like prefix:suffix) may not include escape sequences
 *
 * These relaxations currently allow for fast parsing of Wikidata but might fail
 * on other knowledge bases, so this should be used with caution.
 * @tparam Tokenizer_T
 */
template <class Tokenizer_T>
class TurtleParser {
 public:
  class ParseException : public std::exception {
   public:
    ParseException() = default;
    ParseException(std::string_view msg, const size_t pos)
        : _msg{std::string(msg) + " at position " + std::to_string(pos)} {}
    const char* what() const throw() { return _msg.c_str(); }

   private:
    string _msg = "Error while parsing Turtle";
  };

  // The CTRE Tokenizer implies relaxed parsing.
  static constexpr bool UseRelaxedParsing =
      std::is_same_v<Tokenizer_T, TokenizerCtre>;

  virtual ~TurtleParser() = default;
  TurtleParser() = default;
  TurtleParser(TurtleParser&& rhs) = default;
  TurtleParser& operator=(TurtleParser&& rhs) = default;

  // Wrapper to getLine that is expected by the rest of QLever
  bool getLine(std::array<string, 3>& triple) { return getLine(&triple); }

  // Main access method to the parser
  // If a triple can be parsed (or has previously been parsed and stored
  // Writes the triple to the argument (format subject, object predicate)
  // returns true iff a triple can be successfully written, else the triple
  // value is invalid and the parser is at the end of the input.
  virtual bool getLine(std::array<string, 3>* triple) = 0;

  // Get the offset (relative to the beginning of the file) of the first byte
  // that has not yet been dealt with by the parser.
  virtual size_t getParsePosition() const = 0;

 protected:
  // clear all the parser's state to the initial values.
  void clear() {
    _lastParseResult.clear();

    _activeSubject.clear();
    _activePredicate.clear();
    _activePrefix.clear();

    _prefixMap.clear();

    _tok.reset(nullptr, 0);
    _triples.clear();
    _numBlankNodes = 0;
    _isParserExhausted = false;
  }

  // the following functions refer to the nonterminals of the turtle grammar
  // a return value of true means that the nonterminal could be parsed and that
  // the parser's internal state has been updated accordingly.
  // A return value of false means that the nonterminal could NOT be parsed
  // but that nothing has been changed in the parser's state (the LL1 lookup has
  // failed in this case). In all other cases a ParseException is thrown because
  // the LL1 property was violated.
  void turtleDoc() {
    while (statement()) {
    }
  }

  bool statement();
  /* Data Members */

  // Stores the triples that have been parsed but not retrieved yet.
  std::vector<std::array<string, 3>> _triples;

  // if this is set, there is nothing else to parse and we will only
  // retrieve what is left in our tripleBuffer;
  bool _isParserExhausted = false;

  // The tokenizer
  Tokenizer_T _tok{std::string_view("")};

  // the result of the last succesful call to a parsing function
  // (a function named after a (non-)terminal of the Turtle grammar
  std::string _lastParseResult;

  // maps prefixes to their expanded form, initialized with the empty base
  // (i.e. the prefix ":" maps to the empty IRI)
  ad_utility::HashMap<std::string, std::string> _prefixMap{{"", ""}};

  // there are turtle constructs that reuse prefixes, subjects and predicates
  // so we have to save the last seen ones
  std::string _activePrefix;
  std::string _activeSubject;
  std::string _activePredicate;
  size_t _numBlankNodes = 0;

  // throw an exception annotated with position information
  [[noreturn]] void raise(std::string_view msg) {
    auto d = _tok.view();
    if (!d.empty()) {
      LOG(ERROR) << "Parsing error has occured, showing next 1000 bytes:\n";
      auto s = std::min(size_t(10000), size_t(d.size()));
      LOG(ERROR) << std::string_view(d.data(), s) << std::endl;
    }
    throw ParseException(msg, getParsePosition());
  }

 protected:
  /* private Member Functions */

  bool directive();
  bool prefixID();
  bool base();
  bool sparqlPrefix();
  bool sparqlBase();
  bool triples();
  bool subject();
  bool predicateObjectList();
  bool blankNodePropertyList();
  bool objectList();
  bool object();
  bool verb();
  bool predicateSpecialA();
  bool predicate();

  bool iri();
  bool blankNode();
  bool collection();
  bool literal();
  bool rdfLiteral();
  bool numericLiteral();
  bool booleanLiteral();
  bool prefixedName();
  bool stringParse();

  // Terminal symbols from the grammar
  // Behavior of the functions is similar to the nonterminals (see above)
  bool iriref();
  bool integer() { return parseTerminal<TurtleTokenId::Integer>(); }
  bool decimal() { return parseTerminal<TurtleTokenId::Decimal>(); }
  bool doubleParse() { return parseTerminal<TurtleTokenId::Double>(); }

  /// this version only works if no escape sequences were used.
  bool pnameLnRelaxed();

  // __________________________________________________________________________
  bool pnameNS();

  // __________________________________________________________________________
  bool langtag() { return parseTerminal<TurtleTokenId::Langtag>(); }
  bool blankNodeLabel();

  bool anon() {
    if (!parseTerminal<TurtleTokenId::Anon>()) {
      return false;
    }
    _lastParseResult = createAnonNode();
    return true;
  }

  // Skip a given regex without parsing it
  template <TurtleTokenId reg>
  bool skip() {
    _tok.skipWhitespaceAndComments();
    return _tok.template skip<reg>();
  }

  // if the prefix of the current input position matches the regex argument,
  // put the matching prefix into _lastParseResult, move the input position
  // forward by the length of the match and return true else return false and do
  // not change the parser's state
  template <TurtleTokenId terminal, bool SkipWhitespaceBefore = true>
  bool parseTerminal();

  // ______________________________________________________________________________________
  void emitTriple() {
    _triples.push_back({_activeSubject, _activePredicate, _lastParseResult});
  }

  // enforce that the argument is true: if it is false, a parse Exception is
  // thrown this helps formulating the LL1 property in easily readable code
  bool check(bool result) {
    if (result) {
      return true;
    } else {
      auto view = _tok.view();
      auto s = std::min(size_t(1000), size_t(view.size()));
      auto nextChars = std::string_view(view.data(), s);
      raise(
          "A check for a required Element failed. Next unparsed characters are:\n"s +
          nextChars);
    }
  }

  // map a turtle prefix to its expanded form. Throws if the prefix was not
  // properly registered before
  string expandPrefix(const string& prefix) {
    if (!_prefixMap.count(prefix)) {
      raise("Prefix " + prefix +
            " was not defined using a PREFIX or @prefix "
            "declaration before using it!");
    } else {
      return _prefixMap[prefix];
    }
  }

  // create a new, unused, unique blank node string
  string createAnonNode() {
    string res = ANON_NODE_PREFIX + ":" + std::to_string(_numBlankNodes);
    _numBlankNodes++;
    return res;
  }

  FRIEND_TEST(TurtleParserTest, prefixedName);
  FRIEND_TEST(TurtleParserTest, prefixID);
  FRIEND_TEST(TurtleParserTest, stringParse);
  FRIEND_TEST(TurtleParserTest, rdfLiteral);
  FRIEND_TEST(TurtleParserTest, predicateObjectList);
  FRIEND_TEST(TurtleParserTest, objectList);
  FRIEND_TEST(TurtleParserTest, object);
  FRIEND_TEST(TurtleParserTest, blankNode);
  FRIEND_TEST(TurtleParserTest, blankNodePropertyList);
  FRIEND_TEST(TurtleParserTest, numericLiteral);
  FRIEND_TEST(TurtleParserTest, booleanLiteral);
};

/**
 * Parses turtle from std::string. Used to perform unit tests for
 * the different parser rules
 */
template <class Tokenizer_T>
class TurtleStringParser : public TurtleParser<Tokenizer_T> {
 public:
  using TurtleParser<Tokenizer_T>::_prefixMap;
  using TurtleParser<Tokenizer_T>::getLine;
  bool getLine(std::array<string, 3>* triple) override {
    (void)triple;
    throw std::runtime_error(
        "TurtleStringParser doesn't support calls to getLine. Only use "
        "parseUtf8String() for unit tests\n");
  }

  size_t getParsePosition() const override {
    return _positionOffset + _tmpToParse.size() - this->_tok.data().size();
  }

  void initialize(const string& filename) {
    (void)filename;
    throw std::runtime_error(
        "TurtleStringParser doesn't support calls to initialize. Only use "
        "parseUtf8String() for unit tests\n");
  }

  // load a string object directly to the buffer
  // allows easier testing without a file object
  void parseUtf8String(const std::string& toParse) {
    setInputStream(toParse);
    this->turtleDoc();
  }

  // Parse all Triples (no prefix declarations etc allowed) and return them.
  auto parseAndReturnAllTriples() {
    // Actually parse
    this->turtleDoc();
    auto d = this->_tok.view();
    if (!d.empty()) {
      LOG(INFO) << "Warning at position " << getParsePosition()
                << ":Parsing of line has Failed, but parseInput is not "
                   "yet exhausted. Remaining bytes: "
                << d.size() << '\n';
      auto s = std::min(size_t(1000), size_t(d.size()));
      LOG(INFO) << "Logging first 1000 unparsed characters\n";
      LOG(INFO) << std::string_view(d.data(), s) << std::endl;
    }
    return std::move(this->_triples);
  }

  string_view getUnparsedRemainder() const { return this->_tok.view(); }

  // Parse directive and return true if a directive was found.
  bool parseDirectiveManually() { return this->directive(); }

  void raiseManually(string_view message) { this->raise(message); }

  void setPositionOffset(size_t offset) { _positionOffset = offset; }

 private:
  // The complete input to this parser.
  std::vector<char> _tmpToParse;
  // used to add a certain offset to the parsing position when using this
  // in a parallel setting
  size_t _positionOffset = 0;

 public:
  // testing interface for reusing a parser
  // only specifies the tokenizers input stream.
  // Does not alter the tokenizers state
  void setInputStream(const string& toParse) {
    _tmpToParse.clear();
    _tmpToParse.reserve(toParse.size());
    _tmpToParse.insert(_tmpToParse.end(), toParse.begin(), toParse.end());
    this->_tok.reset(_tmpToParse.data(), _tmpToParse.size());
  }

  void setPrefixMap(decltype(_prefixMap) m) { _prefixMap = std::move(m); }

  const auto& getPrefixMap() const { return _prefixMap; }

  // __________________________________________________________
  void setInputStream(std::vector<char> toParse) {
    _tmpToParse = std::move(toParse);
    this->_tok.reset(_tmpToParse.data(), _tmpToParse.size());
  }

  // testing interface, only works when parsing from an utf8-string
  // return the current position of the tokenizer in the input string
  // can be used to test if the advancing of the tokenizer works
  // as expected
  size_t getPosition() const { return this->_tok.begin() - _tmpToParse.data(); }

  FRIEND_TEST(TurtleParserTest, prefixedName);
  FRIEND_TEST(TurtleParserTest, prefixID);
  FRIEND_TEST(TurtleParserTest, stringParse);
  FRIEND_TEST(TurtleParserTest, rdfLiteral);
  FRIEND_TEST(TurtleParserTest, predicateObjectList);
  FRIEND_TEST(TurtleParserTest, objectList);
  FRIEND_TEST(TurtleParserTest, object);
  FRIEND_TEST(TurtleParserTest, blankNode);
  FRIEND_TEST(TurtleParserTest, blankNodePropertyList);
};

/**
 * This class is a TurtleParser that always assumes that
 * its input file is an uncompressed .ttl file that will be read in
 * chunks. Input file can also be a stream like stdin.
 */
template <class Tokenizer_T>
class TurtleStreamParser : public TurtleParser<Tokenizer_T> {
  // struct that can store the state of a parser
  // the previously extracted triples are not stored
  // but only the number of triples that were already present
  // before the backup
  struct TurtleParserBackupState {
    size_t _numBlankNodes = 0;
    size_t _numTriples;
    const char* _tokenizerPosition;
    size_t _tokenizerSize;
  };

 public:
  // Default construction needed for tests
  TurtleStreamParser() = default;
  explicit TurtleStreamParser(const string& filename) {
    LOG(DEBUG) << "Initialize turtle parsing from uncompressed file or stream "
               << filename << std::endl;
    initialize(filename);
  }

  // inherit the wrapper overload
  using TurtleParser<Tokenizer_T>::getLine;

  bool getLine(std::array<string, 3>* triple) override;

  void initialize(const string& filename);

  size_t getParsePosition() const override {
    return _numBytesBeforeCurrentBatch + (_tok.data().data() - _byteVec.data());
  }

 private:
  using TurtleParser<Tokenizer_T>::_tok;
  using TurtleParser<Tokenizer_T>::_triples;
  using TurtleParser<Tokenizer_T>::_isParserExhausted;
  // Backup the current state of the turtle parser to a
  // TurtleparserBackupState object
  // This can be used e.g. when parsing from a compressed input
  // and the currently uncompressed buffer is not sufficient to parse the
  // next expression
  TurtleParserBackupState backupState() const;

  // Reset the parser to the state indicated by the argument
  // Must be called on the same parser object that was used to create the backup
  // state (the actual triples are not backed up)
  bool resetStateAndRead(TurtleParserBackupState* state);

  // stores the current batch of bytes we have to parse.
  // Might end in the middle of a statement or even a multibyte utf8 character,
  // that's why we need the backupState() and resetStateAndRead() methods
  std::vector<char> _byteVec;

  std::unique_ptr<ParallelBuffer> _fileBuffer;
  // this many characters will be buffered at once,
  // defaults to a global constant
  size_t _bufferSize = FILE_BUFFER_SIZE;

  // that many bytes were already parsed before dealing with the current batch
  // in member _byteVec
  size_t _numBytesBeforeCurrentBatch = 0;
};

/**
 * This class is a TurtleParser
 * reads from an uncompressed .ttl file, that has to lie in memory.
 * It will be loaded at once using Mmap. Thus this class does not support
 * Parsing from streams like stdin
 */
template <class Tokenizer_T>
class TurtleMmapParser : public TurtleParser<Tokenizer_T> {
 public:
  explicit TurtleMmapParser(const string& filename) {
    LOG(DEBUG) << "Initialize turtle parsing from uncompressed file "
               << filename << std::endl;
    LOG(DEBUG) << "This must be a file on disk since it will be loaded "
                  "using the mmap system call"
               << std::endl;
    initialize(filename);
  }

  ~TurtleMmapParser() { unmapFile(); }
  TurtleMmapParser(TurtleMmapParser&& rhs) = default;
  TurtleMmapParser& operator=(TurtleMmapParser&& rhs) = default;

  size_t getParsePosition() const override {
    return _dataSize - _tok.data().size();
  }

  // inherit the other overload
  using TurtleParser<Tokenizer_T>::getLine;
  // overload of the actual mmap-based parsing logic
  bool getLine(std::array<string, 3>* triple) override;

  // initialize parse input from a turtle file using mmap
  void initialize(const string& filename);

  // has to be called after finishing parsing of an mmaped .ttl file
  // if no file is currently mapped this function has no effect
  void unmapFile() {
    if (_data) {
      munmap(const_cast<char*>(_data), _dataSize);
      _data = nullptr;
      _dataSize = 0;
    }
  }

  // used when mapping a turtle file via Mmap
  const char* _data = nullptr;
  // store the size of the mapped input when using the _data ptr
  // via mmap
  size_t _dataSize = 0;
  using TurtleParser<Tokenizer_T>::_tok;
  using TurtleParser<Tokenizer_T>::_isParserExhausted;
  using TurtleParser<Tokenizer_T>::_triples;
};

/**
 * This class is a TurtleParser that always assumes that
 * its input file is an uncompressed .ttl file that will be read in
 * chunks. Input file can also be a stream like stdin.
 */
template <class Tokenizer_T>
class TurtleParallelParser : public TurtleParser<Tokenizer_T> {
 public:
  using Triple = std::array<string, 3>;
  // Default construction needed for tests
  TurtleParallelParser() = default;
  explicit TurtleParallelParser(const string& filename) {
    LOG(DEBUG)
        << "Initialize parallel Turtle Parsing from uncompressed file or "
           "stream "
        << filename << std::endl;
    initialize(filename);
  }

  // inherit the wrapper overload
  using TurtleParser<Tokenizer_T>::getLine;

  bool getLine(std::array<string, 3>* triple) override;

  std::optional<std::vector<Triple>> getBatch();

  void printAndResetQueueStatistics() {
    LOG(TIMING) << parallelParser.getTimeStatistics() << '\n';
    parallelParser.resetTimers();
    LOG(TIMING) << tripleCollector.getTimeStatistics() << '\n';
    tripleCollector.resetTimers();
  }

  void initialize(const string& filename);

  virtual size_t getParsePosition() const override {
    // TODO: can we really define this position here?
    return 0;
  }

 private:
  using TurtleParser<Tokenizer_T>::_tok;
  using TurtleParser<Tokenizer_T>::_triples;
  using TurtleParser<Tokenizer_T>::_isParserExhausted;

  // this many characters will be buffered at once,
  // defaults to a global constant
  size_t _bufferSize = FILE_BUFFER_SIZE;
  ParallelBufferWithEndRegex _fileBuffer{_bufferSize, "\\. *(\\n)"};

  ad_utility::TaskQueue<true> tripleCollector{QUEUE_SIZE_AFTER_PARALLEL_PARSING,
                                              0, "triple collector"};
  ad_utility::TaskQueue<true> parallelParser{QUEUE_SIZE_BEFORE_PARALLEL_PARSING,
                                             NUM_PARALLEL_PARSER_THREADS,
                                             "parallel parser"};
  std::future<void> _parseFuture;

  std::vector<char> _remainingBatchFromInitialization;
};
