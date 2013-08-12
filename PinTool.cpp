/**
 * @author Hermann Loose <hermannloose@gmail.com>
 *
 * TODO(hermannloose): Add description.
 */

#include "pin.H"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <list>
#include <set>


std::ostream * out = &cerr;

static string red = "\x1b[31;1m";
static string green = "\x1b[32;1m";
static string reset = "\x1b[0m";

set<ADDRINT> *addressesToInstrument = new set<ADDRINT>();
set<ADDRINT> *addressesToIgnore = new set<ADDRINT>();


list<pair<ADDRINT, ADDRINT>> *segments;



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
    default:
      cerr << "Signal: " << signal << endl;

      *out << "--- Signal: " << signal << " ---" << endl;
  }

  cerr << "EIP:    " << showbase << hex << PIN_GetContextReg(context, REG_EIP)
      << noshowbase << dec << endl;

  // Pass the signal on to the application.
  return TRUE;
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

  // Intercept SIGILL, raised by UD2 instruction in CFCSS, signalling
  // a signature mismatch.
  PIN_InterceptSignal(4, HandleSignal, 0);

  // Intercept SIGSEGV, since these will count as "detected" errors as well.
  PIN_InterceptSignal(11, HandleSignal, 0);

  addressesToInstrument = new set<ADDRINT>();

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

  addressesToIgnore = new set<ADDRINT>();

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


  // Start the program, never returns.
  PIN_StartProgram();

  return 0;
}
