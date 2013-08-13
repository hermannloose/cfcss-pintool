/**
 * @author Hermann Loose <hermannloose@gmail.com>
 *
 * TODO(hermannloose): Add description.
 */

#include "pin.H"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <list>
#include <random>
#include <set>


std::ostream * out = &cerr;

default_random_engine *generator;
uniform_int_distribution<unsigned int> *distribution;

static string red = "\x1b[31;1m";
static string green = "\x1b[32;1m";
static string reset = "\x1b[0m";

set<ADDRINT> *addressesToInstrument = new set<ADDRINT>();
set<ADDRINT> *addressesToIgnore = new set<ADDRINT>();

enum INSTRUMENT_ACTION {
  NONE,
  DELETE,
  INSERT_BRANCH,
  BRANCH_GARBLE
};

INSTRUMENT_ACTION instrumentAction = NONE;

list<pair<ADDRINT, ADDRINT>> *segments;

KNOB<unsigned int> KnobRandomSeed(
    KNOB_MODE_WRITEONCE,
    "pintool",
    "seed",
    "0",
    "PRNG seed, use to reproduce runs of the tool that involved random bit flips."
    "A given seed of zero is ignored and std::random_device is used instead.");

KNOB<string> KnobOutputFile(
    KNOB_MODE_WRITEONCE,
    "pintool",
    "o",
    "",
    "write output to file");

KNOB<string> KnobAddressFiles(
    KNOB_MODE_APPEND,
    "pintool",
    "address-file",
    "",
    "file containing a list of addresses to instrument at, one per line\n"
    "(can be specified multiple times)");

KNOB<string> KnobIgnoreFiles(
    KNOB_MODE_APPEND,
    "pintool",
    "ignore-file",
    "",
    "file containing a list of addresses to ignore, one per line\n"
    "(can be specified multiple times)");

KNOB<string> KnobInstrumentAction(
    KNOB_MODE_WRITEONCE,
    "pintool",
    "instrument-action",
    "delete",
    "one of (delete|insert-branch|branch-garble)");

KNOB<BOOL> KnobLimitJumpsToExecutableSections(
    KNOB_MODE_WRITEONCE,
    "pintool",
    "avoid-segfaults",
    "1",
    "whether to limit inserted or modified branches to targets within executable sections");

INT32 Usage() {
  cerr << KNOB_BASE::StringKnobSummary() << endl;

  return -1;
}

BOOL HandleSignal(
    THREADID tid,
    INT32 signal,
    CONTEXT *context,
    BOOL hasHandler,
    const EXCEPTION_INFO *exceptionInfo,
    VOID *v) {

  switch (signal) {
    case 4:
      cerr << "CFCSS detected a control-flow violation! (SIGILL)" << endl;

      *out << "--- SIGILL ---" << endl;

      break;
    case 11:
      *out << "--- SIGSEGV ---" << endl;

      break;
    default:
      cerr << "Signal: " << signal << endl;

      *out << "--- Signal: " << signal << " ---" << endl;
  }

  ADDRINT eip = PIN_GetContextReg(context, REG_EIP);
  cerr << "EIP:    " << showbase << hex << eip << noshowbase << dec << endl;
  if (none_of(segments->begin(), segments->end(),
      [&] (pair<ADDRINT, ADDRINT> segment) {
        return segment.first <= eip && segment.second >= eip;
      })) {

    cerr << "(not within an executable section)" << endl;
  }


  // Pass the signal on to the application.
  return TRUE;
}

ADDRINT FlipBitInAddress(ADDRINT address, BOOL limitToExecutableSegments) {
  ADDRINT target;

  do {
    unsigned int position = (*distribution)(*generator);
    target = address ^ (1 << position);
  } while (limitToExecutableSegments && none_of(segments->begin(), segments->end(),
        [&] (pair<ADDRINT, ADDRINT> range) {
          return range.first <= target && range.second >= target;
        }));

  return target;
}

VOID InstrumentInstruction(INS instruction) {
  ADDRINT address = INS_Address(instruction);

  cerr << showbase << hex << address << noshowbase << dec << ": ";

  switch (instrumentAction) {
    case DELETE:
      {
        int width = cerr.width();
        cerr.width(8);
        cerr << "deleting \"" << INS_Disassemble(instruction) << "\"" << endl;
        cerr.width(width);

        *out << "del " << showbase << hex << address << noshowbase << dec << endl;

        INS_Delete(instruction);
      }
      break;
    case INSERT_BRANCH:
      {
        ADDRINT target = FlipBitInAddress(address, KnobLimitJumpsToExecutableSections.Value());

        cerr << "inserting direct jump to "
            << showbase << hex << target << noshowbase << dec << endl;

        *out << "ins " << showbase << hex << address << " " << target << noshowbase << dec << endl;

        INS_InsertDirectJump(instruction, IPOINT_BEFORE, target);
      }
      break;
    default:
      break;
  }
}

VOID HandleRoutine(RTN routine) {
  ADDRINT routineStart = RTN_Address(routine);
  ADDRINT routineEnd = routineStart + RTN_Size(routine);

  auto ai = addressesToInstrument->lower_bound(routineStart);
  auto ae = addressesToInstrument->upper_bound(routineEnd);
  auto end = addressesToInstrument->end();

  if (ai != ae && ai != end) {
    cerr << "[" << RTN_Name(routine) << "]" << endl;
    RTN_Open(routine);
    for (INS instruction = RTN_InsHead(routine); INS_Valid(instruction), ai != ae;
        instruction = INS_Next(instruction)) {

      ADDRINT address = INS_Address(instruction);
      if (address == *ai) {
        InstrumentInstruction(instruction);
        ++ai;
      }
    }
    RTN_Close(routine);
  }
}

VOID HandleExecutableSection(SEC section) {
  ADDRINT sectionStart = SEC_Address(section);
  ADDRINT sectionEnd = sectionStart + SEC_Size(section);

  segments->push_back(pair<ADDRINT, ADDRINT>(sectionStart, sectionEnd));

  auto ai = addressesToInstrument->lower_bound(sectionStart);
  auto ae = addressesToInstrument->upper_bound(sectionEnd);
  auto end = addressesToInstrument->end();

  if (ai != ae && ai != end) {
    cerr << "[" << SEC_Name(section) << "]" << endl;
    for (RTN routine = SEC_RtnHead(section); RTN_Valid(routine); routine = RTN_Next(routine)) {
      HandleRoutine(routine);
    }
  }
}

VOID ImageLoad(IMG image, VOID *v) {
  cerr << "Loading image [" << IMG_Name(image) << "] ..." << endl;

  for (SEC section = IMG_SecHead(image); SEC_Valid(section); section = SEC_Next(section)) {
    if (SEC_IsExecutable(section)) {
      HandleExecutableSection(section);
    }
  }
}

int main(int argc, char *argv[]) {
  // Initialize PIN library. Print help message if -h(elp) is specified
  // in the command line or the command line is invalid
  if (PIN_Init(argc, argv)) {
    return Usage();
  }

  PIN_InitSymbols();

  string fileName = KnobOutputFile.Value();
  if (!fileName.empty()) {
    out = new std::ofstream(fileName.c_str());
  }

  unsigned int seed = KnobRandomSeed.Value();
  if (!seed) {
    seed = random_device{}();
  }
  cerr << "Random seed: " << seed << endl;
  *out << "Random seed: " << seed << endl;

  generator = new default_random_engine(seed);
  distribution = new uniform_int_distribution<unsigned int>(0, 63);

  // Intercept SIGILL, raised by UD2 instruction in CFCSS, signalling
  // a signature mismatch.
  PIN_InterceptSignal(4, HandleSignal, 0);

  // Intercept SIGSEGV, since these will count as "detected" errors as well.
  PIN_InterceptSignal(11, HandleSignal, 0);

  int addressFilesCount = KnobAddressFiles.NumberOfValues();
  for (int i = 0; i < addressFilesCount; ++i) {
    string addressFileName = KnobAddressFiles.Value(i);
    if (!addressFileName.empty()) {
      ifstream addressFile(addressFileName);
      if (addressFile.is_open()) {
        ADDRINT address;
        addressFile >> hex;
        while (addressFile >> address) {
          addressesToInstrument->insert(address);
        }
      } else {
        cerr << "Could not open " << addressFileName << " for reading!" << endl;

        return -1;
      }
    }
  }

  int ignoreFilesCount = KnobIgnoreFiles.NumberOfValues();
  for (int i = 0; i < ignoreFilesCount; ++i) {
    string ignoreFileName = KnobIgnoreFiles.Value(i);
    if (!ignoreFileName.empty()) {
      ifstream ignoreFile(ignoreFileName);
      if (ignoreFile.is_open()) {
        ADDRINT address;
        ignoreFile >> hex;
        while (ignoreFile >> address) {
          addressesToIgnore->insert(address);
        }
      } else {
        cerr << "Could not open " << ignoreFileName << " for reading!" << endl;

        return -1;
      }
    }
  }

  set<ADDRINT> intersection;
  set_intersection(
      addressesToInstrument->begin(), addressesToInstrument->end(),
      addressesToIgnore->begin(), addressesToIgnore->end(),
      inserter(intersection, intersection.begin()));

  if (!intersection.empty()) {
    cerr << red << "The following addresses were requested to both be instrumented and ignored:"
        << reset << endl;

    cerr << showbase << hex;
    for (auto i = intersection.begin(), e = intersection.end(); i != e; ++i) {
      cerr << *i << endl;
    }
    cerr << noshowbase << dec;

    return -1;
  }

  string instrumentActionFlag = KnobInstrumentAction.Value();
  if (instrumentActionFlag == "delete") {
    instrumentAction = DELETE;
  }
  if (instrumentActionFlag == "insert-branch") {
    instrumentAction = INSERT_BRANCH;
  }

  segments = new list<pair<ADDRINT, ADDRINT>>();

  IMG_AddInstrumentFunction(ImageLoad, 0);

  cerr <<  "=================================================" << endl;
  cerr <<  "This application is instrumented by cfcss-pintool" << endl << endl;
  if (!fileName.empty()) {
      cerr << "See file " << endl << fileName << endl
          << "for a log of the modifications performed." << endl;
  }
  cerr <<  "=================================================" << endl;

  // Start the program, never returns.
  PIN_StartProgram();

  return 0;
}
